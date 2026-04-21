#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${script_dir}"

query_filter="${1:-}"
cpu_affinity="${2:-0}"
log_file="${3:-logs/query_timing_single_core.csv}"
sample_dir="${4:-logs/single_core_rapl_samples}"
rapl_interval_ms="${5:-100}"

make run-single-core \
  QUERY="${query_filter}" \
  CPU_AFFINITY="${cpu_affinity}" \
  SINGLE_CORE_LOG_FILE="${log_file}" \
  SINGLE_CORE_SAMPLE_DIR="${sample_dir}" \
  SINGLE_CORE_RAPL_INTERVAL_MS="${rapl_interval_ms}"
