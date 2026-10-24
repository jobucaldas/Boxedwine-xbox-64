#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mode=${1:---futex-only}
[[ $mode == --futex-only || $mode == --full || $mode == --file-only ]] || exit 2
# Explicit desktop-only artifact: exception hook is never enabled in UWP archive.
make -C .. OUT=harness/out/windows-desktop CFLAGS_COMMON='-O2 -g -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Iinclude -DUWPH_DESKTOP_SIGNAL_TEST' harness/out/windows-desktop/win/smoke.exe
# Serialize use of the owned fixture with O4. No normal user prefix is used.
exec 9>out/wine-o4/lock
flock -n 9 || { echo 'Owned Wine fixture already in use' >&2; exit 1; }
export WINEPREFIX=$PWD/out/wine-o4/prefix
[[ -f $WINEPREFIX/system.reg ]] || { echo 'Run wine-test to prepare the dedicated initialized fixture'; exit 1; }
export WINEDEBUG=-all WINEDLLOVERRIDES=wineboot.exe=d
unset LD_PRELOAD UWPH_REPORT
printf 'Windows smoke: THROUGH uwph Windows code, hosted by stock Wine (no Unix ntdll routing). Desktop test-only artifact.\n'
fixture_root=$(realpath ../tests)
# Convert setup paths with this owned prefix's drive mapping, before PE argv.
windows_root=$(timeout 10s winepath -w "$fixture_root")
[[ $windows_root =~ ^[A-Za-z]:\\ ]] || { echo "Not an absolute Windows fixture root: $windows_root" >&2; exit 1; }
printf 'Host CWD=%s\nFixture Unix=%s\nFixture Windows=%s\n' "$PWD" "$fixture_root" "$windows_root"
if [[ $mode == --futex-only ]]; then
    timeout --kill-after=2s 20s /usr/lib/wine/wine64 out/windows-desktop/win/smoke.exe --futex-only
else
    timeout --kill-after=2s 20s /usr/lib/wine/wine64 out/windows-desktop/win/smoke.exe "$windows_root" "$mode"
fi
