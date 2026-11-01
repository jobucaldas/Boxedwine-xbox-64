#!/usr/bin/env bash
# B70 O2 fix: pre-provision the CAs' own CRLs into a Wine prefix's cryptnet URL cache.
# Root cause (B70 O2 trace): Valve's steamclient (own OpenSSL HTTP stack) validates server
# certs through crypt32 CertVerifyRevocation; Wine's cryptnet then GETs the http:// CRL
# distribution point with timeout 0. This box only egresses TCP 443, so the :80 connect
# hangs and Valve's web API call dies at 20 s with "status = 0".
# Fix: fetch the same CRL path over HTTPS from the same CA host (curl, TLS verified), check
# it with openssl against the issuer from the served chain / system store, then commit it
# under its http:// URL with crl_seed.exe (ours). Revocation and TLS checks stay ON.
# CRLs that the CA does not serve over HTTPS are reported as crl_unavailable (not faked).
#
# Usage: seed-crl-cache.sh --prefix PFX --evidence DIR [host ...]
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
LOG=$EVID/crl-seed.log
WORK=$(mktemp -d /tmp/B70-crl-XXXXXX)
trap 'rm -rf "$WORK"' EXIT
: > "$LOG"
say() { echo "$*" | tee -a "$LOG"; }

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
