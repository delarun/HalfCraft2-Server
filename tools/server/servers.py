"""the halfcraft server's source servers (docs/server/PLAN.md, S3): the hub and every highway 17 map, one srcds each.

python tools/server/servers.py start    starts the ones not running, in the background (logs: build/server/srcds-<port>.log)
python tools/server/servers.py stop     stops them all
python tools/server/servers.py status   which run
python tools/server/servers.py links    their link ids, comma separated (HALFCRAFT_SERVER_LINKS for make mc-server-all)

HALFCRAFT_SRCDS (or --srcds) names the dedicated server's folder, as for srcds.py.
"""

import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/, where the halfcraft package is
from halfcraft import REPO

# (name, map, port): the hub first. the link id is srv-<name>
SERVERS = [
    ("hub", "background02", 27015),
    ("d2_coast_01", "d2_coast_01", 27016),
    ("d2_coast_03", "d2_coast_03", 27017),
    ("d2_coast_04", "d2_coast_04", 27018),
    ("d2_coast_05", "d2_coast_05", 27019),
    ("d2_coast_07", "d2_coast_07", 27020),
    ("d2_coast_08", "d2_coast_08", 27021),
]
SRCDS = Path(__file__).resolve().parent / "srcds.py"
LOGS = REPO / "build" / "server"


def link_id(name: str) -> str:
    return f"srv-{name}"


def running() -> dict[int, int]:
    """port -> pid of the srcds_linux64 running on it."""
    found: dict[int, int] = {}
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            args = (proc / "cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        if not args or not args[0].endswith(b"srcds_linux64"):
            continue
        for i, arg in enumerate(args[:-1]):
            if arg == b"-port":
                found[int(args[i + 1])] = int(proc.name)
    return found


def start(srcds: str | None) -> None:
    up = running()
    extra = ["--srcds", srcds] if srcds else []
    for name, map_name, port in SERVERS:
        if port in up:
            print(f"{name}: running on {port} (pid {up[port]})")
            continue
        log = LOGS / f"srcds-{port}.log"
        command = [sys.executable, str(SRCDS), "run", *extra, "--map", map_name, "--port", str(port), "--link", link_id(name), "--log", str(log)]
        with open(LOGS / f"servers-{port}.out", "w") as out:
            subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        print(f"{name}: {map_name} on {port}, link {link_id(name)}, log {log}")
        time.sleep(1.0)  # the layout each run checks: one at a time


def stop() -> None:
    up = running()
    for pid in up.values():
        os.kill(pid, signal.SIGTERM)
    deadline = time.time() + 10.0
    while running() and time.time() < deadline:
        time.sleep(0.5)
    for pid in running().values():
        os.kill(pid, signal.SIGKILL)
    print(f"stopped {len(up)} server(s)")


def main() -> None:
    parser = argparse.ArgumentParser(description="the halfcraft server's source servers: the hub and highway 17")
    parser.add_argument("what", choices=["start", "stop", "status", "links"])
    parser.add_argument("--srcds", help="the dedicated server's folder (HALFCRAFT_SRCDS)")
    options = parser.parse_args()
    if options.what == "links":
        print(",".join(link_id(name) for name, _, _ in SERVERS))
    elif options.what == "start":
        LOGS.mkdir(parents=True, exist_ok=True)
        start(options.srcds)
    elif options.what == "stop":
        stop()
    else:
        up = running()
        for name, map_name, port in SERVERS:
            print(f"{name:12} {map_name:14} {port}  {'running, pid ' + str(up[port]) if port in up else 'stopped'}")


if __name__ == "__main__":
    main()
