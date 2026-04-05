// query_runner.c
// Build: gcc -O2 -o query_runner query_runner.c
// Usage examples:
//   Run all queries in default "queries" dir:
//     ./query_runner
//   Run only queries matching substring "APX1090":
//     QUERY_FILTER=APX1090 ./query_runner
//   Point to a different queries directory and log file:
//     QUERY_DIR=/path/to/queries LOG_FILE=/tmp/q.log QUERY_FILTER=all ./query_runner

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>
#include <strings.h>
#include "rapl.h"

#define MAX_FILES 4096
#define MAX_CMD 4096
#define DEFAULT_QUERY_DIR "queries"
#define DEFAULT_LOG_FILE "query_timing.csv"
#define RUNS 100
#define LOOPS 1
#define SET_PAUSE_SEC 15
#define DEFAULT_SIGLESS_ADDR "127.0.0.1:8000"
#define DEFAULT_SIGLESS_CHANNEL "CH1"
#define DEFAULT_SUDO_PASSWORD "a"
#define DEFAULT_PERF_OUTPUT_DIR "logs/perf_runs"
#define DEFAULT_PERF_EVENTS "cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations,page-faults"
#define DEFAULT_CAPTURE_OUTPUT_DIR "logs/query_outputs"
#define DEFAULT_CAPTURE_SQL_DIR "logs/rewritten_queries"
#define DEFAULT_CAPTURE_SIZE_LOG_FILE "logs/query_output_sizes.csv"

static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void csv_write_field(FILE *fp, const char *value) {
    const char *p = value ? value : "";
    fputc('"', fp);
    while (*p) {
        if (*p == '"') {
            fputc('"', fp);
        }
        fputc(*p, fp);
        p++;
    }
    fputc('"', fp);
}

static void utc_timestamp_now(char *buf, size_t buf_size) {
    time_t now = time(NULL);
    struct tm tm_utc;
    if (gmtime_r(&now, &tm_utc) == NULL) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return;
    }
    strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static void generate_run_id(char *buf, size_t buf_size) {
    if (buf_size < 17) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return;
    }

    unsigned char raw[8];
    int use_fallback = 0;
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (!urandom) {
        use_fallback = 1;
    } else {
        size_t got = fread(raw, 1, sizeof(raw), urandom);
        fclose(urandom);
        if (got != sizeof(raw)) {
            use_fallback = 1;
        }
    }

    if (use_fallback) {
        uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
        for (size_t i = 0; i < sizeof(raw); i++) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            raw[i] = (unsigned char)(seed & 0xFF);
        }
    }

    snprintf(buf, buf_size,
             "%02X%02X%02X%02X%02X%02X%02X%02X",
             raw[0], raw[1], raw[2], raw[3],
             raw[4], raw[5], raw[6], raw[7]);
}

/* Prime sudo credentials once before timed runs so password prompts don't skew loop timing. */
static int prime_sudo_credentials(const char *password) {
    char cmd[512];
    const char *pw = (password && *password) ? password : DEFAULT_SUDO_PASSWORD;
    int n = snprintf(cmd, sizeof(cmd),
                     "printf '%%s\\n' '%s' | sudo -S -v >/dev/null 2>&1",
                     pw);
    if (n <= 0 || n >= (int)sizeof(cmd)) {
        return -1;
    }
    return system(cmd);
}

/* helper to call the script; drop the script into the same directory or adjust path */
static void post_sigless_script(const char *remote, const char *channel, const char *msg) {
    char cmd[2048];
    int n = snprintf(cmd, sizeof(cmd),
                     "sh ./post_to_sigless.sh %s %s \"%s\" >/dev/null 2>&1",
                     remote, channel, msg);
    if (n > 0 && n < (int)sizeof(cmd)) {
        int rc = system(cmd);
        if (rc == -1) {
            perror("system post_to_sigless.sh");
        }
    }
}

/* Simple comparator for qsort */
static int cmp_queries(const void *a, const void *b) {
    const char * const *pa = a;
    const char * const *pb = b;
    return strcmp(*pa, *pb);
}

