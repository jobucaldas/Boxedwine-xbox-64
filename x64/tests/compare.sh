#!/bin/sh
# Compares the x86-64 conformance tests (x64/tests/out/t_*) between a native host run
# and a guest run under BoxedWine's x64 interpreter (x64/run-demo.sh /bin/<name>).
#
# Usage: x64/tests/compare.sh [t_name ...]     (default: every t_* binary in out/)
# Per run: stdout is compared after stripping BoxedWine's header (up to and including
# the "Launching " line) and its "Boxedwine shutdown" footer. Lines containing "x64:"
# (interpreter errors) are listed for each test.
# Exit status: 0 if every selected test passed, 1 if any failed or timed out, 2 on setup error.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RUN_DEMO="$REPO/x64/run-demo.sh"
BIN_DIR="$HERE/out"
TIMEOUT_SECS=60
WORK=$(mktemp -d "${TMPDIR:-/tmp}/x64-compare.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT INT TERM

# Build once so compile errors are reported directly (run-demo.sh also rebuilds on each call).
if ! OUT="$BIN_DIR" "$HERE/build.sh" >"$WORK/build.log" 2>&1; then
    echo "build failed:"
    cat "$WORK/build.log"
    exit 2
fi

if [ "$#" -gt 0 ]; then
    names="$*"
else
    names=""
    for f in "$BIN_DIR"/t_*; do
        [ -f "$f" ] && [ -x "$f" ] && names="$names $(basename "$f")"
    done
fi
if [ -z "$names" ]; then
    echo "no t_* test binaries found in $BIN_DIR"
    exit 2
fi

summary="$WORK/summary"
: >"$summary"
overall=0

for name in $names; do
    bin="$BIN_DIR/$name"
    echo "=== $name ==="
    if [ ! -x "$bin" ]; then
        echo "MISSING  $name (no executable $bin)"
        printf '%s MISSING 0 0 0\n' "$name" >>"$summary"
        overall=1
        continue
    fi

    # Native run: stdout only, exit status recorded.
    timeout "$TIMEOUT_SECS" "$bin" >"$WORK/$name.native" 2>"$WORK/$name.native.err"
    native_rc=$?

    # BoxedWine run: stdout and stderr merged, so header, guest output and any "x64:" lines are all visible.
    timeout "$TIMEOUT_SECS" "$RUN_DEMO" "/bin/$name" >"$WORK/$name.raw" 2>&1
    guest_rc=$?

    # Strip BoxedWine's header (through the "Launching " line) and the "Boxedwine shutdown" footer.
    awk '
        !started { if (/^Launching /) started = 1; next }
        /^Boxedwine shutdown$/ { exit }
        { print }
    ' "$WORK/$name.raw" >"$WORK/$name.guest"

    grep 'x64:' "$WORK/$name.raw" >"$WORK/$name.x64" 2>/dev/null
    x64_count=$(wc -l <"$WORK/$name.x64" | tr -d ' ')
    native_lines=$(wc -l <"$WORK/$name.native" | tr -d ' ')
    guest_lines=$(wc -l <"$WORK/$name.guest" | tr -d ' ')

    if [ "$native_rc" -ne 0 ]; then
        echo "NOTE: native run exited with status $native_rc (expected 0)"
    fi

    if [ "$guest_rc" -eq 124 ]; then
        status=TIMEOUT
    elif cmp -s "$WORK/$name.native" "$WORK/$name.guest"; then
        status=PASS
    else
        status=FAIL
    fi

    if [ "$status" = PASS ]; then
        echo "PASS  $name  (native $native_lines lines, guest $guest_lines lines)"
    else
        echo "$status  $name  (native $native_lines lines, guest $guest_lines lines, boxedwine exit $guest_rc)"
        if [ "$status" = FAIL ]; then
            echo "--- diff native vs boxedwine (first 20 lines) ---"
            diff -u --label native --label boxedwine "$WORK/$name.native" "$WORK/$name.guest" | head -n 20
            echo "--- end diff ---"
        fi
        overall=1
    fi

    if [ "$x64_count" -gt 0 ]; then
        echo "x64: lines ($x64_count):"
        sed 's/^/    /' "$WORK/$name.x64"
    fi
    printf '%s %s %s %s %s\n' "$name" "$status" "$native_lines" "$guest_lines" "$x64_count" >>"$summary"
done

echo
echo "SUMMARY"
printf '%-16s %-8s %-12s %-12s %-10s\n' test result native_lines guest_lines x64_errors
while read -r n s nl gl xe; do
    printf '%-16s %-8s %-12s %-12s %-10s\n' "$n" "$s" "$nl" "$gl" "$xe"
done <"$summary"

exit "$overall"
