#!/usr/bin/env bash
# Real-credential launcher staging is prohibited by the lane orders.
# Fail before any builds, token reads, copies, or network activity.
set -euo pipefail
printf '%s\n' 'BLOCKED: owned downloader proof disabled: plaintext refresh-token staging is not authorized.' >&2
exit 1
