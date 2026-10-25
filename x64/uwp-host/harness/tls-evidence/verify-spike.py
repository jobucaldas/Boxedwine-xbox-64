#!/usr/bin/env python3
"""Fail closed on the Linux THROUGH-uwph TLS spike evidence."""
import pathlib
import re
import sys


def verify(evidence, provider):
    def read(name):
        return (evidence / name).read_text()

    def require(ok, message):
        if not ok:
            raise ValueError(message)

    for mode, expected in (("deny-provider", 0), ("cert-control", 30), ("deny-only", 1)):
        require(read(mode + ".exit").strip() == str(expected),
                f"{mode}: expected exit {expected}")
    positive = read("deny-provider.log")
    require(re.search(r"^PASS HTTPS .* status=200 bytes=[1-9][0-9]*$", positive, re.M),
            "deny-provider: missing HTTP 200")
    require("SUMMARY net_probe pass=7 fail=0" in positive,
            "deny-provider: expected pass=7 fail=0")
    expected = ["badssl.com status=200"] + [
        f"{host}.badssl.com error=12157 flags=0x0"
        for host in ("self-signed", "expired", "untrusted-root", "wrong.host")]
    require(read("cert-control.log").splitlines() == expected,
            "cert-control: expected good-host success and all four certificate rejections")
    causes = re.findall(r"cert verify failed: (\d+)", read("cert-control-debug.log"))
    require(causes == ["12045", "12037", "12045", "12038"],
            "cert-control: unexpected certificate verification causes")
    control = read("deny-only.log")
    require(re.search(r"^FAIL HTTPS .* status=0 bytes=0$", control, re.M)
            and "SUMMARY net_probe pass=5 fail=2" in control
            and "winhttp error 2148074245" in control,
            "deny-only: expected absent-provider failure (0x80090305)")
    require("UWPH_DENY_DL=gnutls: refused" in read("deny-only-debug.log"),
            "deny-only: missing library refusal")
    for mode in ("deny-provider", "cert-control"):
        trace = read(mode + ".exec")
        # Inspect successful opens at every path, including non-system installs.
        for line in trace.splitlines():
            if "openat(" not in line or not re.search(r"= \d+", line):
                continue
            paths = re.findall(r'"([^"\n]+)"', line)
            for path in paths:
                if re.match(r"lib(?:gnutls|nettle|hogweed|gmp|tasn1|p11-kit|ssl|crypto)(?:[.-]|$)",
                            pathlib.Path(path).name):
                    require(path == provider, f"{mode}: forbidden TLS dependency open: {path}")
        require(provider in trace, f"{mode}: missing provider file open")
        require(re.search(r"UWPH_TLS_PROVIDER: .*\(loaded\)", read(mode + "-debug.log")),
                f"{mode}: missing provider-routed dlopen")


if __name__ == "__main__":
    try:
        verify(pathlib.Path(sys.argv[1]), sys.argv[2])
    except (OSError, ValueError) as error:
        sys.exit(f"FAIL TLS spike: {error}")
    print("PASS TLS spike: HTTP 200, certificate rejection, deny-only and provider isolation")
