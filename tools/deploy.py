"""Push wiiustream.wps to the console over ftpiiu.

Same approach as the bk-wiiu dashboard's deploy: ftpiiu exposes the SD card
under different roots depending on the build, so try each until one accepts a
CWD rather than hard-coding one. The card never leaves the console.

    python tools/deploy.py [host]
"""

import ftplib
import os
import socket
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLUGIN = ROOT / "build" / "wiiustream.wps"

FTP_PORT = 21
# Roots ftpiiu is known to serve the SD card under. The first is what the
# bk-wiiu deploy already found working on this console.
SD_ROOTS = [
    "/fs/vol/external01",
    "/storage_sdcard",
    "/sd",
    "",
]
PLUGIN_DIR = "/wiiu/environments/aroma/plugins"

# Set WIIU_HOST to your console's address, or pass it on the command line.
# Anything listed here is only a starting guess for the FTP probe below.
DEFAULT_HOSTS = [h for h in (os.environ.get("WIIU_HOST"),) if h]


def find_console(hosts):
    """Return the first host answering on the FTP port."""
    for host in hosts:
        try:
            with socket.create_connection((host, FTP_PORT), timeout=1.5):
                return host
        except OSError:
            continue
    return None


def scan_subnet():
    """Last resort: the console's DHCP lease may have moved. Sweep the /24 the
    PC is on for anything answering on 21."""
    try:
        local = socket.gethostbyname(socket.gethostname())
    except OSError:
        return []
    prefix = local.rsplit(".", 1)[0]
    found = []
    for i in range(1, 255):
        host = f"{prefix}.{i}"
        try:
            with socket.create_connection((host, FTP_PORT), timeout=0.12):
                found.append(host)
        except OSError:
            pass
    return found


def main():
    if not PLUGIN.is_file():
        sys.exit(f"{PLUGIN} does not exist - run ./build.sh first")

    hosts = [sys.argv[1]] if len(sys.argv) > 1 else DEFAULT_HOSTS
    host = find_console(hosts)
    if host is None:
        print(f"nothing on {', '.join(hosts)}:{FTP_PORT} - scanning the subnet...")
        candidates = scan_subnet()
        if not candidates:
            sys.exit("no FTP server found. Is ftpiiu running on the console?")
        print("answering on 21:", ", ".join(candidates))
        host = candidates[0]

    print(f"connecting to {host}...")
    ftp = ftplib.FTP()
    ftp.connect(host, FTP_PORT, timeout=10.0)
    ftp.login("anonymous", "")
    print(" ", ftp.getwelcome().strip())

    target = None
    for sd in SD_ROOTS:
        path = sd + PLUGIN_DIR
        try:
            ftp.cwd(path)
            target = path
            break
        except ftplib.all_errors:
            continue

    if target is None:
        # Aroma is installed, so wiiu/environments exists; the plugins folder
        # may simply not have been created yet.
        for sd in SD_ROOTS:
            try:
                ftp.cwd(sd + "/wiiu/environments/aroma")
            except ftplib.all_errors:
                continue
            try:
                ftp.mkd("plugins")
            except ftplib.all_errors:
                pass
            try:
                ftp.cwd(sd + PLUGIN_DIR)
                target = sd + PLUGIN_DIR
                print(f"  created {target}")
                break
            except ftplib.all_errors:
                continue

    if target is None:
        ftp.quit()
        sys.exit("could not find wiiu/environments/aroma/plugins on the card")

    print(f"  plugins dir: {target}")

    # nlst() answers with full paths on ftpiiu, so compare basenames or an
    # existing file never matches.
    try:
        existing = {Path(n).name for n in ftp.nlst()}
    except ftplib.all_errors:
        existing = set()
    if PLUGIN.name in existing:
        print(f"  replacing the existing {PLUGIN.name}")

    size = PLUGIN.stat().st_size
    with PLUGIN.open("rb") as fh:
        ftp.storbinary(f"STOR {PLUGIN.name}", fh, blocksize=32768)

    # Read the size back: a truncated transfer over Wi-Fi is silent otherwise,
    # and a half-written plugin is a boot loop rather than an error message.
    try:
        ftp.voidcmd("TYPE I")
        written = ftp.size(PLUGIN.name)
    except ftplib.all_errors:
        written = None

    ftp.quit()

    if written is not None and written != size:
        sys.exit(f"FAILED: sent {size} bytes, console has {written}")
    print(f"sent {PLUGIN.name} ({size} bytes) -> {target}")
    print("\nReboot into Aroma to load it, then run: python pc/dashboard.py")


if __name__ == "__main__":
    main()
