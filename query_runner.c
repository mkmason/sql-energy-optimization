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
#include "rapl.h"

#define MAX_FILES 4096
#define MAX_CMD 2048
#define DEFAULT_QUERY_DIR "queries"
#define DEFAULT_LOG_FILE "query_timing.csv"
#define RUNS 100
#define LOOPS 1
#define SET_PAUSE_SEC 10
#define DEFAULT_SIGLESS_ADDR "127.0.0.1:8000"
#define DEFAULT_SIGLESS_CHANNEL "CH1"

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

int main(void) {
    const char *query_dir = getenv("QUERY_DIR");
    if (!query_dir) query_dir = DEFAULT_QUERY_DIR;

    const char *log_file = getenv("LOG_FILE");
    if (!log_file) log_file = DEFAULT_LOG_FILE;

    const char *sigless_addr = DEFAULT_SIGLESS_ADDR;
    const char *sigless_channel = DEFAULT_SIGLESS_CHANNEL;

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

    DIR *dir = opendir(query_dir);
    if (!dir) {
        fprintf(stderr, "opendir(%s): %s\n", query_dir, strerror(errno));
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
        fclose(log);
        return 1;
    }

    for (int f = 0; f < count; f++) {
        char cmd[MAX_CMD];
        // Run psql as postgres user, sending output to /dev/null (same as original)
        // If your environment doesn't need sudo, set QUERY_RUN_CMD or edit here.
        int n = snprintf(cmd, sizeof(cmd),
                 "sudo -u postgres psql -d tpch -f %s/%s > /dev/null 2>&1",
                 query_dir, files[f]);
        if (n >= (int)sizeof(cmd)) {
            fprintf(stderr, "Command truncated for %s\n", files[f]);
            continue;
        }

        printf("Running %s  (%d runs x %d loops)\n", files[f], RUNS, LOOPS);
        fflush(stdout);

        double file_total_elapsed = 0.0;
        int file_total_failures = 0;

        for (int loop_idx = 1; loop_idx <= LOOPS; loop_idx++) {
            double total_elapsed = 0.0;
            int failures = 0;

            char start_msg[512];
            int s1 = snprintf(start_msg, sizeof(start_msg),
                              "start,run_id=%s,test=%s,loop=%d/%d,runs=%d",
                              run_id, files[f], loop_idx, LOOPS, RUNS);
            if (s1 > 0 && s1 < (int)sizeof(start_msg)) {
                post_sigless_script(sigless_addr, sigless_channel, start_msg);
            }

            rapl_before(log, core);

            // Run exactly RUNS iterations; measure elapsed per iteration and accumulate.
            for (int i = 0; i < RUNS; i++) {
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

                if ((i + 1) % 10 == 0 || i == RUNS - 1) {
                    printf("  loop %d/%d completed %d/%d\n", loop_idx, LOOPS, i + 1, RUNS);
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
                    loop_idx, LOOPS, RUNS, RUNS * LOOPS, total_elapsed, failures);
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
                              run_id, files[f], loop_idx, LOOPS, RUNS, total_elapsed, failures);
            if (s2 > 0 && s2 < (int)sizeof(end_msg)) {
                post_sigless_script(sigless_addr, sigless_channel, end_msg);
            }

            file_total_elapsed += total_elapsed;
            file_total_failures += failures;
            printf("  loop %d/%d total time: %.6f sec (failures: %d)\n",
                   loop_idx, LOOPS, total_elapsed, failures);
            fflush(stdout);

            if (SET_PAUSE_SEC > 0 && !(f == count - 1 && loop_idx == LOOPS)) {
                printf("  pausing %d seconds before next 100-run set...\n", SET_PAUSE_SEC);
                fflush(stdout);
                sleep(SET_PAUSE_SEC);
            }
        }

        printf("  Aggregate total time: %.6f sec (failures: %d)\n\n",
               file_total_elapsed, file_total_failures);

        free(files[f]);
    }

    fclose(log);
    return 0;
}