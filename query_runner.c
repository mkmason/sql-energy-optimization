#define _GNU_SOURCE
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
#include <sched.h>
#include <ctype.h>
#include <sys/wait.h>
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
#define DEFAULT_CPU_AFFINITY (-1)
#define DEFAULT_PERF_OUTPUT_DIR "logs/perf_runs"
#define DEFAULT_PERF_EVENTS "cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations,page-faults,ref-cycles,cpu-clock,task-clock"
#define DEFAULT_CAPTURE_OUTPUT_DIR "logs/query_outputs"
#define DEFAULT_CAPTURE_SQL_DIR "logs/rewritten_queries"
#define DEFAULT_CAPTURE_SIZE_LOG_FILE "logs/query_output_sizes.csv"
#define DEFAULT_DETAILED_PERF_OUTPUT_DIR "logs/detailed_perf_runs"
#define DEFAULT_DETAILED_PERF_LOG_FILE "logs/detailed_perf_runs.csv"
#define DEFAULT_DETAILED_PERF_INTERVAL_SEC 1
#define DEFAULT_DETAILED_TURBOSTAT_COLUMNS "Core,CPU,Avg_MHz,Busy%,Bzy_MHz,TSC_MHz,IPC,CoreTmp,PkgTmp,PkgWatt,CorWatt,RAMWatt"
#define DEFAULT_TEMP_CONTROL_LOG_FILE "logs/temp_controlled_runs.csv"
#define DEFAULT_TEMP_CONTROL_SAMPLE_DIR "logs/temp_controlled_samples"
#define DEFAULT_TEMP_CONTROL_THRESHOLD_C 40.0
#define DEFAULT_TEMP_CONTROL_MIN_PAUSE_SEC 15
#define DEFAULT_TEMP_CONTROL_RAPL_INTERVAL_MS 1
#define DEFAULT_TEMP_CONTROL_RAPL_EVENTS "power/energy-pkg/,power/energy-cores/,power/energy-gpu/,power/energy-ram/"
#define DEFAULT_RAPL_SAMPLE_DIR "logs/rapl_samples"
#define DEFAULT_RAPL_SAMPLE_INTERVAL_MS 100
#define DEFAULT_RAPL_SAMPLE_EVENTS "power/energy-pkg/,power/energy-cores/,power/energy-gpu/,power/energy-ram/"
#define DEFAULT_PLANNING_TIME_LOG_FILE "logs/planning_time.csv"

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

static int parse_cpu_affinity(const char *value, int *cpu_affinity) {
    char *end = NULL;
    long parsed;

    if (!value || !*value) {
        *cpu_affinity = DEFAULT_CPU_AFFINITY;
        return 0;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < -1 || parsed > INT_MAX) {
        return -1;
    }

    *cpu_affinity = (int)parsed;
    return 0;
}

static int apply_cpu_affinity(int cpu_affinity) {
    cpu_set_t mask;

    if (cpu_affinity < 0) {
        return 0;
    }

    CPU_ZERO(&mask);
    CPU_SET(cpu_affinity, &mask);

    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        return -1;
    }

    return 0;
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

