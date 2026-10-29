#!/bin/sh
# SPDX-License-Identifier: MIT
# Synthetic localhost only; no physical scanner is contacted.
command -v python3 >/dev/null 2>&1 || exit 77
exec python3 -B "$srcdir/test_network.py"
