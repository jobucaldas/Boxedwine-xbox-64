#!/usr/bin/env bash
# B70 O2 fix for Valve's client HTTPS ("GetCMListForConnect -- web API call failed
# (status = 0)") in the Wine prefixes run-minimal.sh / run-https-probe.sh / run-steamcmd-anon.sh
# build. Environment + Wine config only; TLS chain, hostname and revocation checks stay ON.
#
# Root cause (B70 O2 +cryptnet trace): Valve's steamclient (its own OpenSSL HTTP stack, not
# WinHTTP) validates server certs through crypt32 CertVerifyRevocation. Wine's cryptnet then
# GETs each cert's http:// CRL distribution point (Let's Encrypt *.c.lencr.org) through
# wininet with timeout 0. This box egresses only TCP 443; :80 is blackholed, so the connect
# hangs ~35 s per CRL and Valve's web API call dies at 20 s with status 0.
#
# Fix, two parts:
#  1. CRLs: fetch the same CRL path over HTTPS from the same CA host (curl, TLS verified),
#     verify it with openssl against the issuer, and commit it under its http:// URL into the
#     prefix's URL cache with crl_seed.exe (ours; re-checks issuer name, signature, expiry).
#     cryptnet reads that cache first, so leaf revocation is really checked offline-free.
#     CRLs the CA does not serve over HTTPS (root-signed yr./ye.c.lencr.org) are logged as
#     crl_unavailable; nothing is faked.
#  2. No :80 on this network: set the prefix's WinINet proxy for the http scheme only to a
#     closed loopback port, so a remaining plain-HTTP fetch fails at once
#     (CRYPT_E_REVOCATION_OFFLINE, the same result a firewall RST gives on Windows) instead of
#     hanging. https:// and WinHTTP are untouched; Valve's own policy decides what to do.
#
# Usage: fix-prefix-tls.sh --prefix PFX --evidence DIR [host ...]
#   default hosts: Steam web API/community/store/login/crash + every websockets CM in
#   GetCMListForConnect. Needs wine, openssl, curl, mingw. Linux box only; the console's
#   network differs (B44 covers Xbox TLS).
set -euo pipefail
PFX= EVID=
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) PFX=$2; shift 2 ;;
        --evidence) EVID=$2; shift 2 ;;
        --) shift; break ;;
        -*) echo "seed-crl-cache.sh: unknown arg $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
