"""the halfcraft server's network (docs/server/PLAN.md, S3): a hub and every half-life 2 map, each a pair of servers.

a pair is a source dedicated server (srcds_linux64, one map) and a minecraft server (the halfcraft mod) linked to it,
each with its own world and ports. the hub's pair runs all the time; a map's starts when a player first goes there
(a portal in the hub, /home) and stops when nobody has been on it for a while. the minecraft servers share one folder
of players' data (inventories, health): a player is on one of them at a time.

python tools/server/network.py layout          lays out every pair's minecraft folder and build/network/network.json
python tools/server/network.py start [name]    starts the hub's pair (or a map's), in the background
python tools/server/network.py ensure <name>   starts a map's pair unless it runs, and waits until both are up
python tools/server/network.py stop [name]     stops one pair, or all of them
python tools/server/network.py status          which pairs run
python tools/server/network.py idle <minutes>  stops the maps' pairs nobody has been on for that long (not the hub's)

HALFCRAFT_SRCDS (or --srcds) names the dedicated server's folder, as for srcds.py; HALFCRAFT_PUBLIC_HOST the address
players reach the servers on (this machine's lan address by default). logs: build/network/<name>/.
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/, where the halfcraft package is
import rcon
from halfcraft import REPO, ToolError

# half-life 2's chapters (no episodes), each a block of the hub: (title, maps)
CHAPTERS = [
    ("Пункт назначения", ["d1_trainstation_01", "d1_trainstation_02", "d1_trainstation_03", "d1_trainstation_04"]),
    ("Красный день календаря", ["d1_trainstation_05", "d1_trainstation_06"]),
    ("Тропа Канал", ["d1_canals_01", "d1_canals_01a", "d1_canals_02", "d1_canals_03", "d1_canals_05"]),
    ("Водная преграда", ["d1_canals_06", "d1_canals_07", "d1_canals_08", "d1_canals_09", "d1_canals_10", "d1_canals_11", "d1_canals_12", "d1_canals_13"]),
    ("Восточная Чёрная Меза", ["d1_eli_01", "d1_eli_02"]),
    ("Мы не ходим в Рейвенхолм", ["d1_town_01", "d1_town_01a", "d1_town_02", "d1_town_03", "d1_town_02a", "d1_town_04", "d1_town_05"]),
    ("Шоссе 17", ["d2_coast_01", "d2_coast_03", "d2_coast_04", "d2_coast_05", "d2_coast_07", "d2_coast_08"]),
    ("Песчаные ловушки", ["d2_coast_09", "d2_coast_10", "d2_coast_11", "d2_coast_12", "d2_prison_01"]),
    ("Нова Проспект", ["d2_prison_02", "d2_prison_03", "d2_prison_04", "d2_prison_05"]),
    ("Запутанность", ["d2_prison_06", "d2_prison_07", "d2_prison_08", "d3_c17_01"]),
    ("Антигражданин Один", ["d3_c17_02", "d3_c17_03", "d3_c17_04", "d3_c17_05", "d3_c17_06a", "d3_c17_06b", "d3_c17_07", "d3_c17_08"]),
    ("Следуйте за Фрименом!", ["d3_c17_09", "d3_c17_10a", "d3_c17_10b", "d3_c17_11", "d3_c17_12", "d3_c17_12b", "d3_c17_13"]),
    ("Наши покровители", ["d3_citadel_01", "d3_citadel_02", "d3_citadel_03", "d3_citadel_04", "d3_citadel_05"]),
    ("Тёмная энергия", ["d3_breen_01"]),
]
HUB = ("hub", "background02")

NETWORK = REPO / "build" / "network"
PLAYERS = NETWORK / "players"  # every minecraft server's world/players
SRCDS = Path(__file__).resolve().parent / "srcds.py"
MINECRAFT = REPO / "minecraft"
TEMPLATE = MINECRAFT / "run" / "server"  # make mc-server's folder: its server.properties, ops and eula go to every pair
JAVA = Path("/usr/lib/jvm/java-25-openjdk/bin/java")
WORLD = "world"
STARTUP_SECONDS = 180.0


def pairs() -> list[dict]:
    """every pair: the hub first, then the maps in the story's order, with their ports."""
    out = [{"name": HUB[0], "map": HUB[1], "chapter": "", "srcds": 27015, "minecraft": 25565, "rcon": 25575}]
    index = 1
    for chapter, maps in CHAPTERS:
        for map_name in maps:
            out.append({"name": map_name, "map": map_name, "chapter": chapter, "srcds": 27015 + index, "minecraft": 26000 + index, "rcon": 26500 + index})
            index += 1
    return out


def find(name: str) -> dict:
    for pair in pairs():
        if pair["name"] == name:
            return pair
    raise ToolError(f"no pair named {name}")


