#!/usr/bin/env python3
"""B50 O2: seed Valve's Steam client login state in the steam-min prefix.

The ONLY reader of the SteamClient refresh token (/workspace/steam-qr/tokens-client.json).
Writes, all chmod 600, all under the gitignored out/ prefix:
  Steam/config/loginusers.vdf          (account remembered, AllowAutoLogin)
  Steam/config/config.vdf              (Accounts -> SteamID), only if absent
  users/<u>/AppData/Local/Steam/local.vdf  (ConnectCache: crc32(account)+"1" ->
                                        hex(CryptProtectData(token, entropy=account)))
  HKCU\\Software\\Valve\\Steam AutoLoginUser/RememberPassword (via regedit /S)
The DPAPI blob is made by the prefix's own Wine CryptProtectData (dpapi_protect.exe,
ours); the token goes to it over stdin, never argv. Prints only "seeded=1".
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def write600(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.chmod(path, 0o600)


def vq(s):
    # VDF quoted string; account names/steamids are [A-Za-z0-9_], but be strict anyway
    if any(c in s for c in '"\\\r\n\0'):
        raise SystemExit("seed-login: refusing odd characters in account field")
    return '"%s"' % s


def wine_user(drive_c):
    users = [u for u in os.listdir(os.path.join(drive_c, "users")) if u.lower() != "public"]
    if len(users) != 1:
        raise SystemExit("seed-login: expected exactly one Wine user in prefix, got %d" % len(users))
    return users[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", default=os.path.join(HERE, "out", "pfx"))
    ap.add_argument("--token", default="/workspace/steam-qr/tokens-client.json")
    ap.add_argument("--helper", default=os.path.join(HERE, "out", "bin", "dpapi_protect.exe"))
    ap.add_argument("--wine", default=os.environ.get("WINE", "wine"))
    a = ap.parse_args()

    prefix = os.path.abspath(a.prefix)
    private_out = os.path.realpath(os.path.join(HERE, "out"))
    if os.path.commonpath([private_out, os.path.realpath(prefix)]) != private_out:
        raise SystemExit("seed-login: prefix must live under gitignored out/")
    os.umask(0o077)
    drive_c = os.path.join(prefix, "drive_c")
    steam = os.path.join(drive_c, "Program Files (x86)", "Steam")
    if not os.path.isfile(os.path.join(prefix, "system.reg")):
        raise SystemExit("seed-login: prefix not initialised (no system.reg)")
    if not os.path.isfile(os.path.join(steam, "steamclient64.dll")):
        raise SystemExit("seed-login: no Valve client tree in prefix (run fetch-valve-client.py)")

    with open(a.token, encoding="utf-8") as f:
        tok = json.load(f)
    account = tok["account_name"]
    steamid = tok["steamid"]
    refresh = tok["refresh_token"]
    if tok.get("platform") != "SteamClient":
        raise SystemExit("seed-login: token is not a SteamClient-platform token")
    if not steamid.isdigit():
        raise SystemExit("seed-login: bad steamid field")

    env = dict(os.environ, WINEPREFIX=prefix, WINEDEBUG="-all")
    # The prefix's own CryptProtectData, entropy = account name (Steam's ConnectCache format).
    p = subprocess.run([a.wine, a.helper], input=(account + "\n" + refresh).encode(),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    blob = p.stdout.decode("ascii", "replace").strip()
    del refresh
    if p.returncode != 0 or not blob or any(c not in "0123456789abcdef" for c in blob):
        sys.stderr.write("seed-login: dpapi_protect failed rc=%d\n" % p.returncode)
        return 1

    key = "%x1" % zlib.crc32(account.encode())
    user = wine_user(drive_c)
    write600(os.path.join(drive_c, "users", user, "AppData", "Local", "Steam", "local.vdf"),
             '"MachineUserConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n'
             '\t\t\t\t"ConnectCache"\n\t\t\t\t{\n\t\t\t\t\t%s\t\t%s\n\t\t\t\t}\n'
             '\t\t\t}\n\t\t}\n\t}\n}\n' % (vq(key), vq(blob)))

    write600(os.path.join(steam, "config", "loginusers.vdf"),
             '"users"\n{\n\t%s\n\t{\n'
             '\t\t"AccountName"\t\t%s\n\t\t"PersonaName"\t\t%s\n'
             '\t\t"RememberPassword"\t\t"1"\n\t\t"WantsOfflineMode"\t\t"0"\n'
             '\t\t"SkipOfflineModeWarning"\t\t"0"\n\t\t"AllowAutoLogin"\t\t"1"\n'
             '\t\t"MostRecent"\t\t"1"\n\t\t"Timestamp"\t\t"%d"\n\t}\n}\n'
             % (vq(steamid), vq(account), vq(account), int(time.time())))

    cfg = os.path.join(steam, "config", "config.vdf")
    if not os.path.exists(cfg):
        write600(cfg, '"InstallConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n'
                      '\t\t\t\t"Accounts"\n\t\t\t\t{\n\t\t\t\t\t%s\n\t\t\t\t\t{\n\t\t\t\t\t\t"SteamID"\t\t%s\n'
                      '\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n' % (vq(account), vq(steamid)))
    else:
        os.chmod(cfg, 0o600)

    # Registry via a temp .reg (account name never on a command line), deleted after.
    steam_win = "C:\\\\Program Files (x86)\\\\Steam"
    reg = ('Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\Valve\\Steam]\r\n'
           '"AutoLoginUser"=%s\r\n"RememberPassword"=dword:00000001\r\n'
           '"SteamExe"="%s\\\\steam.exe"\r\n"SteamPath"="%s"\r\n"SourceModInstallPath"="%s\\\\steamapps\\\\sourcemods"\r\n'
           % (vq(account), steam_win, steam_win, steam_win))
    os.makedirs(os.path.join(HERE, "out"), exist_ok=True)
    fd, regpath = tempfile.mkstemp(suffix=".reg", dir=os.path.join(HERE, "out"))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(reg)
        r = subprocess.run([a.wine, "regedit", "/S", regpath], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    finally:
        os.unlink(regpath)
    if r.returncode != 0:
        sys.stderr.write("seed-login: regedit failed rc=%d\n" % r.returncode)
        return 1
    subprocess.run(["wineserver", "-w"], env=env)
    for f in ("user.reg", "system.reg", "userdef.reg"):
        if os.path.exists(os.path.join(prefix, f)):
            os.chmod(os.path.join(prefix, f), 0o600)
    print("seeded=1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