/* Return 1 if filename matches either format:
   - APX<digits>-query<...>.sql
   - PE<digits>-query<...>.sql
   (case-sensitive)
*/
static int valid_query_name(const char *name) {
    size_t len = strlen(name);
    const char *suffix = ".sql";
    size_t suffix_len = strlen(suffix);

    if (len <= suffix_len) return 0;
    if (strcmp(name + len - suffix_len, suffix) != 0) return 0; // must end with .sql

    // find "-query" occurrence
    const char *dash = strchr(name, '-');
    if (!dash) return 0;
    if (strncmp(dash, "-query", 6) != 0) return 0;

    // check prefix: starts with "APX" or "PE", then at least one digit
    if (strncmp(name, "APX", 3) == 0) {
        const char *p = name + 3;
        if (*p < '0' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') p++;
        // expect hyphen next (we already checked there's a dash somewhere)
        if (*p != '-') return 0;
        return 1;
    } else if (strncmp(name, "PE", 2) == 0) {
        const char *p = name + 2;
        if (*p < '0' || *p > '9') return 0;
        while (*p >= '0' && *p <= '9') p++;
        if (*p != '-') return 0;
        return 1;
    }

    return 0;
}

static int env_is_true(const char *value) {
    if (!value || !*value) {
        return 0;
    }
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0 ||
           strcmp(value, "on") == 0 ||
           strcmp(value, "ON") == 0;
}

static int ensure_dir_exists(const char *dir_path) {
    char tmp[PATH_MAX];
    size_t len;

    if (!dir_path || !*dir_path) {
        return -1;
    }

    len = strlen(dir_path);
    if (len >= sizeof(tmp)) {
        return -1;
    }

    memcpy(tmp, dir_path, len + 1);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static void sanitize_for_filename(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    if (!dst_size) return;

    for (size_t i = 0; src && src[i] && j + 1 < dst_size; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            dst[j++] = c;
        } else {
            dst[j++] = '_';
        }
    }
    dst[j] = '\0';
}

static int write_perf_metadata_file(const char *perf_output_dir,
                                    const char *run_id,
                                    const char *run_started_utc,
                                    const char *query_dir,
                                    const char *query_filter,
                                    const char *perf_events) {
    char path[PATH_MAX];
    FILE *fp;

    int n = snprintf(path, sizeof(path), "%s/%s__perf_metadata.txt", perf_output_dir, run_id);
    if (n <= 0 || n >= (int)sizeof(path)) {
        return -1;
    }

    fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }

    fprintf(fp, "run_id=%s\n", run_id);
    fprintf(fp, "run_started_utc=%s\n", run_started_utc);
    fprintf(fp, "query_dir=%s\n", query_dir ? query_dir : "");
    fprintf(fp, "query_filter=%s\n", query_filter ? query_filter : "all");
    fprintf(fp, "perf_events=%s\n", perf_events ? perf_events : "");

    fclose(fp);
    return 0;
}

static int token_matches_ci(const char *s, const char *token) {
    size_t n = strlen(token);
    if (strncasecmp(s, token, n) != 0) {
        return 0;
    }
    char next = s[n];
    if ((next >= 'a' && next <= 'z') ||
        (next >= 'A' && next <= 'Z') ||
        (next >= '0' && next <= '9') ||
        next == '_') {
        return 0;
    }
    return 1;
}

static char *skip_sql_leading_noise(char *s) {
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
            s++;
        }

        if (s[0] == '-' && s[1] == '-') {
            s += 2;
            while (*s && *s != '\n') {
                s++;
            }
            continue;
        }

        if (s[0] == '/' && s[1] == '*') {
            s += 2;
            while (*s && !(s[0] == '*' && s[1] == '/')) {
                s++;
            }
            if (*s) {
                s += 2;
            }
            continue;
        }

        break;
    }

    return s;
}