def public_host() -> str:
    named = os.environ.get("HALFCRAFT_PUBLIC_HOST")
    if named:
        return named
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect(("10.255.255.255", 1))  # no packet is sent: the route picks the lan address
        return probe.getsockname()[0]


def folder(pair: dict) -> Path:
    return NETWORK / pair["name"]


def layout() -> None:
    """every pair's minecraft folder (its server.properties from make mc-server's, its own ports), and network.json."""
    if not (TEMPLATE / "server.properties").exists():
        raise ToolError(f"no {TEMPLATE / 'server.properties'}: run make mc-server once first")
    template = (TEMPLATE / "server.properties").read_text()
    PLAYERS.mkdir(parents=True, exist_ok=True)
    for pair in pairs():
        here = folder(pair)
        (here / WORLD).mkdir(parents=True, exist_ok=True)
        players = here / WORLD / "players"
        if not players.is_symlink():
            if players.exists():
                shutil.rmtree(players)
            players.symlink_to(PLAYERS)
        properties = {}
        for line in template.splitlines():
            if "=" in line and not line.startswith("#"):
                key, value = line.split("=", 1)
                properties[key] = value
        properties.update({
            "server-port": str(pair["minecraft"]), "query.port": str(pair["minecraft"]), "rcon.port": str(pair["rcon"]),
            "level-name": WORLD, "accepts-transfers": "true", "motd": f"HalfCraft 2: {pair['map']}",
        })  # fmt: skip
        (here / "server.properties").write_text("".join(f"{k}={v}\n" for k, v in properties.items()))
        # minecraft refuses a world with symlinks it isn't told about: the shared players' folder is one
        (here / "allowed_symlinks.txt").write_text(f"[prefix]{PLAYERS}\n")
        for name in ("eula.txt", "ops.json", "whitelist.json", "banned-players.json", "banned-ips.json"):
            if (TEMPLATE / name).exists():
                shutil.copy2(TEMPLATE / name, here / name)
    (NETWORK / "network.json").write_text(json.dumps({"host": public_host(), "chapters": [c for c, _ in CHAPTERS], "pairs": pairs()}, ensure_ascii=False, indent=1))
    print(f"laid out {len(pairs())} pairs in {NETWORK}")


def processes() -> dict[str, dict[str, int]]:
    """name -> {"srcds": pid, "minecraft": pid} of what runs."""
    by_port = {pair["srcds"]: pair["name"] for pair in pairs()}
    by_link = {f"srv-{pair['name']}": pair["name"] for pair in pairs()}
    found: dict[str, dict[str, int]] = {}
    for proc in Path("/proc").iterdir():
        if not proc.name.isdigit():
            continue
        try:
            args = (proc / "cmdline").read_bytes().split(b"\0")
            environ = (proc / "environ").read_bytes().split(b"\0") if args and args[0].endswith(b"java") else []
        except OSError:
            continue
        if args and args[0].endswith(b"srcds_linux64"):
            for i, arg in enumerate(args[:-1]):
                if arg == b"-port" and int(args[i + 1]) in by_port:
                    found.setdefault(by_port[int(args[i + 1])], {})["srcds"] = int(proc.name)
        for variable in environ:
            if variable.startswith(b"HALFCRAFT_SERVER_LINKS=") and b"fabric.dli.env=server" in b" ".join(args):
                link = variable.split(b"=", 1)[1].decode()
                if link in by_link:
                    found.setdefault(by_link[link], {})["minecraft"] = int(proc.name)
    return found


BUILT = [MINECRAFT / "build" / "classes" / "java" / "main", MINECRAFT / "build" / "resources" / "main",
         MINECRAFT / "build" / "classes" / "java" / "client", MINECRAFT / "build" / "resources" / "client"]  # fmt: skip


def snapshot() -> tuple[Path, Path]:
    """the mod as built now, copied where a build can't change it under a running server: (its argfile, its launch.cfg).

    a pair's minecraft loads the mod's classes from minecraft/build lazily: rebuilt under it, one went missing and the
    server crashed (ClassNotFoundException). one copy per build, named by its newest file's time."""
    newest = max((f.stat().st_mtime_ns for folder in BUILT if folder.exists() for f in folder.rglob("*") if f.is_file()), default=0)
    here = NETWORK / "mod" / str(newest)
    argfile, config = here / "runServer", here / "launch.cfg"
    if argfile.exists() and config.exists():
        return argfile, config
    staging = here.with_name(here.name + ".tmp")
    shutil.rmtree(staging, ignore_errors=True)
    replace = {}
    for folder in BUILT:
        if folder.exists():
            copy = staging / folder.relative_to(MINECRAFT / "build")
            shutil.copytree(folder, copy)
            replace[str(folder)] = str(here / folder.relative_to(MINECRAFT / "build"))
    def moved(text: str) -> str:
        for old, new in replace.items():
            text = text.replace(old, new)
        return text
    (staging / "runServer").write_text(moved((MINECRAFT / "build" / "loom-cache" / "argFiles" / "runServer").read_text()))
    (staging / "launch.cfg").write_text(moved((MINECRAFT / ".gradle" / "loom-cache" / "launch.cfg").read_text()))
    shutil.rmtree(here, ignore_errors=True)
    staging.rename(here)
    return argfile, config


