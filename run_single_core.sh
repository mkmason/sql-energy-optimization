#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${script_dir}"

query_filter="${1:-}"
cpu_affinity="${2:-0}"
log_file="${3:-logs/query_timing_single_core.csv}"
sample_dir="${4:-logs/single_core_rapl_samples}"
rapl_interval_ms="${5:-100}"
perf_dir="${6:-logs/single_core_perf_runs}"
perf_log="${7:-logs/single_core_perf_runs.csv}"
perf_interval_sec="${8:-1}"

make run-single-core \
  QUERY="${query_filter}" \
  CPU_AFFINITY="${cpu_affinity}" \
  SINGLE_CORE_LOG_FILE="${log_file}" \
  SINGLE_CORE_SAMPLE_DIR="${sample_dir}" \
  SINGLE_CORE_RAPL_INTERVAL_MS="${rapl_interval_ms}" \
  SINGLE_CORE_DETAILED_PERF_OUTPUT_DIR="${perf_dir}" \
  SINGLE_CORE_DETAILED_PERF_LOG_FILE="${perf_log}" \
  SINGLE_CORE_DETAILED_PERF_INTERVAL_SEC="${perf_interval_sec}"
