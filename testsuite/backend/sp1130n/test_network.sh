#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later WITH SANE-exception
# Synthetic localhost only; no physical scanner is contacted.
command -v python3 >/dev/null 2>&1 || exit 77
exec python3 -B "$srcdir/test_network.py"
