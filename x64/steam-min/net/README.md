# Box prefix TLS setup (B70 O2)

`fix-prefix-tls.sh --prefix PREFIX --evidence DIR` provisions a Wine prefix;
`run-minimal.sh` calls it after wineboot. This is a Linux box configuration for
443-only egress, not an Xbox network configuration or console proof.

The script fetches genuine CA CRLs over verified HTTPS and seeds Wine's URL
cache under their original distribution URLs. OpenSSL and the small
`crl_seed.c` helper verify the issuer signature; the helper rejects expired
CRLs and reads each cache entry back through cryptnet. Nothing changes Valve
binaries, certificate trust, hostname checks, or revocation policy.

The prefix's WinINet HTTP proxy points to a closed loopback port so unavailable
plain HTTP CRL requests fail promptly instead of exceeding Valve's request
timeout. HTTPS remains direct. This deliberately makes all WinINet plain HTTP
unavailable in that prefix; use it only on the box with blocked port 80.
Some intermediate CRLs are unavailable over HTTPS, and the diagnostic reports
their revocation status as unknown (`0x01000000`); Valve's unchanged verifier
decides whether that is acceptable. This is not proof of complete chain
revocation coverage. Re-provision fresh prefixes to refresh expiring CRLs.

Reproduce without credentials:

```sh
bash x64/steam-min/net/run-https-probe.sh --stock --evidence /tmp/B70-https
bash x64/steam-min/net/run-steamcmd-anon.sh --stock --evidence /tmp/B70-anon
```

The anonymous runner obtains unmodified packages from Valve, checks their
manifest SHA-256 values, uses a fresh prefix, redacts identifiers, and removes
the prefix on exit. Its success requires a successful GetCMListForConnect,
no status-zero failures, and a completed anonymous logon. These are stock Wine
tests; no THROUGH-uwph or device result is implied.
