#!/usr/bin/env bash
# Build-only regression: exercise the real verifier on a disposable kit copy.
set -euo pipefail
on_device=$(cd -- "$(dirname -- "$0")/.." && pwd)
scratch=$(mktemp -d /tmp/test-verify-package.XXXXXX)
trap 'rm -rf -- "$scratch"' EXIT

# Build the actual package rather than inventing an inventory fixture.
if ! make --no-print-directory -C "$on_device" kit >"$scratch/build.log" 2>&1; then
    cat "$scratch/build.log" >&2
    exit 1
fi
cp -a -- "$on_device/out/kit" "$scratch/kit"
make --no-print-directory -C "$on_device" verify-package PACKAGE_DIR="$scratch/kit"
printf 'test-verify-package: intact copy PASS\n'

# Choose a listed ordinary file, preserving XML and the inventory so that the
# negative case reaches the contents check rather than failing XML parsing.
victim=$(python3 -I - "$scratch/kit" <<'PY'
import sys
from pathlib import Path

kit = Path(sys.argv[1])
for line in (kit / 'kit-manifest.txt').read_text().splitlines():
    fields = line.split(' | ')
    name = fields[0]
    if ('MISSING (optional)' not in fields[2:]
            and name not in ('kit-manifest.txt', 'payload/AppxManifest.xml')
            and (kit / name).is_file()):
        print(name)
        break
else:
    sys.exit('no listed file available for deletion')
PY
)
rm -- "$scratch/kit/$victim"
if make --no-print-directory -C "$on_device" verify-package \
    PACKAGE_DIR="$scratch/kit" >"$scratch/missing.log" 2>&1; then
    cat "$scratch/missing.log" >&2
    printf 'test-verify-package: FAIL: verifier accepted a missing listed file\n' >&2
    exit 1
fi
if ! grep -Fx -- "verify-package: missing listed file: $victim" "$scratch/missing.log"; then
    cat "$scratch/missing.log" >&2
    printf 'test-verify-package: FAIL: missing-file diagnostic absent\n' >&2
    exit 1
fi
printf 'test-verify-package: deleted listed file rejected PASS\n'
printf 'test-verify-package: PASS (build-only; no Wine or device activation)\n'
