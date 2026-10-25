#!/usr/bin/env bash
# B60 offline test: Linux-built .appx structure/signature + deploy script vs a mock
# Device Portal. Linux box only; touches no console and uses throwaway creds.
#   usage: linux/test-linux-appx.sh   (after `make linux-appx`)
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
appx=out/BoxedwineDev.appx cer=out/test-cert.cer
test -s $appx || { echo "run make linux-appx first" >&2; exit 1; }
tmp=$(mktemp -d /tmp/b60-test.XXXXXX)
mock_pid=
cleanup() { [[ -n $mock_pid ]] && kill "$mock_pid" 2>/dev/null; rm -rf -- "$tmp"; }
trap cleanup EXIT
pass=0
ok() { pass=$((pass + 1)); echo "ok $pass - $*"; }
die() { echo "FAIL - $*" >&2; exit 1; }

echo '== 1/3 package structure + signature =='
(cd out && sha256sum --quiet -c BoxedwineDev.appx.sha256) || die 'appx sha256 sidecar'
ok 'sha256 sidecar verifies'
python3 linux/appx_check.py $appx --cert $cer --payload out/payload >"$tmp/check.log" 2>&1 \
    && die 'full payload (with combo/) must NOT equal the package' || true
grep -q 'package vs payload' "$tmp/check.log" || die 'expected only the combo/ difference'
mkdir "$tmp/staged" && tar -C out/payload --exclude=./combo -cf - . | tar -xf - -C "$tmp/staged"
python3 linux/appx_check.py $appx --cert $cer --payload "$tmp/staged" | tee "$tmp/check.log" | tail -1 | grep -qx 'APPX CHECK PASS' \
    || { cat "$tmp/check.log"; die 'appx_check on the built package'; }
ok 'block map hashes, content types, publisher, payload equality, osslsigncode verify'
unzip -Z1 $appx | grep -Eiq '\.pfx$|LocalState|token|^combo/' && die 'private/hosted file inside appx'
ok 'no .pfx/LocalState/token/combo inside the appx'

# Negative 1: one flipped byte in rungs.exe (re-zipped) must fail the block map check.
python3 - $appx "$tmp/tampered.appx" <<'PY'
import sys, zipfile
src, dst = sys.argv[1:]
with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as zo:
    for i in zi.infolist():
        d = bytearray(zi.read(i.filename))
        if i.filename == 'rungs.exe':
            d[4096] ^= 0xFF
        zo.writestr(i, bytes(d))
PY
python3 linux/appx_check.py "$tmp/tampered.appx" --cert $cer >"$tmp/neg1.log" 2>&1 && die 'tampered appx passed'
grep -q 'rungs.exe: block 0 hash mismatch' "$tmp/neg1.log" || { cat "$tmp/neg1.log"; die 'tamper not caught by block map'; }
ok 'tampered byte -> block map mismatch (FAIL as expected)'
# Negative 2: a signature from a different (CN=Other) key must not verify as CN=BoxedwineDev.
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=Other -keyout "$tmp/o.key" -out "$tmp/o.pem" \
    -addext extendedKeyUsage=codeSigning >/dev/null 2>&1
openssl pkcs12 -export -inkey "$tmp/o.key" -in "$tmp/o.pem" -passout pass: -out "$tmp/o.pfx"
python3 - "$appx" "$tmp/unsigned.appx" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as zi, zipfile.ZipFile(sys.argv[2], 'w') as zo:
    for i in zi.infolist():
        if i.filename != 'AppxSignature.p7x':
            zo.writestr(i, zi.read(i.filename))
PY
osslsigncode sign -pkcs12 "$tmp/o.pfx" -pass '' -in "$tmp/unsigned.appx" -out "$tmp/other.appx" >"$tmp/neg2-sign.log" 2>&1 \
    || { cat "$tmp/neg2-sign.log"; die 'could not produce the CN=Other control package'; }
python3 linux/appx_check.py "$tmp/other.appx" --cert $cer >"$tmp/neg2.log" 2>&1 && die 'foreign signature passed'
grep -Eq 'osslsigncode verify failed|signer subject' "$tmp/neg2.log" || { cat "$tmp/neg2.log"; die 'foreign signature not caught'; }
ok 'CN=Other signature -> verify/subject FAIL (as expected)'
# Negative 3: makemsix (validation parser on) must schema-reject a bad manifest.
makemsix=${MAKEMSIX:-$(command -v makemsix || linux/build-makemsix.sh)}
cp -r "$tmp/staged" "$tmp/badman"
sed -i 's/ProcessorArchitecture="x64"/ProcessorArchitecture="x64" Bogus="1"/' "$tmp/badman/AppxManifest.xml"
"$makemsix" pack -d "$tmp/badman" -p "$tmp/badman.appx" >"$tmp/neg3.log" 2>&1 && die 'invalid manifest packed'
grep -q "attribute 'Bogus' is not declared" "$tmp/neg3.log" || { cat "$tmp/neg3.log"; die 'no schema error'; }
ok 'makemsix schema-validates AppxManifest.xml (bogus attribute rejected; real manifest accepted)'

echo '== 2/3 deploy-devportal.sh vs mock Device Portal (HTTPS self-signed, Basic auth, CSRF) =='
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=mock-xbox -keyout "$tmp/tls.key" -out "$tmp/tls.pem" >/dev/null 2>&1
user=mockuser pw="mock-$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
printf '%s:%s\n' "$user" "$pw" | python3 linux/mock_devportal.py --cert "$tmp/tls.pem" --key "$tmp/tls.key" \
    --events "$tmp/events.json" --port-file "$tmp/port" &
