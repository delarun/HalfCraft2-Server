"""puts the halfcraft server's network on a linux machine (docs/server/PLAN.md, S4) and starts it as systemd services.

python tools/server/deploy.py root@<server>          everything: files, services, start
python tools/server/deploy.py root@<server> --code   only halfcraft's own files (a new build), then restart

what goes there, under /srv/halfcraft (owned by the halfcraft user it makes):
  hl2dm-ds/   the dedicated server (this machine's HALFCRAFT_SRCDS folder: steam's app 232370 with the 64-bit srcds)
  hl2/        half-life 2's content the maps need (hl2, episodic, ep2, hl2_complete, lostcoast), from steam here:
              steam gives the dedicated server no map of half-life 2 itself
  repo/       tools/, the server's game folder (build/server/game-hl2dm), minecraft's server (fabric's server
              launcher, the mod, fabric api) and the client package the launcher fetches (build/network/client)
the services: halfcraft-network (the hub's pair; maps' pairs start as players go there) and halfcraft-package (the
client package on port 27080). minecraft runs in online mode: players sign in with their microsoft accounts.

it needs ssh to the machine as root, rsync on both, and java 25 there (apt's openjdk-25-jre-headless).
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/, where the halfcraft package is
import network

from halfcraft import REPO, ToolError

TARGET = "/srv/halfcraft"
CACHE = REPO / ".cache" / "deploy"
STAGE = REPO / "build" / "deploy"
HL2_PARTS = ["hl2", "episodic", "ep2", "hl2_complete", "lostcoast"]
FABRIC_INSTALLER = "1.1.0"
MEMORY = os.environ.get("HALFCRAFT_MC_MEMORY", "1G")  # each pair's minecraft's heap (network.py's jvm flags keep it small)


def run(command: list[str], **kwargs: object) -> None:
    print("$ " + " ".join(command))
    subprocess.run(command, check=True, **kwargs)


def remote(host: str, script: str) -> None:
    run(["ssh", "-o", "BatchMode=yes", host, "bash -se"], input=script.encode())


def rsync(source: str, host: str, destination: str, *extra: str) -> None:
    run(["rsync", "-a", "--delete", "--info=stats1", *extra, source, f"{host}:{destination}"])


def download(url: str, path: Path, sha512: str | None = None) -> Path:
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        print(f"downloading {url}")
        urllib.request.urlretrieve(url, path)  # noqa: S310 fixed https urls
    if sha512 and hashlib.sha512(path.read_bytes()).hexdigest() != sha512:
        path.unlink()
        raise ToolError(f"{url}: not the file its hash names")
    return path


def stage(windows_zip: Path | None = None) -> Path:
    """halfcraft's own files as they go to repo/: tools, the game folder's place, minecraft's server, the package."""
    shutil.rmtree(STAGE, ignore_errors=True)
    repo = STAGE / "repo"
    shutil.copytree(REPO / "tools", repo / "tools", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    # minecraft's server: fabric's launcher for this minecraft and loader, the mod, fabric api
    minecraft_version, loader = network.gradle_property("minecraft_version"), network.gradle_property("loader_version")
    launcher = download(f"https://meta.fabricmc.net/v2/versions/loader/{minecraft_version}/{loader}/{FABRIC_INSTALLER}/server/jar",
                        CACHE / f"fabric-server-{minecraft_version}-{loader}-{FABRIC_INSTALLER}.jar")  # fmt: skip
    server = repo / "minecraft" / "server"
    (server / "mods").mkdir(parents=True)
    shutil.copy2(launcher, server / "fabric-server-launcher.jar")
    jars = sorted(
        (p for p in (network.MINECRAFT / "build" / "libs").glob("halfcraft-*.jar") if not p.name.endswith("-sources.jar")), key=lambda p: p.stat().st_mtime
    )
    if not jars:
        raise ToolError("no mod jar in minecraft/build/libs: run gradle jar first")
    shutil.copy2(jars[-1], server / "mods" / "halfcraft.jar")
    shutil.copy2(download(network.FABRIC_API_URL, CACHE / "fabric-api.jar", network.FABRIC_API_SHA512), server / "mods" / "fabric-api.jar")
    shutil.copy2(network.MINECRAFT / "gradle.properties", repo / "minecraft" / "gradle.properties")
    # the pairs' server.properties come from this one: online mode, players sign in with microsoft
    template = repo / "minecraft" / "run" / "server"
    template.mkdir(parents=True)
    lines = ["online-mode=true" if line.startswith("online-mode=") else line for line in (network.TEMPLATE / "server.properties").read_text().splitlines()]
    (template / "server.properties").write_text("\n".join(lines) + "\n")
    (template / "eula.txt").write_text("eula=true\n")  # the user accepted minecraft's eula for the dev server
    (template / "ops.json").write_text("[]\n")
    # the client package the launcher fetches
    network.package(windows_zip)
    shutil.copytree(network.CLIENT, repo / "build" / "network" / "client")
    # the source servers' rcon password (srcds.py, rcon.py), the one the minecraft servers' properties have
    (repo / "build" / "server").mkdir(parents=True)
    shutil.copy2(REPO / "build" / "server" / "rcon.txt", repo / "build" / "server" / "rcon.txt")
    return repo


SERVICES = """\
[Unit]
Description=HalfCraft 2 network: the hub's pair (maps' pairs start as players go there)
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
User=halfcraft
Environment=HALFCRAFT_SRCDS={target}/hl2dm-ds HALFCRAFT_MC_MEMORY={memory} HOME={target}
WorkingDirectory={target}/repo
ExecStart=/usr/bin/python3 {target}/repo/tools/server/network.py start hub
ExecStop=/usr/bin/python3 {target}/repo/tools/server/network.py stop
KillMode=process
TimeoutStopSec=60
# the kernel killing a pair for memory takes that pair alone: the others play on, its players go back to the hub,
# and halfcraft-watchdog brings the hub back if it was the one
OOMPolicy=continue

[Install]
WantedBy=multi-user.target
"""

WATCHDOG = """\
[Unit]
Description=HalfCraft 2 network watchdog: the hub's pair back if it went down

[Service]
Type=oneshot
User=halfcraft
Environment=HALFCRAFT_SRCDS={target}/hl2dm-ds HALFCRAFT_MC_MEMORY={memory} HOME={target}
WorkingDirectory={target}/repo
ExecStart=/usr/bin/python3 {target}/repo/tools/server/network.py start hub
"""

WATCHDOG_TIMER = """\
[Unit]
Description=HalfCraft 2 network watchdog, every minute

[Timer]
OnBootSec=2min
OnUnitActiveSec=1min

[Install]
WantedBy=timers.target
"""

PACKAGE_SERVICE = """\
[Unit]
Description=HalfCraft 2 client package for the launcher (port 27080)
After=network-online.target

[Service]
User=halfcraft
WorkingDirectory={target}/repo
ExecStart=/usr/bin/python3 {target}/repo/tools/server/network.py serve
Restart=on-failure

[Install]
WantedBy=multi-user.target
"""


def deploy(host: str, *, code_only: bool, windows_zip: Path | None = None) -> None:
    repo = stage(windows_zip)
    srcds = os.environ.get("HALFCRAFT_SRCDS")
    if not srcds:
        raise ToolError("name this machine's dedicated server folder: HALFCRAFT_SRCDS")
    remote(
        host,
        f"""
set -e
id halfcraft >/dev/null 2>&1 || useradd --system --home-dir {TARGET} --shell /usr/sbin/nologin halfcraft
mkdir -p {TARGET}/hl2dm-ds {TARGET}/hl2 {TARGET}/repo/build/server
systemctl stop halfcraft-network 2>/dev/null || true
""",
    )
    if not code_only:
        rsync(f"{srcds}/", host, f"{TARGET}/hl2dm-ds/", "--exclude=game-hl2dm", "--exclude=hl2c", "--exclude=*.log")
        hl2 = next((Path(p) for p in ["/mnt/data/SteamLibrary/steamapps/common/Half-Life 2", str(Path.home() / ".local/share/Steam/steamapps/common/Half-Life 2")]
                    if (Path(p) / "hl2").is_dir()), None)  # fmt: skip
        if hl2 is None:
            raise ToolError("half-life 2 isn't installed here: its content goes to the server from steam's install")
        for part in HL2_PARTS:
            rsync(f"{hl2 / part}/", host, f"{TARGET}/hl2/{part}/")
    # steam's client library, which srcds signs on to steam with (as the anonymous game server it is on a lan)
    steamclient = Path.home() / ".steam" / "sdk64" / "steamclient.so"
    if steamclient.exists():
        remote(host, f"mkdir -p {TARGET}/.steam/sdk64")
        run(["rsync", "-aL", str(steamclient), f"{host}:{TARGET}/.steam/sdk64/steamclient.so"])
    # halfcraft's own: the server's game folder (its libraries' links are made there again), and the staged repo
    rsync(f"{REPO / 'build' / 'server' / 'game-hl2dm'}/", host, f"{TARGET}/repo/build/server/game-hl2dm/", "--exclude=srvlib", "--exclude=*.log",
          "--exclude=screenshots", "--exclude=save")  # fmt: skip
    rsync(f"{repo}/", host, f"{TARGET}/repo/", "--exclude=build/server/game-hl2dm", "--exclude=build/network/hub", "--exclude=build/network/d*",
          "--exclude=build/network/players", "--exclude=build/network/*.json")  # fmt: skip
    remote(
        host,
        f"""
set -e
cd {TARGET}
ln -sfn {TARGET}/repo/build/server/game-hl2dm hl2dm-ds/game-hl2dm
ln -sfn {TARGET}/hl2 hl2dm-ds/hl2c
mkdir -p repo/build/server/game-hl2dm/srvlib
for lib in hl2dm-ds/bin/linux64/*_srv.so; do
    name=$(basename "$lib" _srv.so).so
    ln -sfn {TARGET}/$lib repo/build/server/game-hl2dm/srvlib/$name
done
cat > /etc/systemd/system/halfcraft-network.service <<'UNIT'
{SERVICES.format(target=TARGET, memory=MEMORY)}UNIT
cat > /etc/systemd/system/halfcraft-package.service <<'UNIT'
{PACKAGE_SERVICE.format(target=TARGET)}UNIT
cat > /etc/systemd/system/halfcraft-watchdog.service <<'UNIT'
{WATCHDOG.format(target=TARGET, memory=MEMORY)}UNIT
cat > /etc/systemd/system/halfcraft-watchdog.timer <<'UNIT'
{WATCHDOG_TIMER}UNIT
chown -R halfcraft:halfcraft {TARGET}
systemctl daemon-reload
systemctl enable --now halfcraft-package >/dev/null
systemctl enable halfcraft-network >/dev/null
systemctl restart halfcraft-package
sudo -u halfcraft env HALFCRAFT_SRCDS={TARGET}/hl2dm-ds python3 repo/tools/server/network.py layout
systemctl restart halfcraft-network
systemctl enable --now halfcraft-watchdog.timer >/dev/null
systemctl --no-pager status halfcraft-network halfcraft-package | grep -E "Active|Main PID" || true
""",
    )
    print(f"deployed to {host}:{TARGET}")


def main() -> None:
    parser = argparse.ArgumentParser(description="the halfcraft server's network on a linux machine, as systemd services")
    parser.add_argument("host", help="user@machine (root: it makes the halfcraft user and the services)")
    parser.add_argument("--code", action="store_true", help="only halfcraft's own files (a new build), then restart")
    parser.add_argument("--windows-zip", type=Path, help="the windows game folder for the client package (ci's HalfCraft-client-windows zip)")
    options = parser.parse_args()
    deploy(options.host, code_only=options.code, windows_zip=options.windows_zip)


if __name__ == "__main__":
    main()
