#!/usr/bin/env python3
"""Regression checks for fail-closed spike assertions, without network access."""
import importlib.util
import pathlib
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "verify_spike", pathlib.Path(__file__).with_name("verify-spike.py"))
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)
PROVIDER = "/provider/libgnutls.so.30"


class SpikeChecks(unittest.TestCase):
    def test_security_controls(self):
        files = {
            "deny-provider.exit": "0", "cert-control.exit": "30", "deny-only.exit": "1",
            "deny-provider.log": "PASS HTTPS probe status=200 bytes=825\nSUMMARY net_probe pass=7 fail=0\n",
            "cert-control.log": "badssl.com status=200\n" + "".join(
                f"{host}.badssl.com error=12157 flags=0x0\n"
                for host in ("self-signed", "expired", "untrusted-root", "wrong.host")),
            "cert-control-debug.log": "".join(
                f"cert verify failed: {cause}\n" for cause in (12045, 12037, 12045, 12038)),
            "deny-only.log": "FAIL HTTPS probe status=0 bytes=0\nSUMMARY net_probe pass=5 fail=2\nwinhttp error 2148074245\n",
            "deny-only-debug.log": "UWPH_DENY_DL=gnutls: refused\n",
            "deny-provider-debug.log": "",
        }
        for mode in ("deny-provider", "cert-control"):
            files[mode + ".exec"] = f'openat(AT_FDCWD, "{PROVIDER}", O_RDONLY) = 7\n'
            files[mode + "-debug.log"] += "UWPH_TLS_PROVIDER: provider (loaded)\n"
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for name, value in files.items():
                (root / name).write_text(value)
            validator.verify(root, PROVIDER)
            # Every required evidence file is mandatory, even if the rest passes.
            for name in files:
                with self.subTest(missing=name):
                    (root / name).write_text("")
                    with self.assertRaises(ValueError):
                        validator.verify(root, PROVIDER)
                    (root / name).write_text(files[name])
            for host in ("self-signed", "expired", "untrusted-root", "wrong.host"):
                with self.subTest(accepted=host):
                    name = "cert-control.log"
                    (root / name).write_text(files[name].replace(
                        f"{host}.badssl.com error=12157 flags=0x0",
                        f"{host}.badssl.com status=200"))
                    with self.assertRaises(ValueError):
                        validator.verify(root, PROVIDER)
                    (root / name).write_text(files[name])
            for library in ("gnutls", "nettle", "hogweed", "gmp", "tasn1", "p11-kit", "ssl", "crypto"):
                with self.subTest(host_library=library):
                    (root / "deny-provider.exec").write_text(files["deny-provider.exec"] +
                        f'openat(AT_FDCWD, "/opt/host/lib{library}.so.1", O_RDONLY) = 8\n')
                    with self.assertRaises(ValueError):
                        validator.verify(root, PROVIDER)


if __name__ == "__main__":
    unittest.main()
