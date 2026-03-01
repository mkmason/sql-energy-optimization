# ===== Makefile =====

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra
LDFLAGS = -lm
TARGET  = query_runner
SRC     = query_runner.c rapl.c
RUNNER_PREFIX ?= sudo

# Default target
all: compile

# Compile the program
compile:
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

# Run all queries
run: compile
	$(RUNNER_PREFIX) ./$(TARGET)

# Run a specific query (reminder-style target)
# Usage:
#   make run-single QUERY=APX1090
#   make run-single QUERY=APX1090-queryA.sql
run-single: compile
ifndef QUERY
	$(error You must provide QUERY. Example: make run-single QUERY=APX1090)
endif
	$(RUNNER_PREFIX) env QUERY_FILTER=$(QUERY) ./$(TARGET)

# Optional: clean build artifacts
clean:
	rm -f $(TARGET)

# ===== Sigless Simulation Controls =====

SIGLESS_BIN ?= sigless
SIGLESS_PORT ?= 8000
SIGLESS_OUT ?= ./logs
SIGLESS_PID_FILE := .sigless.pid

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