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