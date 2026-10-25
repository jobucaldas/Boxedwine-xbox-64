#!/usr/bin/env bash
# B60: install/launch/pull-logs on João's Xbox from this Linux box via the
# documented Xbox Device Portal REST API. No Windows PC, no security bypass.
#   export DP_URL=https://<console-ip>:11443 DP_USER=... DP_PASS=...   (never printed)
#   ./deploy-devportal.sh info | install [out/BoxedwineDev.appx] | list | launch [App|BigFile]
#                         stop | files | pull [ladder-native.log ...] [--out DIR] | pull-logs [DIR]
#                         settings | set-setting NAME VALUE | screenshot | uninstall yes | reboot now
# Optional: DP_TLS_SHA256 pins the console's self-signed TLS cert (printed by `info`).
set -euo pipefail
set +x   # never trace: the environment holds the Device Portal password
exec python3 -I "$(cd -- "$(dirname -- "$0")" && pwd)/linux/devportal.py" "$@"
