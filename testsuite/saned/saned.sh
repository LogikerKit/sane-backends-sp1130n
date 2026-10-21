#!/bin/sh -u
# testsuite/saned/saned.sh -- saned end-to-end regression test
# Copyright (C) 2026  Sane Developers.
#
# License: GPL-3.0+
#
# Starts saned in standalone mode serving the test backend and runs
# scanimage -T against it.

SANED=../../frontend/saned
SCANIMAGE=../../frontend/scanimage
READY_TIMEOUT=10
TEST_TIMEOUT=60

export SANE_CONFIG_DIR=$(mktemp -d)
LD_LIBRARY_PATH="$(cd ../../backend/.libs && pwd)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH
SANED_LOG="$SANE_CONFIG_DIR/saned.log"

SANED_PID=
TEST_FAILED=

cleanup() {
    if test -n "$SANED_PID"; then
        kill "$SANED_PID" 2>/dev/null
        wait "$SANED_PID" 2>/dev/null
    fi

    if test -n "$TEST_FAILED" && test -f "$SANED_LOG"; then
        echo "--- saned log ---" >&2
        cat "$SANED_LOG" >&2
    fi

    test -n "$SANE_CONFIG_DIR" && rm -rf "$SANE_CONFIG_DIR"
}

trap cleanup EXIT
trap 'exit 1' INT TERM

if test ! -x "$SANED"; then
    echo "saned test: saned not built, skipping"
    exit 77
fi

if test ! -f ../../backend/libsane-test.la; then
    echo "saned test: test backend not built, skipping"
    exit 77
fi

printf 'net\ntest\n' > "$SANE_CONFIG_DIR/dll.conf"
printf 'resolution 50.0\n' > "$SANE_CONFIG_DIR/test.conf"
printf '' > "$SANE_CONFIG_DIR/saned.conf"

"$SANED" -l -o -d 128 -e -b 127.0.0.1 -p 0 > "$SANED_LOG" 2>&1 &
SANED_PID=$!

# Poll the log for readiness and the assigned port
SANED_PORT=
i=0
until test -n "$SANED_PORT"; do
    SANED_PORT=$(sed -n 's/.*selected ephemeral port: \([0-9][0-9]*\)$/\1/p' "$SANED_LOG" 2>/dev/null | tail -n 1)
    i=$((i + 1))
    if test "$i" -gt "$READY_TIMEOUT"; then
        TEST_FAILED=1
        echo "saned test: saned not listening or no 'selected ephemeral port' line in saned log after $READY_TIMEOUT seconds" >&2
        exit 1
    fi
    sleep 1
done

# Test whether a second saned on the same port exits cleanly.
status=0
timeout "$READY_TIMEOUT" "$SANED" -l -o -d 128 -e -b 127.0.0.1 -p "$SANED_PORT" >/dev/null 2>&1 || status=$?
if test "$status" -ne 1; then
    echo "saned test: second saned on a busy port exited with status $status, expected 1" >&2
    exit 1
fi

printf 'localhost:%s\n' "$SANED_PORT" > "$SANE_CONFIG_DIR/net.conf"

# Full scanimage test.
if timeout "$TEST_TIMEOUT" "$SCANIMAGE" -d "net:localhost:$SANED_PORT:test" -T; then
    :
else
    status=$?
    TEST_FAILED=1
    if test "$status" -ge 124; then
        echo "saned test: scanimage -T timed out or was interrupted" >&2
    else
        echo "saned test: scanimage -T failed" >&2
    fi
    exit "$status"
fi
