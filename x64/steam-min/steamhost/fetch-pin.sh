#!/usr/bin/env bash
# Fetch only the pinned, unmodified Valve package; never execute client code.
set -euo pipefail
exec python3 "$(dirname "$0")/fetch-pin.py" "$@"