static int rewrite_query_without_explain_analyze(const char *query_name,
                                                 const char *src_path,
                                                 const char *dst_path) {
    FILE *in = NULL;
    FILE *out = NULL;
    char *buf = NULL;
    long size = 0;
    int rc = -1;

    /* Capture-mode override: make cursor query emit rows via NOTICE output. */
    if (query_name && strcmp(query_name, "APX1186-queryA.sql") == 0) {
        static const char *override_sql =
            "DO $$\n"
            "DECLARE\n"
            "    rec RECORD;\n"
            "    cur_lineitems CURSOR FOR\n"
            "        SELECT l_orderkey, l_extendedprice\n"
            "        FROM lineitem\n"
            "        WHERE l_extendedprice > 100000;\n"
            "BEGIN\n"
            "    OPEN cur_lineitems;\n"
            "    LOOP\n"
            "        FETCH cur_lineitems INTO rec;\n"
            "        EXIT WHEN NOT FOUND;\n"
            "\n"
            "        RAISE NOTICE 'Order: %, Price: %', rec.l_orderkey, rec.l_extendedprice;\n"
            "    END LOOP;\n"
            "    CLOSE cur_lineitems;\n"
            "END $$;\n";

        out = fopen(dst_path, "wb");
        if (!out) {
            return -1;
        }
        if (fputs(override_sql, out) == EOF) {
            fclose(out);
            return -1;
        }
        fclose(out);
        return 0;
    }

    in = fopen(src_path, "rb");
    if (!in) {
        return -1;
    }

    if (fseek(in, 0, SEEK_END) != 0) {
        goto cleanup;
    }
    size = ftell(in);
    if (size < 0) {
        goto cleanup;
    }
    if (fseek(in, 0, SEEK_SET) != 0) {
        goto cleanup;
    }

    buf = malloc((size_t)size + 1);
    if (!buf) {
        goto cleanup;
    }

    if (size > 0) {
        size_t got = fread(buf, 1, (size_t)size, in);
        if (got != (size_t)size) {
            goto cleanup;
        }
    }
    buf[size] = '\0';

    char *stmt = skip_sql_leading_noise(buf);
    if (token_matches_ci(stmt, "EXPLAIN")) {
        char *p = stmt + strlen("EXPLAIN");
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (token_matches_ci(p, "ANALYZE")) {
            p += strlen("ANALYZE");
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
                p++;
            }
            stmt = p;
        }
    }

    out = fopen(dst_path, "wb");
    if (!out) {
        goto cleanup;
    }

    if (fputs(stmt, out) == EOF) {
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (in) fclose(in);
    if (out) fclose(out);
    free(buf);
    return rc;
}

static long long file_size_bytes(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (long long)st.st_size;
}

