#!/bin/sh
set -eu

QUERY_FILTER="${1:-}"
LOOPS_VALUE="${2:-}"

MAKE_ARGS=""
if [ -n "$QUERY_FILTER" ]; then
    MAKE_ARGS="$MAKE_ARGS QUERY=$QUERY_FILTER"
fi
if [ -n "$LOOPS_VALUE" ]; then
    MAKE_ARGS="$MAKE_ARGS LOOPS=$LOOPS_VALUE"
fi

if [ -n "$QUERY_FILTER" ]; then
    exec make run-temp-controlled-single $MAKE_ARGS
fi

exec make run-temp-controlled $MAKE_ARGS