def start(pair: dict, srcds: str | None) -> None:
    here = folder(pair)
    running = processes().get(pair["name"], {})
    if "srcds" not in running:
        extra = ["--srcds", srcds] if srcds else []
        command = [sys.executable, str(SRCDS), "run", "--no-layout", *extra, "--map", pair["map"], "--port", str(pair["srcds"]), "--link", f"srv-{pair['name']}",
                   "--mc-port", str(pair["minecraft"]), "--log", str(here / "srcds.log")]  # fmt: skip
        with open(here / "srcds.out", "w") as out:
            subprocess.Popen(command, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    if "minecraft" not in running:
        if not (MINECRAFT / "build" / "loom-cache" / "argFiles" / "runServer").exists():
            raise ToolError("no runServer argfile: run make mc-server once first")
        argfile, config = snapshot()
        command = [str(JAVA), f"-Dfabric.dli.config={config}", "-Dfabric.dli.env=server",
                   "-Dfabric.dli.main=net.fabricmc.loader.impl.launch.knot.KnotServer", f"@{argfile}", "--enable-native-access=ALL-UNNAMED",
                   "--sun-misc-unsafe-memory-access=allow", "-Dfile.encoding=UTF-8", "-Xmx2G", "net.fabricmc.devlaunchinjector.Main", "nogui"]  # fmt: skip
        env = {**os.environ, "HALFCRAFT_SERVER_LINKS": f"srv-{pair['name']}", "HALFCRAFT_NETWORK": str(NETWORK / "network.json"), "HALFCRAFT_PAIR": pair["name"]}
        with open(here / "minecraft.log", "w") as out:
            subprocess.Popen(command, cwd=here, env=env, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    print(f"{pair['name']}: srcds {pair['srcds']}, minecraft {pair['minecraft']} (logs in {here})")


def ready(pair: dict) -> bool:
    here = folder(pair)
    try:
        srcds_up = "VAC secure" in (here / "srcds.log").read_text(errors="replace")
        minecraft_up = f"server link srv-{pair['name']} up" in (here / "minecraft.log").read_text(errors="replace")
    except OSError:
        return False
    return srcds_up and minecraft_up


def ensure(pair: dict, srcds: str | None) -> None:
    running = processes().get(pair["name"], {})
    if not ("srcds" in running and "minecraft" in running and ready(pair)):
        start(pair, srcds)
        deadline = time.time() + STARTUP_SECONDS
        while not ready(pair):
            if time.time() > deadline:
                raise ToolError(f"{pair['name']} didn't come up within {STARTUP_SECONDS:.0f} s: see {folder(pair)}")
            time.sleep(1.0)
    print(json.dumps({"name": pair["name"], "srcds": pair["srcds"], "minecraft": pair["minecraft"], "host": public_host()}))


def stop(names: list[str]) -> None:
    for name, pids in processes().items():
        if names and name not in names:
            continue
        for pid in pids.values():
            os.kill(pid, signal.SIGTERM)
        print(f"{name}: stopping")
    deadline = time.time() + 20.0
    while any(not names or name in names for name in processes()) and time.time() < deadline:
        time.sleep(0.5)
    for name, pids in processes().items():
        if not names or name in names:
            for pid in pids.values():
                os.kill(pid, signal.SIGKILL)


def players_on(pair: dict) -> int:
    """how many players the pair's source server has (its rcon's status), -1 if it can't tell."""
    try:
        status = rcon.run(["status"], host="127.0.0.1", port=pair["srcds"])
    except OSError:
        return -1
    for line in status.splitlines():
        if line.startswith("players"):
            return int(line.split(":", 1)[1].split()[0])
    return -1


def idle(minutes: float) -> None:
    """stops the maps' pairs that have had nobody on them for that long, as build/network/<name>/empty-since keeps."""
    now = time.time()
    for name in processes():
        if name == HUB[0]:
            continue
        pair = find(name)
        marker = folder(pair) / "empty-since"
        if players_on(pair) != 0:
            marker.unlink(missing_ok=True)
            continue
        if not marker.exists():
            marker.write_text(str(now))
        elif now - float(marker.read_text()) >= minutes * 60.0:
            print(f"{name}: nobody for {minutes:.0f} min")
            stop([name])
            marker.unlink(missing_ok=True)


CLIENT = NETWORK / "client"  # the client package the launcher fetches (serve: port 27080)
PACKAGE_PORT = 27080
FABRIC_API_URL = "https://cdn.modrinth.com/data/P7dR8mSH/versions/bNnaTiuM/fabric-api-0.161.0%2B26.3.jar"
FABRIC_API_SHA512 = "ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d"


def gradle_property(name: str) -> str:
    for line in (MINECRAFT / "gradle.properties").read_text().splitlines():
        if line.startswith(name + "="):
            return line.split("=", 1)[1].strip()
    raise ToolError(f"no {name} in minecraft/gradle.properties")


def file_entry(path: str, source: Path) -> dict:
    import hashlib  # noqa: PLC0415

    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    return {"path": path, "sha256": digest, "size": source.stat().st_size}


def package() -> None:
    """the client package the launcher (launcher/) installs: the linux game folder (build/game-hl2dm's own files,
    the libraries without their debug symbols), the mod's jar and fabric api (from modrinth), and manifest.json.
    windows has none yet: its dlls come from a windows build."""
    game = REPO / "build" / "game-hl2dm"
    listed = game / "halfcraft-files.txt"
    if not listed.exists():
        raise ToolError(f"no {listed}: make build first")
    shutil.rmtree(CLIENT, ignore_errors=True)
    linux = []
    names = [line.strip().lstrip("\ufeff") for line in listed.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    for name in [*names, "gameinfo.txt", "halfcraft-files.txt"]:
        source = game / name
        if not source.is_file():
            continue
        target = CLIENT / "game-hl2dm" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if name.endswith(".so"):
            subprocess.run(["objcopy", "--strip-debug", str(source), str(target)], check=True)
        else:
            shutil.copy2(source, target)
        linux.append(file_entry(f"game-hl2dm/{name}", target))
    jars = sorted((p for p in (MINECRAFT / "build" / "libs").glob("halfcraft-*.jar") if not p.name.endswith("-sources.jar")), key=lambda p: p.stat().st_mtime)
    if not jars:
        raise ToolError("no mod jar in minecraft/build/libs: run gradle build first")
    mod = CLIENT / "minecraft" / "instance" / "mods" / "halfcraft.jar"
    mod.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(jars[-1], mod)
    any_platform = [file_entry("minecraft/instance/mods/halfcraft.jar", mod),
                    {"path": "minecraft/instance/mods/fabric-api.jar", "url": FABRIC_API_URL, "sha512": FABRIC_API_SHA512}]  # fmt: skip
    manifest = {
        "version": jars[-1].stem.removeprefix("halfcraft-"),
        "minecraft": gradle_property("minecraft_version"),
        "fabric_loader": gradle_property("loader_version"),
        "srcds_port": find(HUB[0])["srcds"],
        "files": {"any": any_platform, "linux": linux, "windows": []},
    }
    (CLIENT / "manifest.json").write_text(json.dumps(manifest, indent=1))
    size = sum(f.get("size", 0) for f in linux) // (1 << 20)
    print(f"client package {manifest['version']}: {len(linux)} linux files ({size} MB) in {CLIENT}")


def serve() -> None:
    """the client package over http, on port 27080, until stopped."""
    if not (CLIENT / "manifest.json").exists():
        package()
    subprocess.run([sys.executable, "-m", "http.server", str(PACKAGE_PORT), "--directory", str(CLIENT)], check=False)


def main() -> None:
    parser = argparse.ArgumentParser(description="the halfcraft server's network: a hub and every half-life 2 map")
    parser.add_argument("what", choices=["layout", "start", "ensure", "stop", "status", "idle", "package", "serve"])
    parser.add_argument("name", nargs="?")
    parser.add_argument("--srcds", help="the dedicated server's folder (HALFCRAFT_SRCDS)")
    options = parser.parse_args()
    if options.what == "layout":
        layout()
    elif options.what == "start":
        layout()
        if not processes():
            # the game folder, once, before any of the network's source servers maps it
            subprocess.run([sys.executable, str(SRCDS), "layout", *(["--srcds", options.srcds] if options.srcds else [])], check=True)
        start(find(options.name or HUB[0]), options.srcds)
    elif options.what == "ensure":
        ensure(find(options.name), options.srcds)
    elif options.what == "stop":
        stop([options.name] if options.name else [])
    elif options.what == "package":
        package()
    elif options.what == "serve":
        serve()
    elif options.what == "idle":
        idle(float(options.name or 10))
    else:
        up = processes()
        for pair in pairs():
            if pair["name"] in up:
                print(f"{pair['name']:20} {pair['map']:20} srcds {pair['srcds']} minecraft {pair['minecraft']}: {', '.join(sorted(up[pair['name']]))}")
        print(f"{len(up)} of {len(pairs())} pairs run")


if __name__ == "__main__":
    main()