int main(void) {
    const char *query_dir = getenv("QUERY_DIR");
    if (!query_dir) query_dir = DEFAULT_QUERY_DIR;

    const char *log_file = getenv("LOG_FILE");
    if (!log_file) log_file = DEFAULT_LOG_FILE;

    const char *sigless_addr = DEFAULT_SIGLESS_ADDR;
    const char *sigless_channel = DEFAULT_SIGLESS_CHANNEL;
    const char *sudo_password = getenv("SUDO_PASSWORD");
    const char *perf_enable_env = getenv("PERF_ENABLE");
    const char *perf_output_dir = getenv("PERF_OUTPUT_DIR");
    const char *perf_events = getenv("PERF_EVENTS");
    const char *capture_enable_env = getenv("OUTPUT_CAPTURE_ENABLE");
    const char *capture_output_dir = getenv("OUTPUT_CAPTURE_DIR");
    const char *capture_sql_dir = getenv("OUTPUT_CAPTURE_SQL_DIR");
    const char *capture_size_log_file = getenv("OUTPUT_CAPTURE_LOG_FILE");
    int perf_enabled = env_is_true(perf_enable_env);
    int capture_enabled = env_is_true(capture_enable_env);

    if (capture_enabled && perf_enabled) {
        fprintf(stderr, "OUTPUT_CAPTURE_ENABLE and PERF_ENABLE are both set; disabling PERF for capture mode.\n");
        perf_enabled = 0;
    }

    if (!perf_output_dir || !*perf_output_dir) {
        perf_output_dir = DEFAULT_PERF_OUTPUT_DIR;
    }
    if (!perf_events || !*perf_events) {
        perf_events = DEFAULT_PERF_EVENTS;
    }
    if (!capture_output_dir || !*capture_output_dir) {
        capture_output_dir = DEFAULT_CAPTURE_OUTPUT_DIR;
    }
    if (!capture_sql_dir || !*capture_sql_dir) {
        capture_sql_dir = DEFAULT_CAPTURE_SQL_DIR;
    }
    if (!capture_size_log_file || !*capture_size_log_file) {
        capture_size_log_file = DEFAULT_CAPTURE_SIZE_LOG_FILE;
    }

    const char *filter = getenv("QUERY_FILTER"); // if NULL -> treat as "all"
    int filter_all = 0;
    if (!filter) filter_all = 1;
    else if (strcmp(filter, "") == 0 || strcmp(filter, "all") == 0) filter_all = 1;

    struct stat st;
    int needs_csv_header = (stat(log_file, &st) != 0 || st.st_size == 0);

    FILE *log = fopen(log_file, "a");
    if (!log) {
        perror("fopen log file");
        return 1;
    }

    if (needs_csv_header) {
        fprintf(log,
                "timestamp_utc,run_id,test_name,loop_index,loops,runs_per_loop,total_runs,total_elapsed_sec,failures,rapl_package_j,rapl_core_j,rapl_gpu_j,rapl_dram_j,query_dir,query_filter,sigless_addr,run_started_utc\n");
        fflush(log);
    }

    char run_id[32];
    char run_started_utc[32];
    generate_run_id(run_id, sizeof(run_id));
    utc_timestamp_now(run_started_utc, sizeof(run_started_utc));

    FILE *capture_log = NULL;
    if (capture_enabled) {
        struct stat cap_st;
        int needs_capture_header;

        if (ensure_dir_exists(capture_output_dir) != 0) {
            fprintf(stderr, "Failed to create output capture dir: %s\n", capture_output_dir);
            fclose(log);
            return 1;
        }
        if (ensure_dir_exists(capture_sql_dir) != 0) {
            fprintf(stderr, "Failed to create rewritten SQL dir: %s\n", capture_sql_dir);
            fclose(log);
            return 1;
        }

        needs_capture_header = (stat(capture_size_log_file, &cap_st) != 0 || cap_st.st_size == 0);
        capture_log = fopen(capture_size_log_file, "a");
        if (!capture_log) {
            perror("fopen output capture log file");
            fclose(log);
            return 1;
        }

        if (needs_capture_header) {
            fprintf(capture_log,
                    "timestamp_utc,run_id,test_name,loop_index,run_index,effective_runs,output_file,output_bytes,exit_code,source_query_file,rewritten_query_file,capture_output_dir,run_started_utc\n");
            fflush(capture_log);
        }

        printf("Output capture mode enabled. Forcing runs_per_loop=1 (RUNS=%d ignored).\n", RUNS);
        printf("Capture outputs in %s\n", capture_output_dir);
        fflush(stdout);
    }

    if (perf_enabled) {
        if (ensure_dir_exists(perf_output_dir) != 0) {
            fprintf(stderr, "Failed to create PERF output dir: %s\n", perf_output_dir);
            if (capture_log) fclose(capture_log);
            fclose(log);
            return 1;
        }
        if (write_perf_metadata_file(perf_output_dir,
                                     run_id,
                                     run_started_utc,
                                     query_dir,
                                     filter_all ? "all" : filter,
                                     perf_events) != 0) {
            fprintf(stderr, "Failed to write PERF metadata file in: %s\n", perf_output_dir);
            if (capture_log) fclose(capture_log);
            fclose(log);
            return 1;
        }
        printf("PERF enabled. Writing stat files to %s\n", perf_output_dir);
        fflush(stdout);
    }

    DIR *dir = opendir(query_dir);
    if (!dir) {
        fprintf(stderr, "opendir(%s): %s\n", query_dir, strerror(errno));
        if (capture_log) fclose(capture_log);
        fclose(log);
        return 1;
    }

    char *files[MAX_FILES];
    int count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!valid_query_name(entry->d_name)) continue; // accepts the two formats
        if (!filter_all) {
            if (strstr(entry->d_name, filter) == NULL) continue; // apply substring filter
        }
        if (count >= MAX_FILES) break;
        files[count] = strdup(entry->d_name);
        if (!files[count]) {
            fprintf(stderr, "strdup failed\n");
            break;
        }
        count++;
    }
    closedir(dir);

    if (count == 0) {
        fprintf(stderr, "No matching queries found in %s (filter='%s')\n",
                query_dir, filter ? filter : "NULL");
        fprintf(log, "No matching queries found in %s (filter='%s')\n",
                query_dir, filter ? filter : "NULL");
        if (capture_log) fclose(capture_log);
        fclose(log);
        return 1;
    }

    qsort(files, count, sizeof(char *), cmp_queries);

    int core = 0;
    if (rapl_init(core) != 0) {
        fprintf(stderr, "rapl_init failed\n");
        for (int i = 0; i < count; i++) {
            free(files[i]);
        }
        if (capture_log) fclose(capture_log);
        fclose(log);
        return 1;
    }

    if (prime_sudo_credentials(sudo_password) != 0) {
        fprintf(stderr, "Failed to prime sudo credentials; set SUDO_PASSWORD if needed.\n");
        for (int i = 0; i < count; i++) {
            free(files[i]);
        }
        if (capture_log) fclose(capture_log);
        fclose(log);
        return 1;
    }

    for (int f = 0; f < count; f++) {
        const int effective_runs = capture_enabled ? 1 : RUNS;
        char rewritten_query_path[PATH_MAX];
        char source_query_path[PATH_MAX];
        source_query_path[0] = '\0';
        rewritten_query_path[0] = '\0';

        int sp = snprintf(source_query_path, sizeof(source_query_path), "%s/%s", query_dir, files[f]);
        if (sp <= 0 || sp >= (int)sizeof(source_query_path)) {
            fprintf(stderr, "Source query path truncated for %s\n", files[f]);
            free(files[f]);
            continue;
        }

        if (capture_enabled) {
            char safe_name[256];
            sanitize_for_filename(files[f], safe_name, sizeof(safe_name));

            int rp = snprintf(rewritten_query_path, sizeof(rewritten_query_path),
                              "%s/%s__%s.sql", capture_sql_dir, run_id, safe_name);
            if (rp <= 0 || rp >= (int)sizeof(rewritten_query_path)) {
                fprintf(stderr, "Rewritten query path truncated for %s\n", files[f]);
                free(files[f]);
                continue;
            }

            if (rewrite_query_without_explain_analyze(files[f], source_query_path, rewritten_query_path) != 0) {
                fprintf(stderr, "Failed to rewrite query without EXPLAIN ANALYZE: %s\n", source_query_path);
                free(files[f]);
                continue;
            }
        }

        printf("Running %s  (%d runs x %d loops)\n", files[f], effective_runs, LOOPS);
        fflush(stdout);

        double file_total_elapsed = 0.0;
        int file_total_failures = 0;

        for (int loop_idx = 1; loop_idx <= LOOPS; loop_idx++) {
            double total_elapsed = 0.0;
            int failures = 0;

            char start_msg[512];
            int s1 = snprintf(start_msg, sizeof(start_msg),
                              "start,run_id=%s,test=%s,loop=%d/%d,runs=%d",
                              run_id, files[f], loop_idx, LOOPS, effective_runs);
            if (s1 > 0 && s1 < (int)sizeof(start_msg)) {
                post_sigless_script(sigless_addr, sigless_channel, start_msg);
            }

            rapl_before(log, core);

            // Capture mode intentionally forces one run per loop; performance mode keeps RUNS.
            for (int i = 0; i < effective_runs; i++) {
                char cmd[MAX_CMD];
                char capture_output_file[PATH_MAX];
                capture_output_file[0] = '\0';

                if (capture_enabled) {
                    char safe_name[256];
                    sanitize_for_filename(files[f], safe_name, sizeof(safe_name));
                    int p = snprintf(capture_output_file, sizeof(capture_output_file),
                                     "%s/%s__%s__%d_%d.out",
                                     capture_output_dir, run_id, safe_name, loop_idx, i + 1);
                    if (p <= 0 || p >= (int)sizeof(capture_output_file)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: output path truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, LOOPS, i + 1, effective_runs);
                        continue;
                    }

                    int n = snprintf(cmd, sizeof(cmd),
                                     "sudo -n -u postgres psql -d tpch -f \"%s\" > \"%s\" 2>&1",
                                     rewritten_query_path, capture_output_file);
                    if (n <= 0 || n >= (int)sizeof(cmd)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: capture command truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, LOOPS, i + 1, effective_runs);
                        continue;
                    }
                } else if (perf_enabled) {
                    char safe_name[256];
                    char perf_out_file[PATH_MAX];
                    sanitize_for_filename(files[f], safe_name, sizeof(safe_name));
                    int p = snprintf(perf_out_file, sizeof(perf_out_file),
                                     "%s/%s__%s.perfstat.csv",
                                     perf_output_dir, run_id, safe_name);
                    if (p <= 0 || p >= (int)sizeof(perf_out_file)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: PERF output path truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, LOOPS, i + 1, effective_runs);
                        continue;
                    }

                    // PERF mode appends each invocation into one CSV per query for the whole run.
                    int n = snprintf(cmd, sizeof(cmd),
                                     "perf stat -x, -e %s -- sh -c 'sudo -n -u postgres psql -d tpch -f \"%s/%s\" > /dev/null 2>&1' "
                                     "> /dev/null 2>> \"%s\"",
                                     perf_events, query_dir, files[f], perf_out_file);
                    if (n <= 0 || n >= (int)sizeof(cmd)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: PERF command truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, LOOPS, i + 1, effective_runs);
                        continue;
                    }
                } else {
                    int n = snprintf(cmd, sizeof(cmd),
                                     "sudo -n -u postgres psql -d tpch -f \"%s/%s\" > /dev/null 2>&1",
                                     query_dir, files[f]);
                    if (n <= 0 || n >= (int)sizeof(cmd)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: command truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, LOOPS, i + 1, effective_runs);
                        continue;
                    }
                }

                double start = get_time_sec();
                int ret = system(cmd);
                double end = get_time_sec();

                double elapsed = end - start;
                total_elapsed += elapsed;

                if (ret != 0) {
                    failures++;
                    fprintf(stderr, "  Warning: run %d failed (code %d) for %s [loop %d/%d]\n",
                            i + 1, ret, files[f], loop_idx, LOOPS);
                }

                if (capture_enabled && capture_log) {
                    long long out_size = file_size_bytes(capture_output_file);
                    char timestamp_utc[32];
                    utc_timestamp_now(timestamp_utc, sizeof(timestamp_utc));

                    csv_write_field(capture_log, timestamp_utc);
                    fprintf(capture_log, ",");
                    csv_write_field(capture_log, run_id);
                    fprintf(capture_log, ",");
                    csv_write_field(capture_log, files[f]);
                    fprintf(capture_log, ",%d,%d,%d,", loop_idx, i + 1, effective_runs);
                    csv_write_field(capture_log, capture_output_file);
                    fprintf(capture_log, ",%lld,%d,", out_size, ret);
                    csv_write_field(capture_log, source_query_path);
                    fprintf(capture_log, ",");
                    csv_write_field(capture_log, rewritten_query_path);
                    fprintf(capture_log, ",");
                    csv_write_field(capture_log, capture_output_dir);
                    fprintf(capture_log, ",");
                    csv_write_field(capture_log, run_started_utc);
                    fprintf(capture_log, "\n");
                    fflush(capture_log);
                }

                if ((i + 1) % 10 == 0 || i == effective_runs - 1) {
                    printf("  loop %d/%d completed %d/%d\n", loop_idx, LOOPS, i + 1, effective_runs);
                    fflush(stdout);
                }
            }

            char timestamp_utc[32];
            utc_timestamp_now(timestamp_utc, sizeof(timestamp_utc));

            csv_write_field(log, timestamp_utc);
            fprintf(log, ",");
            csv_write_field(log, run_id);
            fprintf(log, ",");
            csv_write_field(log, files[f]);
            fprintf(log, ",%d,%d,%d,%d,%.6f,%d,",
                    loop_idx, LOOPS, effective_runs, effective_runs * LOOPS, total_elapsed, failures);
            rapl_after(log, core);
            fprintf(log, ",");
            csv_write_field(log, query_dir);
            fprintf(log, ",");
            csv_write_field(log, filter_all ? "all" : filter);
            fprintf(log, ",");
            csv_write_field(log, sigless_addr);
            fprintf(log, ",");
            csv_write_field(log, run_started_utc);
            fprintf(log, "\n");
            fflush(log);

            char end_msg[512];
            int s2 = snprintf(end_msg, sizeof(end_msg),
                              "end,run_id=%s,test=%s,loop=%d/%d,runs=%d,elapsed_sec=%.6f,failures=%d",
                              run_id, files[f], loop_idx, LOOPS, effective_runs, total_elapsed, failures);
            if (s2 > 0 && s2 < (int)sizeof(end_msg)) {
                post_sigless_script(sigless_addr, sigless_channel, end_msg);
            }

            file_total_elapsed += total_elapsed;
            file_total_failures += failures;
            printf("  loop %d/%d total time: %.6f sec (failures: %d)\n",
                   loop_idx, LOOPS, total_elapsed, failures);
            fflush(stdout);

            if (SET_PAUSE_SEC > 0 && !(f == count - 1 && loop_idx == LOOPS)) {
                printf("  pausing %d seconds before next set...\n", SET_PAUSE_SEC);
                fflush(stdout);
                sleep(SET_PAUSE_SEC);
            }
        }

        printf("  Aggregate total time: %.6f sec (failures: %d)\n\n",
               file_total_elapsed, file_total_failures);

        free(files[f]);
    }

    if (capture_log) {
        fclose(capture_log);
    }
    fclose(log);
    return 0;
}