static int ensure_parent_dir_exists(const char *file_path) {
    char tmp[PATH_MAX];
    const char *slash;
    size_t len;

    if (!file_path || !*file_path) {
        return -1;
    }

    slash = strrchr(file_path, '/');
    if (!slash) {
        return 0;
    }

    len = (size_t)(slash - file_path);
    if (len == 0 || len >= sizeof(tmp)) {
        return (len == 0) ? 0 : -1;
    }

    memcpy(tmp, file_path, len);
    tmp[len] = '\0';
    return ensure_dir_exists(tmp);
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

static int tool_available(const char *tool_name) {
    char cmd[256];
    int n = snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", tool_name);
    if (n <= 0 || n >= (int)sizeof(cmd)) {
        return 0;
    }
    return system(cmd) == 0;
}

static int parse_last_double_from_line(const char *line, double *value) {
    const char *cursor = line;
    int found = 0;
    double parsed = 0.0;

    while (*cursor) {
        char *end = NULL;
        double candidate = strtod(cursor, &end);
        if (end != cursor) {
            parsed = candidate;
            found = 1;
            cursor = end;
        } else {
            cursor++;
        }
    }

    if (!found) {
        return -1;
    }

    *value = parsed;
    return 0;
}

static int parse_planning_time_ms_from_line(const char *line, double *value) {
    const char *marker = "Planning Time:";
    const char *p = strstr(line, marker);
    char *end = NULL;

    if (!p) {
        return 0;
    }

    p += strlen(marker);
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (!*p) {
        return -1;
    }

    *value = strtod(p, &end);
    if (end == p) {
        return -1;
    }

    return 1;
}

static int parse_execution_time_ms_from_line(const char *line, double *value) {
    const char *marker = "Execution Time:";
    const char *p = strstr(line, marker);
    char *end = NULL;

    if (!p) {
        return 0;
    }

    p += strlen(marker);
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (!*p) {
        return -1;
    }

    *value = strtod(p, &end);
    if (end == p) {
        return -1;
    }

    return 1;
}

static int run_psql_capture_planning_time(const char *cmd,
                                          double *planning_time_ms,
                                          double *execution_time_ms,
                                          int *exit_code) {
    FILE *pipe = NULL;
    char line[1024];
    int found = 0;
    double last_value = 0.0;
    int exec_found = 0;
    double last_exec_value = 0.0;
    int status = 0;
    int code = -1;

    if (exit_code) {
        *exit_code = -1;
    }
    if (planning_time_ms) {
        *planning_time_ms = 0.0;
    }
    if (execution_time_ms) {
        *execution_time_ms = -1.0;
    }

    pipe = popen(cmd, "r");
    if (!pipe) {
        return -1;
    }

    while (fgets(line, sizeof(line), pipe)) {
        double parsed = 0.0;
        int rc = parse_planning_time_ms_from_line(line, &parsed);
        if (rc == 1) {
            found = 1;
            last_value = parsed;
        }

        rc = parse_execution_time_ms_from_line(line, &parsed);
        if (rc == 1) {
            exec_found = 1;
            last_exec_value = parsed;
        }
    }

    status = pclose(pipe);
    if (status != -1) {
        if (WIFEXITED(status)) {
            code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            code = 128 + WTERMSIG(status);
        }
    }

    if (exit_code) {
        *exit_code = code;
    }

    if (!found) {
        return 1;
    }

    if (planning_time_ms) {
        *planning_time_ms = last_value;
    }
    if (execution_time_ms && exec_found) {
        *execution_time_ms = last_exec_value;
    }

    return 0;
}

static int read_package_temp_celsius(double *temp_c) {
    const char *cmd = "sudo -n turbostat --quiet --Summary --show PkgTmp --interval 1 --num_iterations 1 2>/dev/null";
    FILE *pipe = popen(cmd, "r");
    char line[512];
    char last_line[512];

    if (!pipe) {
        return -1;
    }

    last_line[0] = '\0';
    while (fgets(line, sizeof(line), pipe)) {
        if (line[0] == '\0' || line[0] == '\n') {
            continue;
        }
        strncpy(last_line, line, sizeof(last_line) - 1);
        last_line[sizeof(last_line) - 1] = '\0';
    }

    if (pclose(pipe) == -1) {
        return -1;
    }

    if (last_line[0] == '\0') {
        return -1;
    }

    return parse_last_double_from_line(last_line, temp_c);
}

static int wait_for_package_cooldown(int min_pause_sec,
                                     double threshold_c,
                                     double *waited_sec) {
    double wait_started_sec = get_time_sec();

    for (;;) {
        double elapsed_sec = get_time_sec() - wait_started_sec;
        if (elapsed_sec < (double)min_pause_sec) {
            double remaining_sec = (double)min_pause_sec - elapsed_sec;
            if (remaining_sec > 1.0) {
                remaining_sec = 1.0;
            }

            struct timespec ts;
            ts.tv_sec = (time_t)remaining_sec;
            ts.tv_nsec = (long)((remaining_sec - (double)ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
            continue;
        }

        double temp_c = 0.0;
        if (read_package_temp_celsius(&temp_c) != 0) {
            return -1;
        }

        if (temp_c <= threshold_c) {
            if (waited_sec) {
                *waited_sec = get_time_sec() - wait_started_sec;
            }
            return 0;
        }

        sleep(1);
    }
}

static int write_detailed_perf_metadata_file(const char *detailed_perf_output_dir,
                                             const char *run_id,
                                             const char *run_started_utc,
                                             const char *query_dir,
                                             const char *query_filter,
                                             const char *perf_events,
                                             const char *turbostat_columns,
                                             int interval_sec) {
    char path[PATH_MAX];
    FILE *fp;

    int n = snprintf(path, sizeof(path), "%s/%s__detailed_perf_metadata.txt", detailed_perf_output_dir, run_id);
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
    fprintf(fp, "turbostat_columns=%s\n", turbostat_columns ? turbostat_columns : "");
    fprintf(fp, "sample_interval_sec=%d\n", interval_sec);

    fclose(fp);
    return 0;
}

static int write_combined_perf_script(const char *script_path,
                                      const char *rapl_sample_file,
                                      const char *rapl_sample_events,
                                      int rapl_sample_interval_ms,
                                      const char *detailed_turbostat_file,
                                      const char *detailed_turbostat_columns,
                                      int detailed_perf_interval_sec,
                                      const char *detailed_perf_file,
                                      const char *perf_events,
                                      const char *query_exec_path,
                                      int effective_runs) {
    FILE *fp = fopen(script_path, "w");
    if (!fp) {
        return -1;
    }

    fprintf(fp, "#!/bin/sh\n");
    fprintf(fp, "set -eu\n");
    fprintf(fp, "perf stat -a -I %d -x, -e %s -o \"%s\" -- sleep 999999 >/dev/null 2>&1 &\n",
            rapl_sample_interval_ms, rapl_sample_events, rapl_sample_file);
    fprintf(fp, "sample_pid=$!\n");
    fprintf(fp, "turbostat --quiet --show \"%s\" --interval %d >> \"%s\" 2>/dev/null &\n",
            detailed_turbostat_columns, detailed_perf_interval_sec, detailed_turbostat_file);
    fprintf(fp, "tpid=$!\n");
    fprintf(fp, "perf stat --append -I %d -x, -a -e %s -o \"%s\" -- sudo -n sh -c 'r=1; while [ \"$r\" -le %d ]; do sudo -n -u postgres psql -d tpch -f \"%s\" > /dev/null 2>&1 || exit $?; r=$((r+1)); done'\n",
            detailed_perf_interval_sec * 1000, perf_events, detailed_perf_file,
            effective_runs, query_exec_path);
    fprintf(fp, "status=$?\n");
    fprintf(fp, "kill -INT $sample_pid >/dev/null 2>&1 || true\n");
    fprintf(fp, "kill -INT $tpid >/dev/null 2>&1 || true\n");
    fprintf(fp, "wait $sample_pid >/dev/null 2>&1 || true\n");
    fprintf(fp, "wait $tpid >/dev/null 2>&1 || true\n");
    fprintf(fp, "exit $status\n");

    if (fclose(fp) != 0) {
        return -1;
    }

    if (chmod(script_path, 0700) != 0) {
        return -1;
    }

    return 0;
}

static int pin_postgres_to_cpu(int cpu_affinity) {
    char cmd[1024];
    int n;

    printf("Using taskset to pin postgres processes to CPU %d...\n", cpu_affinity);
    fflush(stdout);

    /* First, restart postgres to ensure fresh processes */
    n = snprintf(cmd, sizeof(cmd),
                 "sudo -n systemctl restart postgresql 2>&1");
    if (n <= 0 || n >= (int)sizeof(cmd)) {
        return -1;
    }
    int restart_ret = system(cmd);
    if (restart_ret != 0) {
        fprintf(stderr, "Failed to restart postgresql\n");
        return -1;
    }
    sleep(2);

    /* Get main postgres process PID and pin it; children will inherit affinity */
    n = snprintf(cmd, sizeof(cmd),
                 "MAIN_PID=$(sudo -n pgrep -f '^/usr/lib/postgresql.*/bin/postgres -D' | head -1); "
                 "if [ -z \"$MAIN_PID\" ]; then MAIN_PID=$(sudo -n pgrep -f 'postgres: ' | head -1); fi; "
                 "if [ -n \"$MAIN_PID\" ]; then sudo -n taskset -pc %d \"$MAIN_PID\"; fi",
                 cpu_affinity);
    if (n <= 0 || n >= (int)sizeof(cmd)) {
        return -1;
    }

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "Failed to execute taskset command\n");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        fprintf(stdout, "  %s", line);
    }
    int pipe_ret = pclose(pipe);

    if (pipe_ret != 0) {
        fprintf(stderr, "  Taskset command returned non-zero\n");
    }

    /* Verify the affinity was set by checking all postgres PIDs */
    printf("Verifying affinity of all postgres processes:\n");
    fflush(stdout);
    n = snprintf(cmd, sizeof(cmd),
                 "sudo -n pgrep -f 'postgres' | while read pid; do sudo -n taskset -pc \"$pid\" 2>/dev/null; done | grep -E 'affinity|pid'");
    if (n <= 0 || n >= (int)sizeof(cmd)) {
        return -1;
    }
    int verify_ret = system(cmd);

    printf("PostgreSQL processes pinning complete (verification code: %d)\n", verify_ret);
    fflush(stdout);
    return 0;
}

static int unpin_postgres(void) {
    /* Just restart postgres without any CPU affinity constraints */
    const char *cmd_restart = "sudo -n systemctl restart postgresql >/dev/null 2>&1";

    int ret = system(cmd_restart);
    if (ret != 0) {
        fprintf(stderr, "Warning: failed to restart postgresql\n");
        return -1;
    }

    /* Give postgres time to start up */
    sleep(2);
    return 0;
}

int main(void) {
    const char *query_dir = getenv("QUERY_DIR");
    if (!query_dir) query_dir = DEFAULT_QUERY_DIR;

    const char *log_file = getenv("LOG_FILE");
    if (!log_file) log_file = DEFAULT_LOG_FILE;

    const char *sigless_addr = DEFAULT_SIGLESS_ADDR;
    const char *sigless_channel = DEFAULT_SIGLESS_CHANNEL;
    const char *cpu_affinity_env = getenv("CPU_AFFINITY");
    const char *sudo_password = getenv("SUDO_PASSWORD");
    const char *runs_per_loop_env = getenv("RUNS_PER_LOOP");
    const char *loops_env = getenv("LOOPS");
    const char *perf_enable_env = getenv("PERF_ENABLE");
    const char *perf_output_dir = getenv("PERF_OUTPUT_DIR");
    const char *perf_events = getenv("PERF_EVENTS");
    const char *capture_enable_env = getenv("OUTPUT_CAPTURE_ENABLE");
    const char *capture_output_dir = getenv("OUTPUT_CAPTURE_DIR");
    const char *capture_sql_dir = getenv("OUTPUT_CAPTURE_SQL_DIR");
    const char *capture_size_log_file = getenv("OUTPUT_CAPTURE_LOG_FILE");
    const char *detailed_perf_enable_env = getenv("DETAILED_PERF_ENABLE");
    const char *detailed_perf_output_dir = getenv("DETAILED_PERF_OUTPUT_DIR");
    const char *detailed_perf_log_file = getenv("DETAILED_PERF_LOG_FILE");
    const char *detailed_perf_interval_env = getenv("DETAILED_PERF_INTERVAL_SEC");
    const char *detailed_turbostat_columns = getenv("DETAILED_TURBOSTAT_COLUMNS");
    const char *temp_control_enable_env = getenv("TEMP_CONTROL_ENABLE");
    const char *temp_control_log_file = getenv("TEMP_CONTROL_LOG_FILE");
    const char *temp_control_sample_dir = getenv("TEMP_CONTROL_SAMPLE_DIR");
    const char *temp_control_threshold_env = getenv("TEMP_CONTROL_THRESHOLD_C");
    const char *temp_control_min_pause_env = getenv("TEMP_CONTROL_MIN_PAUSE_SEC");
    const char *temp_control_rapl_events = getenv("TEMP_CONTROL_RAPL_EVENTS");
    const char *temp_control_rapl_interval_env = getenv("TEMP_CONTROL_RAPL_INTERVAL_MS");
    const char *rapl_sample_enable_env = getenv("RAPL_SAMPLE_ENABLE");
    const char *rapl_sample_dir = getenv("RAPL_SAMPLE_DIR");
    const char *rapl_sample_events = getenv("RAPL_SAMPLE_EVENTS");
    const char *rapl_sample_interval_env = getenv("RAPL_SAMPLE_INTERVAL_MS");
    const char *planning_time_enable_env = getenv("PLANNING_TIME_ENABLE");
    const char *planning_time_log_file = getenv("PLANNING_TIME_LOG_FILE");
    int perf_enabled = env_is_true(perf_enable_env);
    int capture_enabled = env_is_true(capture_enable_env);
    int detailed_perf_enabled = env_is_true(detailed_perf_enable_env);
    int temp_control_enabled = env_is_true(temp_control_enable_env);
    int rapl_sample_enabled = env_is_true(rapl_sample_enable_env);
    int planning_time_enabled = env_is_true(planning_time_enable_env);
    int runs_per_loop = RUNS;
    int loop_count = LOOPS;
    int detailed_perf_interval_sec = DEFAULT_DETAILED_PERF_INTERVAL_SEC;
    int temp_control_rapl_interval_ms = DEFAULT_TEMP_CONTROL_RAPL_INTERVAL_MS;
    int rapl_sample_interval_ms = DEFAULT_RAPL_SAMPLE_INTERVAL_MS;
    int temp_control_min_pause_sec = DEFAULT_TEMP_CONTROL_MIN_PAUSE_SEC;
    double temp_control_threshold_c = DEFAULT_TEMP_CONTROL_THRESHOLD_C;
    int cpu_affinity = DEFAULT_CPU_AFFINITY;
    int base_runs_per_loop = runs_per_loop;

    if (runs_per_loop_env && *runs_per_loop_env) {
        int parsed_runs = atoi(runs_per_loop_env);
        if (parsed_runs > 0) {
            runs_per_loop = parsed_runs;
        }
    }
    if (loops_env && *loops_env) {
        int parsed_loops = atoi(loops_env);
        if (parsed_loops > 0) {
            loop_count = parsed_loops;
        }
    }
    if (detailed_perf_interval_env && *detailed_perf_interval_env) {
        int parsed_interval = atoi(detailed_perf_interval_env);
        if (parsed_interval > 0) {
            detailed_perf_interval_sec = parsed_interval;
        }
    }
    if (temp_control_threshold_env && *temp_control_threshold_env) {
        double parsed_threshold = atof(temp_control_threshold_env);
        if (parsed_threshold > 0.0) {
            temp_control_threshold_c = parsed_threshold;
        }
    }
    if (temp_control_min_pause_env && *temp_control_min_pause_env) {
        int parsed_pause = atoi(temp_control_min_pause_env);
        if (parsed_pause > 0) {
            temp_control_min_pause_sec = parsed_pause;
        }
    }
    if (temp_control_rapl_interval_env && *temp_control_rapl_interval_env) {
        int parsed_interval = atoi(temp_control_rapl_interval_env);
        if (parsed_interval > 0) {
            temp_control_rapl_interval_ms = parsed_interval;
        }
    }
    if (rapl_sample_interval_env && *rapl_sample_interval_env) {
        int parsed_interval = atoi(rapl_sample_interval_env);
        if (parsed_interval > 0) {
            rapl_sample_interval_ms = parsed_interval;
        }
    }
    base_runs_per_loop = runs_per_loop;
    if (parse_cpu_affinity(cpu_affinity_env, &cpu_affinity) != 0) {
        fprintf(stderr, "Invalid CPU_AFFINITY value: %s\n", cpu_affinity_env);
        return 1;
    }

    if (cpu_affinity >= 0) {
        if (apply_cpu_affinity(cpu_affinity) != 0) {
            fprintf(stderr, "Failed to pin process to logical CPU %d: %s\n",
                    cpu_affinity, strerror(errno));
            return 1;
        }
        printf("Pinned runner and child processes to logical CPU %d\n", cpu_affinity);
        fflush(stdout);

        printf("Pinning PostgreSQL to logical CPU %d and restarting...\n", cpu_affinity);
        fflush(stdout);
        if (pin_postgres_to_cpu(cpu_affinity) != 0) {
            fprintf(stderr, "Warning: failed to pin PostgreSQL to CPU %d; continuing anyway\n", cpu_affinity);
        } else {
            printf("PostgreSQL pinned and restarted on CPU %d\n", cpu_affinity);
            fflush(stdout);
        }
    }

    if (capture_enabled && perf_enabled) {
        fprintf(stderr, "OUTPUT_CAPTURE_ENABLE and PERF_ENABLE are both set; disabling PERF for capture mode.\n");
        perf_enabled = 0;
    }
    if (detailed_perf_enabled) {
        if (perf_enabled || capture_enabled) {
            fprintf(stderr, "DETAILED_PERF_ENABLE overrides PERF_ENABLE and OUTPUT_CAPTURE_ENABLE.\n");
        }
        perf_enabled = 0;
        capture_enabled = 0;
    }
    if (temp_control_enabled) {
        if (perf_enabled || capture_enabled || detailed_perf_enabled) {
            fprintf(stderr, "TEMP_CONTROL_ENABLE overrides PERF_ENABLE, OUTPUT_CAPTURE_ENABLE, and DETAILED_PERF_ENABLE.\n");
        }
        if (rapl_sample_enabled) {
            fprintf(stderr, "TEMP_CONTROL_ENABLE overrides RAPL_SAMPLE_ENABLE.\n");
            rapl_sample_enabled = 0;
        }
        perf_enabled = 0;
        capture_enabled = 0;
        detailed_perf_enabled = 0;
        runs_per_loop = 1;
        if (!temp_control_log_file || !*temp_control_log_file) {
            temp_control_log_file = DEFAULT_TEMP_CONTROL_LOG_FILE;
        }
        if (!temp_control_sample_dir || !*temp_control_sample_dir) {
            temp_control_sample_dir = DEFAULT_TEMP_CONTROL_SAMPLE_DIR;
        }
        log_file = temp_control_log_file;
    }

    if (planning_time_enabled) {
        if (perf_enabled || capture_enabled || detailed_perf_enabled || temp_control_enabled || rapl_sample_enabled) {
            fprintf(stderr, "PLANNING_TIME_ENABLE overrides PERF_ENABLE, OUTPUT_CAPTURE_ENABLE, DETAILED_PERF_ENABLE, TEMP_CONTROL_ENABLE, and RAPL_SAMPLE_ENABLE.\n");
        }
        perf_enabled = 0;
        capture_enabled = 0;
        detailed_perf_enabled = 0;
        temp_control_enabled = 0;
        rapl_sample_enabled = 0;
        runs_per_loop = base_runs_per_loop;

        if (!planning_time_log_file || !*planning_time_log_file) {
            planning_time_log_file = DEFAULT_PLANNING_TIME_LOG_FILE;
        }
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
    if (!detailed_perf_output_dir || !*detailed_perf_output_dir) {
        detailed_perf_output_dir = DEFAULT_DETAILED_PERF_OUTPUT_DIR;
    }
    if (!detailed_perf_log_file || !*detailed_perf_log_file) {
        detailed_perf_log_file = DEFAULT_DETAILED_PERF_LOG_FILE;
    }
    if (!detailed_turbostat_columns || !*detailed_turbostat_columns) {
        detailed_turbostat_columns = DEFAULT_DETAILED_TURBOSTAT_COLUMNS;
    }
    if (!temp_control_rapl_events || !*temp_control_rapl_events) {
        temp_control_rapl_events = DEFAULT_TEMP_CONTROL_RAPL_EVENTS;
    }
    if (!rapl_sample_dir || !*rapl_sample_dir) {
        rapl_sample_dir = DEFAULT_RAPL_SAMPLE_DIR;
    }
    if (!rapl_sample_events || !*rapl_sample_events) {
        rapl_sample_events = DEFAULT_RAPL_SAMPLE_EVENTS;
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
        if (temp_control_enabled) {
            fprintf(log,
                    "timestamp_utc,run_id,test_name,loop_index,loops,runs_per_loop,total_runs,total_elapsed_sec,failures,rapl_package_j,rapl_core_j,rapl_gpu_j,rapl_dram_j,start_pkg_tmp_c,end_pkg_tmp_c,cooldown_wait_sec,cooldown_threshold_c,rapl_1ms_file,query_dir,query_filter,sigless_addr,run_started_utc\n");
        } else {
            fprintf(log,
                    "timestamp_utc,run_id,test_name,loop_index,loops,runs_per_loop,total_runs,total_elapsed_sec,failures,rapl_package_j,rapl_core_j,rapl_gpu_j,rapl_dram_j,query_dir,query_filter,sigless_addr,run_started_utc\n");
        }
        fflush(log);
    }

    char run_id[32];
    char run_started_utc[32];
    generate_run_id(run_id, sizeof(run_id));
    utc_timestamp_now(run_started_utc, sizeof(run_started_utc));

    FILE *capture_log = NULL;
    FILE *detailed_perf_log = NULL;
    FILE *planning_time_log = NULL;
    if (planning_time_enabled) {
        struct stat plan_st;
        int needs_planning_header;

        if (ensure_parent_dir_exists(planning_time_log_file) != 0) {
            fprintf(stderr, "Failed to create planning-time log directory for: %s\n", planning_time_log_file);
            fclose(log);
            return 1;
        }

        needs_planning_header = (stat(planning_time_log_file, &plan_st) != 0 || plan_st.st_size == 0);
        planning_time_log = fopen(planning_time_log_file, "a");
        if (!planning_time_log) {
            perror("fopen planning time log file");
            fclose(log);
            return 1;
        }

        if (needs_planning_header) {
            fprintf(planning_time_log,
                    "timestamp_utc,run_id,test_name,loop_index,run_index,planning_time_ms,execution_time_ms,exit_code,source_query_file,query_dir,query_filter,run_started_utc\n");
            fflush(planning_time_log);
        }

        printf("Planning-time mode enabled. Logging planning times to %s\n", planning_time_log_file);
        fflush(stdout);
    }
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
        runs_per_loop = 1;
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

    if (detailed_perf_enabled) {
        struct stat detailed_st;
        int needs_detailed_header;

        if (!tool_available("perf")) {
            fprintf(stderr, "perf is not available on PATH.\n");
            if (capture_log) fclose(capture_log);
            fclose(log);
            return 1;
        }
        if (!tool_available("turbostat")) {
            fprintf(stderr, "turbostat is not available on PATH. Temperature/frequency sampling will not work.\n");
            if (capture_log) fclose(capture_log);
            fclose(log);
            return 1;
        }
        if (ensure_dir_exists(detailed_perf_output_dir) != 0) {
            fprintf(stderr, "Failed to create detailed PERF output dir: %s\n", detailed_perf_output_dir);
            if (capture_log) fclose(capture_log);
            fclose(log);
            return 1;
        }

        if (write_detailed_perf_metadata_file(detailed_perf_output_dir,
                                              run_id,
                                              run_started_utc,
                                              query_dir,
                                              filter_all ? "all" : filter,
                                              perf_events,
                                              detailed_turbostat_columns,
                                              detailed_perf_interval_sec) != 0) {
            fprintf(stderr, "Failed to write detailed PERF metadata file in: %s\n", detailed_perf_output_dir);
            if (capture_log) fclose(capture_log);
            fclose(log);
            return 1;
        }

        needs_detailed_header = (stat(detailed_perf_log_file, &detailed_st) != 0 || detailed_st.st_size == 0);
        detailed_perf_log = fopen(detailed_perf_log_file, "a");
        if (!detailed_perf_log) {
            perror("fopen detailed PERF log file");
            if (capture_log) fclose(capture_log);
            fclose(log);
            return 1;
        }
        if (needs_detailed_header) {
            fprintf(detailed_perf_log,
                    "timestamp_utc,run_id,test_name,loop_index,run_index,effective_runs,perf_file,turbostat_file,exit_code,source_query_file,detailed_perf_output_dir,sample_interval_sec,run_started_utc\n");
            fflush(detailed_perf_log);
        }

        printf("Detailed PERF mode enabled. Runs per loop follow RUNS=%d with continuous interval sampling.\n", runs_per_loop);
        printf("Detailed PERF outputs in %s\n", detailed_perf_output_dir);
        printf("Detailed turbostat columns: %s\n", detailed_turbostat_columns);
        fflush(stdout);
    }

    if (temp_control_enabled) {
        double probe_temp_c = 0.0;

        if (!tool_available("perf")) {
            fprintf(stderr, "perf is not available on PATH.\n");
            if (capture_log) fclose(capture_log);
            if (detailed_perf_log) fclose(detailed_perf_log);
            fclose(log);
            return 1;
        }
        if (!tool_available("turbostat")) {
            fprintf(stderr, "turbostat is not available on PATH. Temperature gating will not work.\n");
            if (capture_log) fclose(capture_log);
            if (detailed_perf_log) fclose(detailed_perf_log);
            fclose(log);
            return 1;
        }
        if (ensure_dir_exists(temp_control_sample_dir) != 0) {
            fprintf(stderr, "Failed to create temp-control sample dir: %s\n", temp_control_sample_dir);
            if (capture_log) fclose(capture_log);
            if (detailed_perf_log) fclose(detailed_perf_log);
            fclose(log);
            return 1;
        }
        if (read_package_temp_celsius(&probe_temp_c) != 0) {
            fprintf(stderr, "Failed to read package temperature through turbostat.\n");
            if (capture_log) fclose(capture_log);
            if (detailed_perf_log) fclose(detailed_perf_log);
            fclose(log);
            return 1;
        }

        printf("Temp-controlled mode enabled. Runs per loop forced to 1.\n");
        printf("Cooling threshold: %.1f C, minimum pause: %d sec\n", temp_control_threshold_c, temp_control_min_pause_sec);
        printf("Temp-control samples in %s\n", temp_control_sample_dir);
        printf("Initial package temperature: %.2f C\n", probe_temp_c);
        fflush(stdout);
    }

    if (rapl_sample_enabled) {
        if (!tool_available("perf")) {
            fprintf(stderr, "perf is not available on PATH.\n");
            if (capture_log) fclose(capture_log);
            if (detailed_perf_log) fclose(detailed_perf_log);
            fclose(log);
            return 1;
        }
        if (ensure_dir_exists(rapl_sample_dir) != 0) {
            fprintf(stderr, "Failed to create RAPL sample dir: %s\n", rapl_sample_dir);
            if (capture_log) fclose(capture_log);
            if (detailed_perf_log) fclose(detailed_perf_log);
            fclose(log);
            return 1;
        }

        printf("RAPL interval sampling enabled (normal mode).\n");
        printf("RAPL sample interval: %d ms\n", rapl_sample_interval_ms);
        printf("RAPL sample outputs in %s\n", rapl_sample_dir);
        fflush(stdout);
    }

    DIR *dir = opendir(query_dir);
    if (!dir) {
        fprintf(stderr, "opendir(%s): %s\n", query_dir, strerror(errno));
        if (capture_log) fclose(capture_log);
        if (detailed_perf_log) fclose(detailed_perf_log);
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
        if (detailed_perf_log) fclose(detailed_perf_log);
        fclose(log);
        return 1;
    }

    qsort(files, count, sizeof(char *), cmp_queries);

    int rapl_core = cpu_affinity >= 0 ? cpu_affinity : 0;
    if (rapl_init(rapl_core) != 0) {
        fprintf(stderr, "rapl_init failed\n");
        for (int i = 0; i < count; i++) {
            free(files[i]);
        }
        if (capture_log) fclose(capture_log);
        if (detailed_perf_log) fclose(detailed_perf_log);
        fclose(log);
        return 1;
    }

    if (prime_sudo_credentials(sudo_password) != 0) {
        fprintf(stderr, "Failed to prime sudo credentials; set SUDO_PASSWORD if needed.\n");
        for (int i = 0; i < count; i++) {
            free(files[i]);
        }
        if (capture_log) fclose(capture_log);
        if (detailed_perf_log) fclose(detailed_perf_log);
        fclose(log);
        return 1;
    }

    for (int f = 0; f < count; f++) {
        const int effective_runs = runs_per_loop;
        char rewritten_query_path[PATH_MAX];
        char source_query_path[PATH_MAX];
        char detailed_perf_file[PATH_MAX];
        char detailed_turbostat_file[PATH_MAX];
        source_query_path[0] = '\0';
        rewritten_query_path[0] = '\0';
        detailed_perf_file[0] = '\0';
        detailed_turbostat_file[0] = '\0';

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

        if (detailed_perf_enabled) {
            char safe_name[256];
            sanitize_for_filename(files[f], safe_name, sizeof(safe_name));

            int pp = snprintf(detailed_perf_file, sizeof(detailed_perf_file),
                              "%s/%s__%s.perf.csv",
                              detailed_perf_output_dir, run_id, safe_name);
            if (pp <= 0 || pp >= (int)sizeof(detailed_perf_file)) {
                fprintf(stderr, "Detailed PERF aggregate path truncated for %s\n", files[f]);
                free(files[f]);
                continue;
            }

            int tp = snprintf(detailed_turbostat_file, sizeof(detailed_turbostat_file),
                              "%s/%s__%s.turbostat.csv",
                              detailed_perf_output_dir, run_id, safe_name);
            if (tp <= 0 || tp >= (int)sizeof(detailed_turbostat_file)) {
                fprintf(stderr, "Detailed turbostat aggregate path truncated for %s\n", files[f]);
                free(files[f]);
                continue;
            }
        }

        printf("Running %s  (%d runs x %d loops)\n", files[f], effective_runs, loop_count);
        fflush(stdout);

        double file_total_elapsed = 0.0;
        int file_total_failures = 0;

        for (int loop_idx = 1; loop_idx <= loop_count; loop_idx++) {
            double total_elapsed = 0.0;
            int failures = 0;
            double start_pkg_tmp_c = 0.0;
            double end_pkg_tmp_c = 0.0;
            double cooldown_wait_sec = 0.0;
            char temp_control_rapl_file[PATH_MAX];
            char combined_script_path[PATH_MAX];

            temp_control_rapl_file[0] = '\0';
            combined_script_path[0] = '\0';

            char start_msg[512];
            int s1 = snprintf(start_msg, sizeof(start_msg),
                              "start,run_id=%s,test=%s,loop=%d/%d,runs=%d",
                              run_id, files[f], loop_idx, loop_count, effective_runs);
            if (s1 > 0 && s1 < (int)sizeof(start_msg)) {
                post_sigless_script(sigless_addr, sigless_channel, start_msg);
            }

            if (temp_control_enabled) {
                if (read_package_temp_celsius(&start_pkg_tmp_c) != 0) {
                    fprintf(stderr, "  Warning: failed to read starting package temperature for %s [loop %d/%d]\n",
                            files[f], loop_idx, loop_count);
                    failures++;
                }
            }

            rapl_before(log, rapl_core);

            // Capture mode intentionally forces one run per loop; PERF and detailed PERF keep RUNS.
            for (int i = 0; i < effective_runs; i++) {
                char cmd[MAX_CMD];
                char capture_output_file[PATH_MAX];
                char rapl_sample_file[PATH_MAX];
                capture_output_file[0] = '\0';
                rapl_sample_file[0] = '\0';

                if (temp_control_enabled) {
                    char safe_name[256];
                    sanitize_for_filename(files[f], safe_name, sizeof(safe_name));

                    int rp = snprintf(temp_control_rapl_file, sizeof(temp_control_rapl_file),
                                      "%s/%s__%s__loop%d_run%d.rapl.csv",
                                      temp_control_sample_dir, run_id, safe_name, loop_idx, i + 1);
                    if (rp <= 0 || rp >= (int)sizeof(temp_control_rapl_file)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: temp-control RAPL path truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
                        continue;
                    }

                    int n = snprintf(cmd, sizeof(cmd),
                                     "perf stat -a -I %d -x, -e %s -o \"%s\" -- "
                                     "sudo -n -u postgres psql -d tpch -f \"%s\" > /dev/null 2>&1",
                                     temp_control_rapl_interval_ms, temp_control_rapl_events,
                                     temp_control_rapl_file, source_query_path);
                    if (n <= 0 || n >= (int)sizeof(cmd)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: temp-control command truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
                        continue;
                    }
                } else if (planning_time_enabled) {
                    int n = snprintf(cmd, sizeof(cmd),
                                     "sudo -n -u postgres psql -d tpch -f \"%s\" 2>&1",
                                     source_query_path);
                    if (n <= 0 || n >= (int)sizeof(cmd)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: planning-time command truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
                        continue;
                    }
                } else if (detailed_perf_enabled && rapl_sample_enabled) {
                    char safe_name[256];
                    char rapl_sample_file[PATH_MAX];
                    sanitize_for_filename(files[f], safe_name, sizeof(safe_name));

                    int rp = snprintf(rapl_sample_file, sizeof(rapl_sample_file),
                                      "%s/%s__%s__loop%d_run%d.rapl.csv",
                                      rapl_sample_dir, run_id, safe_name, loop_idx, i + 1);
                    if (rp <= 0 || rp >= (int)sizeof(rapl_sample_file)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: RAPL sample path truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
                        continue;
                    }

                    const char *query_exec_path = source_query_path;
            int sc = snprintf(combined_script_path, sizeof(combined_script_path),
                      "/tmp/rapl_single_core_%s_%d_%d.sh",
                      run_id, loop_idx, i + 1);
            if (sc <= 0 || sc >= (int)sizeof(combined_script_path)) {
            failures++;
            fprintf(stderr,
                "  Warning: combined script path truncated for %s [loop %d/%d run %d/%d]\n",
                files[f], loop_idx, loop_count, i + 1, effective_runs);
            continue;
            }

            if (write_combined_perf_script(combined_script_path,
                           rapl_sample_file,
                           rapl_sample_events,
                           rapl_sample_interval_ms,
                           detailed_turbostat_file,
                           detailed_turbostat_columns,
                           detailed_perf_interval_sec,
                           detailed_perf_file,
                           perf_events,
                           query_exec_path,
                           effective_runs) != 0) {
            failures++;
            fprintf(stderr,
                "  Warning: failed to write combined script for %s [loop %d/%d run %d/%d]\n",
                files[f], loop_idx, loop_count, i + 1, effective_runs);
            continue;
            }

            int n = snprintf(cmd, sizeof(cmd), "sh \"%s\"", combined_script_path);
                    if (n <= 0 || n >= (int)sizeof(cmd)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: combined detailed PERF/RAPL command truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
                        continue;
                    }
                } else if (detailed_perf_enabled) {
                    const char *query_exec_path = source_query_path;

                    int n = snprintf(cmd, sizeof(cmd),
                                     "sudo -n sh -c '"
                                     "turbostat --quiet --show \"%s\" --interval %d >> \"%s\" 2>/dev/null & "
                                     "tpid=$!; "
                                     "perf stat --append -I %d -x, -a -e %s -o \"%s\" -- "
                                     "sudo -n sh -c \"r=1; while [ \\$r -le %d ]; do sudo -n -u postgres psql -d tpch -f \\\"%s\\\" > /dev/null 2>&1 || exit \\$?; r=\\$((r+1)); done\"; "
                                     "status=$?; "
                                     "kill -INT $tpid >/dev/null 2>&1 || true; "
                                     "wait $tpid >/dev/null 2>&1 || true; "
                                     "exit $status'",
                                     detailed_turbostat_columns,
                                     detailed_perf_interval_sec, detailed_turbostat_file,
                                     detailed_perf_interval_sec * 1000, perf_events,
                                     detailed_perf_file, effective_runs, query_exec_path);
                    if (n <= 0 || n >= (int)sizeof(cmd)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: detailed PERF command truncated for %s [loop %d/%d]\n",
                                files[f], loop_idx, loop_count);
                        continue;
                    }
                } else if (rapl_sample_enabled) {
                    char safe_name[256];
                    sanitize_for_filename(files[f], safe_name, sizeof(safe_name));

                    int rp = snprintf(rapl_sample_file, sizeof(rapl_sample_file),
                                      "%s/%s__%s__loop%d_run%d.rapl.csv",
                                      rapl_sample_dir, run_id, safe_name, loop_idx, i + 1);
                    if (rp <= 0 || rp >= (int)sizeof(rapl_sample_file)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: RAPL sample path truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
                        continue;
                    }

                    int n = snprintf(cmd, sizeof(cmd),
                                     "perf stat -a -I %d -x, -e %s -o \"%s\" -- "
                                     "sudo -n -u postgres psql -d tpch -f \"%s/%s\" > /dev/null 2>&1",
                                     rapl_sample_interval_ms, rapl_sample_events,
                                     rapl_sample_file, query_dir, files[f]);
                    if (n <= 0 || n >= (int)sizeof(cmd)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: RAPL sample command truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
                        continue;
                    }
                } else if (capture_enabled) {
                    char safe_name[256];
                    sanitize_for_filename(files[f], safe_name, sizeof(safe_name));
                    int p = snprintf(capture_output_file, sizeof(capture_output_file),
                                     "%s/%s__%s__%d_%d.out",
                                     capture_output_dir, run_id, safe_name, loop_idx, i + 1);
                    if (p <= 0 || p >= (int)sizeof(capture_output_file)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: output path truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
                        continue;
                    }

                    int n = snprintf(cmd, sizeof(cmd),
                                     "sudo -n -u postgres psql -d tpch -f \"%s\" > \"%s\" 2>&1",
                                     rewritten_query_path, capture_output_file);
                    if (n <= 0 || n >= (int)sizeof(cmd)) {
                        failures++;
                        fprintf(stderr,
                                "  Warning: capture command truncated for %s [loop %d/%d run %d/%d]\n",
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
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
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
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
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
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
                                files[f], loop_idx, loop_count, i + 1, effective_runs);
                        continue;
                    }
                }

                int ret = 0;
                int planning_exit_code = 0;
                int planning_status = 0;
                double planning_time_ms = 0.0;
                double execution_time_ms = -1.0;

                double start = get_time_sec();
                if (planning_time_enabled) {
                    planning_status = run_psql_capture_planning_time(cmd, &planning_time_ms, &execution_time_ms, &planning_exit_code);
                    if (planning_exit_code != 0) {
                        ret = planning_exit_code;
                    } else if (planning_status != 0) {
                        ret = 1;
                    } else {
                        ret = 0;
                    }
                } else {
                    ret = system(cmd);
                }
                double end = get_time_sec();

                double elapsed = end - start;
                total_elapsed += elapsed;

                if (combined_script_path[0] != '\0') {
                    unlink(combined_script_path);
                    combined_script_path[0] = '\0';
                }

                if (ret != 0) {
                    failures++;
                    if (detailed_perf_enabled) {
                        fprintf(stderr, "  Warning: detailed PERF batch failed (code %d) for %s [loop %d/%d]\n",
                                ret, files[f], loop_idx, loop_count);
                    } else {
                        fprintf(stderr, "  Warning: run %d failed (code %d) for %s [loop %d/%d]\n",
                                i + 1, ret, files[f], loop_idx, loop_count);
                    }
                }

                if (planning_time_enabled && planning_time_log) {
                    char timestamp_utc[32];

                    utc_timestamp_now(timestamp_utc, sizeof(timestamp_utc));

                    csv_write_field(planning_time_log, timestamp_utc);
                    fprintf(planning_time_log, ",");
                    csv_write_field(planning_time_log, run_id);
                    fprintf(planning_time_log, ",");
                    csv_write_field(planning_time_log, files[f]);
                    fprintf(planning_time_log, ",%d,%d,", loop_idx, i + 1);
                    if (planning_status == 0) {
                        fprintf(planning_time_log, "%.6f", planning_time_ms);
                    }
                    fprintf(planning_time_log, ",");
                    if (execution_time_ms >= 0.0) {
                        fprintf(planning_time_log, "%.6f", execution_time_ms);
                    }
                    fprintf(planning_time_log, ",%d,", planning_exit_code);
                    csv_write_field(planning_time_log, source_query_path);
                    fprintf(planning_time_log, ",");
                    csv_write_field(planning_time_log, query_dir);
                    fprintf(planning_time_log, ",");
                    csv_write_field(planning_time_log, filter_all ? "all" : filter);
                    fprintf(planning_time_log, ",");
                    csv_write_field(planning_time_log, run_started_utc);
                    fprintf(planning_time_log, "\n");
                    fflush(planning_time_log);
                }

                if (temp_control_enabled) {
                    if (read_package_temp_celsius(&end_pkg_tmp_c) != 0) {
                        fprintf(stderr, "  Warning: failed to read ending package temperature for %s [loop %d/%d]\n",
                                files[f], loop_idx, loop_count);
                        failures++;
                    }
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

                if (detailed_perf_enabled && detailed_perf_log) {
                    char timestamp_utc[32];

                    utc_timestamp_now(timestamp_utc, sizeof(timestamp_utc));

                    csv_write_field(detailed_perf_log, timestamp_utc);
                    fprintf(detailed_perf_log, ",");
                    csv_write_field(detailed_perf_log, run_id);
                    fprintf(detailed_perf_log, ",");
                    csv_write_field(detailed_perf_log, files[f]);
                    fprintf(detailed_perf_log, ",%d,%d,%d,", loop_idx, i + 1, effective_runs);
                    csv_write_field(detailed_perf_log, detailed_perf_file);
                    fprintf(detailed_perf_log, ",");
                    csv_write_field(detailed_perf_log, detailed_turbostat_file);
                    fprintf(detailed_perf_log, ",%d,", ret);
                    csv_write_field(detailed_perf_log, source_query_path);
                    fprintf(detailed_perf_log, ",");
                    csv_write_field(detailed_perf_log, detailed_perf_output_dir);
                    fprintf(detailed_perf_log, ",%d,", detailed_perf_interval_sec);
                    csv_write_field(detailed_perf_log, run_started_utc);
                    fprintf(detailed_perf_log, "\n");
                    fflush(detailed_perf_log);
                }

                if (detailed_perf_enabled) {
                    printf("  loop %d/%d completed %d/%d\n", loop_idx, loop_count, effective_runs, effective_runs);
                    fflush(stdout);
                    break;
                }

                if ((i + 1) % 10 == 0 || i == effective_runs - 1) {
                    printf("  loop %d/%d completed %d/%d\n", loop_idx, loop_count, i + 1, effective_runs);
                    fflush(stdout);
                }
            }

            if (temp_control_enabled && !(f == count - 1 && loop_idx == loop_count)) {
                if (wait_for_package_cooldown(temp_control_min_pause_sec,
                                              temp_control_threshold_c,
                                              &cooldown_wait_sec) != 0) {
                    fprintf(stderr, "  Warning: failed while waiting for package cooldown after %s [loop %d/%d]\n",
                            files[f], loop_idx, loop_count);
                    failures++;
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
                    loop_idx, loop_count, effective_runs, effective_runs * loop_count, total_elapsed, failures);
            rapl_after(log, rapl_core);
            fprintf(log, ",");

            if (temp_control_enabled) {
                fprintf(log, "%.6f,%.6f,%.6f,%.1f,",
                        start_pkg_tmp_c,
                        end_pkg_tmp_c,
                        cooldown_wait_sec,
                        temp_control_threshold_c);
                csv_write_field(log, temp_control_rapl_file);
                fprintf(log, ",");
            }

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
                              run_id, files[f], loop_idx, loop_count, effective_runs, total_elapsed, failures);
            if (s2 > 0 && s2 < (int)sizeof(end_msg)) {
                post_sigless_script(sigless_addr, sigless_channel, end_msg);
            }

            file_total_elapsed += total_elapsed;
            file_total_failures += failures;
            printf("  loop %d/%d total time: %.6f sec (failures: %d)\n",
                   loop_idx, loop_count, total_elapsed, failures);
            fflush(stdout);

            if (!temp_control_enabled && SET_PAUSE_SEC > 0 && !(f == count - 1 && loop_idx == loop_count)) {
                printf("  pausing %d seconds before next set...\n", SET_PAUSE_SEC);
                fflush(stdout);
                sleep(SET_PAUSE_SEC);
            }
        }

        printf("  Aggregate total time: %.6f sec (failures: %d)\n\n",
               file_total_elapsed, file_total_failures);

        free(files[f]);
    }

    if (cpu_affinity >= 0) {
        printf("Restoring PostgreSQL to use all CPUs...\n");
        fflush(stdout);
        if (unpin_postgres() != 0) {
            fprintf(stderr, "Warning: failed to restore PostgreSQL CPU affinity\n");
        }
    }

    if (capture_log) {
        fclose(capture_log);
    }
    if (detailed_perf_log) {
        fclose(detailed_perf_log);
    }
    if (planning_time_log) {
        fclose(planning_time_log);
    }
    fclose(log);
    return 0;
}