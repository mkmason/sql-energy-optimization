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
#include <errno.h>
#include <unistd.h>
#include "rapl.h"

#define MAX_FILES 4096
#define MAX_CMD 2048
#define DEFAULT_QUERY_DIR "queries"
#define DEFAULT_LOG_FILE "query_timing.log"
#define RUNS 100

static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* helper to call the script; drop the script into the same directory or adjust path */
static void post_sigless_script(const char *remote, const char *channel, const char *msg) {
    char cmd[2048];
    int n = snprintf(cmd, sizeof(cmd),
                     "sh ./post_to_sigless.sh %s %s \"%s\" >/dev/null 2>&1",
                     remote, channel, msg);
    if (n > 0 && n < (int)sizeof(cmd)) {
        system(cmd);
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

    const char *filter = getenv("QUERY_FILTER"); // if NULL -> treat as "all"
    int filter_all = 0;
    if (!filter) filter_all = 1;
    else if (strcmp(filter, "") == 0 || strcmp(filter, "all") == 0) filter_all = 1;

    FILE *log = fopen(log_file, "w");
    if (!log) {
        perror("fopen log file");
        return 1;
    }

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

    fprintf(log, "Query Timing Log\n================\n");

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

        printf("Running %s  (%d runs)\n", files[f], RUNS);
        fflush(stdout);

        double total_elapsed = 0.0;
        int failures = 0;

        post_sigless_script(getenv("SIGLESS_ADDR") ? getenv("SIGLESS_ADDR") : "127.0.0.1:8000",
                    getenv("SIGLESS_CHANNEL") ? getenv("SIGLESS_CHANNEL") : "CH1",
                    files[f]); // message: filename

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
                // print minimal warning to stderr, but do not log per-run times.
                fprintf(stderr, "  Warning: run %d failed (code %d) for %s\n", i + 1, ret, files[f]);
            }

            // Optional: light progress to stdout (not logged into file)
            if ((i+1) % 10 == 0 || i == RUNS - 1) {
                printf("  completed %d/%d\n", i+1, RUNS);
                fflush(stdout);
            }
        }

        // Only log the total time for the RUNS runs (and an informative failure count).
        fprintf(log, "\n%s\n", files[f]);
        fprintf(log, "  Total time for %d runs: %.6f sec (failures: %d)\n", RUNS, total_elapsed, failures);
        fprintf(log, "  RAPL delta (package, core, gpu, dram): ");
        rapl_after(log, core);

        post_sigless_script(getenv("SIGLESS_ADDR") ? getenv("SIGLESS_ADDR") : "127.0.0.1:8000",
                    getenv("SIGLESS_CHANNEL") ? getenv("SIGLESS_CHANNEL") : "CH1",
                    "stop");

        fprintf(log, "\n");
        printf("  Total time: %.6f sec (failures: %d)\n\n", total_elapsed, failures);

        free(files[f]);
    }

    fclose(log);
    return 0;
}