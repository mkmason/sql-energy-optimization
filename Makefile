# ===== Makefile =====

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
LDFLAGS = -lm
TARGET  = query_runner
SRC     = query_runner.c rapl.c
RUNNER_PREFIX ?= sudo
SUDO_PASSWORD ?= a
SUDO_PRIME_CMD = printf '%s\n' '$(SUDO_PASSWORD)' | sudo -S -v >/dev/null 2>&1
PERF_ENABLE ?= 1
PERF_OUTPUT_DIR ?= logs/perf_runs
PERF_EVENTS ?= cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations,page-faults
OUTPUT_CAPTURE_ENABLE ?= 1
OUTPUT_CAPTURE_DIR ?= logs/query_outputs
OUTPUT_CAPTURE_SQL_DIR ?= logs/rewritten_queries
OUTPUT_CAPTURE_LOG_FILE ?= logs/query_output_sizes.csv
DETAILED_PERF_ENABLE ?= 1
DETAILED_PERF_OUTPUT_DIR ?= logs/detailed_perf_runs
DETAILED_PERF_LOG_FILE ?= logs/detailed_perf_runs.csv
DETAILED_PERF_INTERVAL_SEC ?= 1
TEMP_CONTROL_LOG_FILE ?= logs/temp_controlled_runs.csv
TEMP_CONTROL_SAMPLE_DIR ?= logs/temp_controlled_samples
TEMP_CONTROL_THRESHOLD_C ?= 40
TEMP_CONTROL_MIN_PAUSE_SEC ?= 15
TEMP_CONTROL_RAPL_INTERVAL_MS ?= 1
TEMP_CONTROL_RAPL_EVENTS ?= power/energy-pkg/,power/energy-cores/,power/energy-gpu/,power/energy-ram/
LOOPS ?= 1
CPU_AFFINITY ?= 0
SINGLE_CORE_LOG_FILE ?= logs/query_timing_single_core.csv
SINGLE_CORE_SAMPLE_DIR ?= logs/single_core_rapl_samples
SINGLE_CORE_RAPL_INTERVAL_MS ?= 100
SINGLE_CORE_RAPL_EVENTS ?= power/energy-pkg/,power/energy-cores/,power/energy-gpu/,power/energy-ram/
SINGLE_CORE_DETAILED_PERF_OUTPUT_DIR ?= logs/single_core_perf_runs
SINGLE_CORE_DETAILED_PERF_LOG_FILE ?= logs/single_core_perf_runs.csv
SINGLE_CORE_DETAILED_PERF_INTERVAL_SEC ?= 1

# Default target
all: compile

# Compile the program
compile:
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

# Run all queries
run: compile
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) ./$(TARGET)

# Run a specific query (reminder-style target)
# Usage:
#   make run-single QUERY=APX1090
#   make run-single QUERY=APX1090-queryA.sql
run-single: compile
	@if [ -z "$(QUERY)" ]; then echo "You must provide QUERY. Example: make run-single QUERY=APX1090"; exit 1; fi
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) env QUERY_FILTER=$(QUERY) ./$(TARGET)

# Run all queries or a filtered subset pinned to one logical CPU/thread
# Usage:
#   make run-single-core
#   make run-single-core QUERY=APX1090 CPU_AFFINITY=2
run-single-core: compile
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) env QUERY_FILTER=$(QUERY) CPU_AFFINITY=$(CPU_AFFINITY) LOG_FILE="$(SINGLE_CORE_LOG_FILE)" RAPL_SAMPLE_ENABLE=1 RAPL_SAMPLE_DIR="$(SINGLE_CORE_SAMPLE_DIR)" RAPL_SAMPLE_INTERVAL_MS=$(SINGLE_CORE_RAPL_INTERVAL_MS) RAPL_SAMPLE_EVENTS="$(SINGLE_CORE_RAPL_EVENTS)" DETAILED_PERF_ENABLE=1 DETAILED_PERF_OUTPUT_DIR="$(SINGLE_CORE_DETAILED_PERF_OUTPUT_DIR)" DETAILED_PERF_LOG_FILE="$(SINGLE_CORE_DETAILED_PERF_LOG_FILE)" DETAILED_PERF_INTERVAL_SEC=$(SINGLE_CORE_DETAILED_PERF_INTERVAL_SEC) ./$(TARGET)

