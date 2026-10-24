#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
logs=/workspace/x64-run/v2/R4H
printf 'O7 acceptance replay on Linux x86-64; NOT Xbox Dev Mode acceptance.\n'
printf '$ make -C x64/uwp-host/harness backend-test (live THROUGH uwph)\n'
make backend-test
sleep 4
printf '\nSaved longer-run routing results: %s/O7-regression.log\n' "$logs"
grep -E 'checks:|counter verification:|backend=|ntdll=|verification: BLOCKED|Remaining bypasses:|Separate stock' "$logs/O7-regression.log"
sleep 5
printf '\nWindows THROUGH uwph, hosted by stock Wine; saved O6 full suite:\n'
printf '%s/O6-full.log\n' "$logs"
grep -E 'passed.*failed|futex SIGUSR1: PASS|CreateFile2' "$logs/O6-full.log" | tail -5
printf '\nAll order recordings: /workspace/x64-run/videos/R4H-O1.mp4 through O7.mp4\n'
printf 'Overall acceptance BLOCKED: wineboot attempts; no DONE-R4H.\n'
printf 'Replay exit 0 describes successful display, not passing Wine acceptance.\n'
sleep 5