mock_pid=$!
for _ in $(seq 50); do [[ -s $tmp/port ]] && break; sleep 0.1; done
export DP_URL="https://127.0.0.1:$(cat "$tmp/port")" DP_USER=$user DP_PASS=$pw
dp() { ./deploy-devportal.sh "$@" >>"$tmp/dp.out" 2>&1; }
dp info && grep -q '"DeviceType": "Windows.Xbox"' "$tmp/dp.out" || die 'info'
ok 'info (auth + device family + TLS fingerprint)'
dp install $appx && grep -q 'installed: Joao.Boxedwine.DevMode_0.1.0.0_x64__mockpublisherid' "$tmp/dp.out" || { cat "$tmp/dp.out"; die 'install'; }
python3 - "$tmp/events.json" "$(sha256sum $appx | cut -d' ' -f1)" <<'PY' || die 'uploaded bytes differ'
import json, sys
ev = [e for e in json.load(open(sys.argv[1])) if e.get('path') == '/api/app/packagemanager/package']
assert ev and ev[-1]['package'] == 'BoxedwineDev.appx', ev
assert ev[-1]['parts'][0][0] == 'BoxedwineDev.appx' and ev[-1]['parts'][0][2] == sys.argv[2], ev
PY
ok 'install: multipart upload byte-identical, CSRF accepted, 204->200 state polled'
dp list && grep -q '^Joao.Boxedwine.DevMode_' "$tmp/dp.out" || die 'list'
ok 'list'
dp launch && dp launch BigFile && dp stop || die 'launch/stop'
python3 - "$tmp/events.json" <<'PY' || die 'taskmanager args'
import json, sys
ev = [e for e in json.load(open(sys.argv[1])) if e.get('path') == '/api/taskmanager/app']
assert [(e['method'], e['appid']) for e in ev] == [('POST', 'Joao.Boxedwine.DevMode_mockpublisherid!App'), ('POST', 'Joao.Boxedwine.DevMode_mockpublisherid!BigFile'), ('DELETE', None)], ev
assert all(e['package'].startswith('Joao.Boxedwine.DevMode_') for e in ev) and ev[-1]['forcestop'] == 'yes'
PY
ok 'launch App, launch BigFile, stop (base64 appid/package)'
dp launch Nope && die 'unknown app id accepted' || ok 'unknown app id refused'
dp files && dp pull-logs "$tmp/pulled" && dp pull --out "$tmp/one" || die 'files/pull'
test -s "$tmp/one/ladder-native.log" || die 'pull default file'
grep -qx 'STAGE n10 mock PASS' "$tmp/pulled/ladder-native.log" && test -s "$tmp/pulled/ladder-App.log" || die 'pulled content'
ok 'files + pull ladder-native.log + pull-logs (all logs)'
dp pull ../escape && die 'path traversal accepted' || ok 'pull refuses path separators'
dp settings && grep -q 'DefaultApplicationType.*game/app-type candidate' "$tmp/dp.out" || die 'settings'
dp set-setting DefaultApplicationType Game && grep -q '"value": "Game"' "$tmp/events.json" || die 'set-setting'
ok 'settings list + set-setting (PUT /ext/settings/NAME)'
dp screenshot --out "$tmp/s.png" && head -c4 "$tmp/s.png" | grep -q PNG || die 'screenshot'
ok 'screenshot'
dp uninstall && die 'uninstall without confirmation ran' || ok 'uninstall needs "yes"'
dp reboot && die 'reboot without confirmation ran' || true
dp reboot now && grep -q '/api/control/restart' "$tmp/events.json" || die 'reboot now'
ok 'reboot needs "now", then POST /api/control/restart'
dp uninstall yes || die 'uninstall yes'
python3 - "$tmp/events.json" <<'PY' || die 'uninstall event'
import json, sys
ev = [e for e in json.load(open(sys.argv[1])) if e.get('path') == '/api/app/packagemanager/package' and e.get('method') == 'DELETE']
assert ev and ev[-1]['package'].startswith('Joao.Boxedwine.DevMode_'), ev
PY
ok 'uninstall yes -> DELETE package by full name'
DP_PASS=wrong ./deploy-devportal.sh info >"$tmp/bad.out" 2>&1 && die 'wrong password accepted'
grep -q '401 Unauthorized' "$tmp/bad.out" || die 'no 401 message'
ok 'wrong password -> 401 message, exit 1'
n_before=$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))))' "$tmp/events.json")
DP_TLS_SHA256=00ff ./deploy-devportal.sh info >"$tmp/pin.out" 2>&1 && die 'pin mismatch accepted'
n_after=$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))))' "$tmp/events.json")
[[ $n_before == "$n_after" ]] || die 'pin mismatch still sent a request'
grep -q 'does not match DP_TLS_SHA256' "$tmp/pin.out" || die 'no pin message'
fp=$(openssl x509 -in "$tmp/tls.pem" -outform DER | sha256sum | cut -d' ' -f1)
DP_TLS_SHA256=$fp ./deploy-devportal.sh info >>"$tmp/pin.out" 2>&1 || die 'correct pin rejected'
ok 'DP_TLS_SHA256 pin: mismatch refused, correct pin accepted'

echo '== 3/3 no credentials in any output =='
b64=$(printf '%s:%s' "$user" "$pw" | base64)
cat "$tmp"/*.out "$tmp/events.json" | grep -Fq -e "$pw" -e "$b64" && die 'credential leaked into output'
ok 'password and Basic header never appear in stdout/stderr/events'
echo "LINUX APPX TEST PASS ($pass checks; mock Device Portal only, no console)"