# Run all queries with PERF stat enabled (per query execution output files)
run-perf: compile
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) env PERF_ENABLE=$(PERF_ENABLE) PERF_OUTPUT_DIR="$(PERF_OUTPUT_DIR)" PERF_EVENTS="$(PERF_EVENTS)" ./$(TARGET)

# Run a specific query filter with PERF stat enabled
# Usage:
#   make run-perf-single QUERY=APX1090
run-perf-single: compile
	@if [ -z "$(QUERY)" ]; then echo "You must provide QUERY. Example: make run-perf-single QUERY=APX1090"; exit 1; fi
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) env QUERY_FILTER=$(QUERY) PERF_ENABLE=$(PERF_ENABLE) PERF_OUTPUT_DIR="$(PERF_OUTPUT_DIR)" PERF_EVENTS="$(PERF_EVENTS)" ./$(TARGET)

# Capture full query output for size analysis (forces 1 run/query/loop in runner)
run-output-capture: compile
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) env OUTPUT_CAPTURE_ENABLE=$(OUTPUT_CAPTURE_ENABLE) OUTPUT_CAPTURE_DIR="$(OUTPUT_CAPTURE_DIR)" OUTPUT_CAPTURE_SQL_DIR="$(OUTPUT_CAPTURE_SQL_DIR)" OUTPUT_CAPTURE_LOG_FILE="$(OUTPUT_CAPTURE_LOG_FILE)" ./$(TARGET)

# Capture full output for one filtered query set
# Usage:
#   make run-output-capture-single QUERY=APX1090
run-output-capture-single: compile
	@if [ -z "$(QUERY)" ]; then echo "You must provide QUERY. Example: make run-output-capture-single QUERY=APX1090"; exit 1; fi
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) env QUERY_FILTER=$(QUERY) OUTPUT_CAPTURE_ENABLE=$(OUTPUT_CAPTURE_ENABLE) OUTPUT_CAPTURE_DIR="$(OUTPUT_CAPTURE_DIR)" OUTPUT_CAPTURE_SQL_DIR="$(OUTPUT_CAPTURE_SQL_DIR)" OUTPUT_CAPTURE_LOG_FILE="$(OUTPUT_CAPTURE_LOG_FILE)" ./$(TARGET)

# Detailed PERF run with interval sampling and turbostat temperature/frequency data
run-perf-detailed: compile
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) env DETAILED_PERF_ENABLE=$(DETAILED_PERF_ENABLE) DETAILED_PERF_OUTPUT_DIR="$(DETAILED_PERF_OUTPUT_DIR)" DETAILED_PERF_LOG_FILE="$(DETAILED_PERF_LOG_FILE)" DETAILED_PERF_INTERVAL_SEC=$(DETAILED_PERF_INTERVAL_SEC) ./$(TARGET)

# Detailed PERF run for one query filter
# Usage:
#   make run-perf-detailed-single QUERY=APX1090
run-perf-detailed-single: compile
	@if [ -z "$(QUERY)" ]; then echo "You must provide QUERY. Example: make run-perf-detailed-single QUERY=APX1090"; exit 1; fi
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) env QUERY_FILTER=$(QUERY) DETAILED_PERF_ENABLE=$(DETAILED_PERF_ENABLE) DETAILED_PERF_OUTPUT_DIR="$(DETAILED_PERF_OUTPUT_DIR)" DETAILED_PERF_LOG_FILE="$(DETAILED_PERF_LOG_FILE)" DETAILED_PERF_INTERVAL_SEC=$(DETAILED_PERF_INTERVAL_SEC) ./$(TARGET)

# Temperature-controlled run with 1 run per loop and cooldown between runs
run-temp-controlled: compile
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) env TEMP_CONTROL_ENABLE=1 TEMP_CONTROL_LOG_FILE="$(TEMP_CONTROL_LOG_FILE)" TEMP_CONTROL_SAMPLE_DIR="$(TEMP_CONTROL_SAMPLE_DIR)" TEMP_CONTROL_THRESHOLD_C=$(TEMP_CONTROL_THRESHOLD_C) TEMP_CONTROL_MIN_PAUSE_SEC=$(TEMP_CONTROL_MIN_PAUSE_SEC) TEMP_CONTROL_RAPL_INTERVAL_MS=$(TEMP_CONTROL_RAPL_INTERVAL_MS) TEMP_CONTROL_RAPL_EVENTS="$(TEMP_CONTROL_RAPL_EVENTS)" LOOPS=$(LOOPS) RUNS_PER_LOOP=1 ./$(TARGET)