[ -n "$PFX" ] && [ -n "$EVID" ] || { echo "usage: $0 --prefix PFX --evidence DIR [host ...]" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$EVID"; EVID=$(cd "$EVID" && pwd)
LOG=$EVID/prefix-tls-fix.log
WORK=$(mktemp -d /tmp/B70-crl-XXXXXX)
trap 'rm -rf "$WORK"' EXIT
: > "$LOG"
say() { echo "$*" | tee -a "$LOG"; }

# --- 2 (applied first so it holds even if CRL seeding is partial): no-:80 fast fail -----
FASTFAIL_PORT=9
if (exec 3<>/dev/tcp/127.0.0.1/$FASTFAIL_PORT) 2>/dev/null; then
    say "http80_fastfail=0 reason=127.0.0.1:$FASTFAIL_PORT is listening"; exit 1
fi
KEY='HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings'
WINEPREFIX=$PFX WINEDEBUG=-all wine reg add "$KEY" /v ProxyEnable /t REG_DWORD /d 1 /f >/dev/null
WINEPREFIX=$PFX WINEDEBUG=-all wine reg add "$KEY" /v ProxyServer /t REG_SZ /d "http=127.0.0.1:$FASTFAIL_PORT" /f >/dev/null
WINEPREFIX=$PFX wineserver -w
say "http80_fastfail=1 wininet_proxy=http=127.0.0.1:$FASTFAIL_PORT (closed; https direct)"

# --- 1. CRLs over HTTPS -> prefix URL cache -----------------------------------------------

HOSTS=("$@")
if [ ${#HOSTS[@]} = 0 ]; then
    HOSTS=(api.steampowered.com steamcommunity.com store.steampowered.com login.steampowered.com crash.steampowered.com)
    curl -fsS -m 20 -o "$WORK/dir.json" 'https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0' || :
    if [ -s "$WORK/dir.json" ]; then
        mapfile -t CMS < <(python3 -I -c '
import json,sys
for e in json.load(open(sys.argv[1]))["response"]["serverlist"]:
    if e.get("type")=="websockets": print(e["endpoint"].rsplit(":",1)[0])' "$WORK/dir.json" | sort -u)
        HOSTS+=("${CMS[@]}")
    fi
fi
say "hosts=${#HOSTS[@]}"

x86_64-w64-mingw32-gcc -municode -O2 -Wall -Wextra "$HERE/crl_seed.c" -o "$WORK/crl_seed.exe" \
    -lwininet -lcrypt32 -lcryptnet

# issuer DER for cert $1 (PEM): next cert in the served chain whose subject matches, else system store
find_issuer() {
    local cert=$1 dir=$2 want c
    want=$(openssl x509 -in "$cert" -noout -issuer -nameopt RFC2253)
    want=${want#issuer=}
    for c in "$dir"/c*.pem; do
        [ "$(openssl x509 -in "$c" -noout -subject -nameopt RFC2253)" = "subject=$want" ] || continue
        openssl x509 -in "$c" -outform DER -out "$cert.issuer.der"; return 0
    done
    c=/etc/ssl/certs/$(openssl x509 -in "$cert" -noout -issuer_hash).0
    [ -f "$c" ] && openssl x509 -in "$c" -outform DER -out "$cert.issuer.der" && return 0
    return 1
}

winpath() { local p=$1; echo "Z:${p//\//\\}"; }  # Wine's default Z: = /

declare -A SEEN
ARGS=()
n=0
for h in "${HOSTS[@]}"; do
    n=$((n + 1)); d=$WORK/h$n; mkdir -p "$d"
    echo | timeout 15 openssl s_client -connect "$h:443" -servername "$h" -showcerts 2>/dev/null \
        | awk -v d="$d" '/-----BEGIN CERTIFICATE-----/{i++; f=sprintf("%s/c%02d.pem",d,i)} f{print > f} /-----END CERTIFICATE-----/{f=""}'
    ls "$d"/c*.pem >/dev/null 2>&1 || { say "host=$h chain=unavailable"; continue; }
    for c in "$d"/c*.pem; do
        subj=$(openssl x509 -in "$c" -noout -subject -nameopt RFC2253); subj=${subj#subject=}
        iss=$(openssl x509 -in "$c" -noout -issuer -nameopt RFC2253); iss=${iss#issuer=}
        [ "$subj" = "$iss" ] && continue  # self-signed root: never revocation-checked
        for url in $(openssl x509 -in "$c" -noout -ext crlDistributionPoints 2>/dev/null | sed -n 's/^ *URI:\(http:\/\/.*\)$/\1/p'); do
            [ -n "${SEEN[$url]:-}" ] && continue
            SEEN[$url]=1
            k=${#SEEN[@]}
            if ! find_issuer "$c" "$d"; then say "crl_unavailable url=$url reason=no_issuer"; continue; fi
            if ! curl -fsS -m 30 -o "$WORK/$k.crl" "https://${url#http://}" 2>>"$WORK/curl.err"; then
                say "crl_unavailable url=$url reason=https_fetch_failed"; continue
            fi
            openssl x509 -inform DER -in "$c.issuer.der" -out "$WORK/$k.issuer.pem"
            if ! openssl crl -inform DER -in "$WORK/$k.crl" -CAfile "$WORK/$k.issuer.pem" -noout 2>&1 | grep -q 'verify OK'; then
                say "crl_unavailable url=$url reason=openssl_verify_failed"; continue
            fi
            cp "$c.issuer.der" "$WORK/$k.issuer.der"
            ARGS+=("$url" "$(winpath "$WORK/$k.crl")" "$(winpath "$WORK/$k.issuer.der")")
            say "crl_fetched url=$url via=https openssl_verify=ok subject=$subj"
        done
    done
done

[ ${#ARGS[@]} -gt 0 ] || { say "crl_seed_total=0 ok=0"; exit 1; }
WINEPREFIX=$PFX WINEDEBUG=-all wine "$WORK/crl_seed.exe" "${ARGS[@]}" | tr -d '\r' | tee -a "$LOG" || :
WINEPREFIX=$PFX wineserver -w
# success = every CRL we could fetch over HTTPS was verified and is a cache hit
grep -Eq '^crl_seed_total=([0-9]+) ok=\1$' "$LOG"