# Temperature-controlled run for one query filter
# Usage:
#   make run-temp-controlled-single QUERY=APX1090 LOOPS=5
run-temp-controlled-single: compile
	@if [ -z "$(QUERY)" ]; then echo "You must provide QUERY. Example: make run-temp-controlled-single QUERY=APX1090"; exit 1; fi
	@$(SUDO_PRIME_CMD)
	$(RUNNER_PREFIX) env QUERY_FILTER=$(QUERY) TEMP_CONTROL_ENABLE=1 TEMP_CONTROL_LOG_FILE="$(TEMP_CONTROL_LOG_FILE)" TEMP_CONTROL_SAMPLE_DIR="$(TEMP_CONTROL_SAMPLE_DIR)" TEMP_CONTROL_THRESHOLD_C=$(TEMP_CONTROL_THRESHOLD_C) TEMP_CONTROL_MIN_PAUSE_SEC=$(TEMP_CONTROL_MIN_PAUSE_SEC) TEMP_CONTROL_RAPL_INTERVAL_MS=$(TEMP_CONTROL_RAPL_INTERVAL_MS) TEMP_CONTROL_RAPL_EVENTS="$(TEMP_CONTROL_RAPL_EVENTS)" LOOPS=$(LOOPS) RUNS_PER_LOOP=1 ./$(TARGET)

# Optional: clean build artifacts
clean:
	rm -f $(TARGET)

# ===== Sigless Simulation Controls =====

SIGLESS_BIN ?= sigless
SIGLESS_PORT ?= 8000
SIGLESS_OUT ?= ./logs
SIGLESS_PID_FILE := .sigless.pid
TPCH_DBGEN_REPO ?= https://github.com/electrum/tpch-dbgen
TPCH_DBGEN_DIR ?= $(CURDIR)/tpch-dbgen
TPCH_SCALE ?= 1
PGUSER ?= postgres
PGDATABASE ?= tpch

sigless-sim-start:
	@echo "Starting Sigless in simulation mode..."
	@if [ -f $(SIGLESS_PID_FILE) ]; then \
		echo "Sigless appears to already be running (PID file exists)."; \
		exit 1; \
	fi
	@mkdir -p $(SIGLESS_OUT)
	@$(SIGLESS_BIN) --simulate --ch1 --web $(SIGLESS_PORT) --out $(SIGLESS_OUT) --verbose 1 > /dev/null 2>&1 & \
	echo $$! > $(SIGLESS_PID_FILE)
	@sleep 0.5
	@echo "Sigless started on http://localhost:$(SIGLESS_PORT)"

sigless-sim-end:
	@echo "Stopping Sigless..."
	@if [ ! -f $(SIGLESS_PID_FILE) ]; then \
		echo "No PID file found. Sigless may not be running."; \
		exit 1; \
	fi
	@kill `cat $(SIGLESS_PID_FILE)` 2>/dev/null || true
	@rm -f $(SIGLESS_PID_FILE)
	@echo "Sigless stopped."


setup:
	@set -e; \
	echo "Installing required system packages (build-essential, git, postgresql)..."; \
	sudo apt update; \
	sudo apt install -y build-essential git postgresql postgresql-contrib
	@set -e; \
	if [ ! -d "$(TPCH_DBGEN_DIR)" ]; then \
		echo "Cloning tpch-dbgen into $(TPCH_DBGEN_DIR)..."; \
		git clone "$(TPCH_DBGEN_REPO)" "$(TPCH_DBGEN_DIR)"; \
	else \
		echo "tpch-dbgen already exists at $(TPCH_DBGEN_DIR), skipping clone."; \
	fi
	@set -e; \
	echo "Building tpch-dbgen..."; \
	$(MAKE) -C "$(TPCH_DBGEN_DIR)"
	@set -e; \
	echo "Generating TPC-H data with scale factor $(TPCH_SCALE)..."; \
	cd "$(TPCH_DBGEN_DIR)"; \
	./dbgen -s "$(TPCH_SCALE)"
	@set -e; \
	echo "Removing trailing delimiters from generated .tbl files..."; \
	sed -i 's/|$$//' "$(TPCH_DBGEN_DIR)"/*.tbl
	@set -e; \
	echo "Ensuring PostgreSQL database $(PGDATABASE) exists..."; \
	sudo -u "$(PGUSER)" psql -tAc "SELECT 1 FROM pg_database WHERE datname='$(PGDATABASE)'" | grep -q 1 || \
		sudo -u "$(PGUSER)" psql -c "CREATE DATABASE $(PGDATABASE);"
	@set -e; \
	echo "Applying schema from tpch_schema.sql..."; \
	sudo -u "$(PGUSER)" psql -d "$(PGDATABASE)" -f "$(CURDIR)/tpch_schema.sql"
	@set -e; \
	echo "Creating load script for generated TPC-H tables..."; \
	printf "%s\n" \
		"\\copy region   FROM '$(TPCH_DBGEN_DIR)/region.tbl'   WITH (FORMAT csv, DELIMITER '|');" \
		"\\copy nation   FROM '$(TPCH_DBGEN_DIR)/nation.tbl'   WITH (FORMAT csv, DELIMITER '|');" \
		"\\copy supplier FROM '$(TPCH_DBGEN_DIR)/supplier.tbl' WITH (FORMAT csv, DELIMITER '|');" \
		"\\copy customer FROM '$(TPCH_DBGEN_DIR)/customer.tbl' WITH (FORMAT csv, DELIMITER '|');" \
		"\\copy part     FROM '$(TPCH_DBGEN_DIR)/part.tbl'     WITH (FORMAT csv, DELIMITER '|');" \
		"\\copy partsupp FROM '$(TPCH_DBGEN_DIR)/partsupp.tbl' WITH (FORMAT csv, DELIMITER '|');" \
		"\\copy orders   FROM '$(TPCH_DBGEN_DIR)/orders.tbl'   WITH (FORMAT csv, DELIMITER '|');" \
		"\\copy lineitem FROM '$(TPCH_DBGEN_DIR)/lineitem.tbl' WITH (FORMAT csv, DELIMITER '|');" \
	> "$(TPCH_DBGEN_DIR)/load_tpch.sql"
	@set -e; \
	echo "Loading generated data into $(PGDATABASE)..."; \
	sudo -u "$(PGUSER)" psql -d "$(PGDATABASE)" -f "$(TPCH_DBGEN_DIR)/load_tpch.sql"
	@set -e; \
	echo "Applying post-load preparation from tpch_prep.sql..."; \
	sudo -u "$(PGUSER)" psql -d "$(PGDATABASE)" -f "$(CURDIR)/tpch_prep.sql"
	@echo "Setup completed successfully."
#sudo mkdir /home/user01/workspace
#cd /home/user01/workspace
#sudo apt update
#sudo apt install build-essential
#git clone https://github.com/electrum/tpch-dbgen
#cd tpch-dbgen
#make
#./dbgen -s 1
#sudo -u postgres psql
#CREATE DATABASE tpch;
#\c tpch
#sudo -u postgres psql -d tpch -f tpch_schema.sql
#sed -i 's/|$//' *.tbl
#cd /home/user01/workspace/tpch-dbgen
#chmod a+r ~/workspace/tpch-dbgen/*.tbl
#nano load_tpch.sql
#\copy region   FROM 'region.tbl'   WITH (FORMAT csv, DELIMITER '|');
#\copy nation   FROM 'nation.tbl'   WITH (FORMAT csv, DELIMITER '|');
#\copy supplier FROM 'supplier.tbl' WITH (FORMAT csv, DELIMITER '|');
#\copy customer FROM 'customer.tbl' WITH (FORMAT csv, DELIMITER '|');
#\copy part     FROM 'part.tbl'     WITH (FORMAT csv, DELIMITER '|');
#\copy partsupp FROM 'partsupp.tbl' WITH (FORMAT csv, DELIMITER '|');
#\copy orders   FROM 'orders.tbl'   WITH (FORMAT csv, DELIMITER '|');
#\copy lineitem FROM 'lineitem.tbl' WITH (FORMAT csv, DELIMITER '|');
#cd ~/workspace/tpch-dbgen
#sudo -u postgres psql -d tpch -f load_tpch.sql
#cd ~/workspace
#sudo -u postgres psql -d tpch -f tpch_prep.sql
