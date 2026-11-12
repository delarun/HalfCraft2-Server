"""halfcraft's source dedicated server on linux: half-life 2: deathmatch's srcds_linux64 with halfcraft's server.so,
running one map (docs/server/PLAN.md, milestone S0).

python tools/server/srcds.py layout                lays out build/server/game-hl2dm from build/game-hl2dm (make build)
python tools/server/srcds.py run --map d2_coast_01  lays it out, then runs the server in this terminal (its console)
python tools/server/srcds.py run --map d2_coast_01 --port 27016 --link srv2 --log build/server/srcds-27016.log

the dedicated server (steam app 232370) comes from steamcmd, anonymously; HALFCRAFT_SRCDS (or --srcds) names its
folder. give steamcmd a home of its own: run through ~/.steam it rewrites the steam client's library list.

    HOME=~/.local/share/steamcmd-home steamcmd +force_install_dir <folder> +login anonymous +app_update 232370 +quit

what a dedicated server wants that the client's game folder doesn't have, all made here:
- the game folder inside the server's own folder: the dedicated file system reads no file (cfg/valve.rc, so no
  +map) and mounts no vpk outside it. <srcds>/game-hl2dm links to build/server/game-hl2dm, <srcds>/hl2c to half-life
  2's install (its campaign maps and content; HALFCRAFT_HL2 or --hl2 names it, else steam's).
- gameinfo.txt with half-life 2's content (|appid_220|) under hl2c and the server's own (|appid_320|) under its
  folder, both as |all_source_engine_paths| (the server has no steam client to ask for app folders), and vpk files by
  their name without _dir, as the server's own gameinfo names them.
- the game dll as server_srv.so, the name the dedicated engine loads, and its libraries under the names valve's sdk
  links with (libtier0.so, soundemittersystem.so, ...): links in build/server/game-hl2dm/srvlib to the server's
  *_srv.so, on the library path. glibc takes a link to an object already loaded for that object, so each library is
  loaded once.
- srcds_linux64 and the 64-bit libsteam_api.so, which app 232370 doesn't ship: copied from the client's install.

clients connect by the machine's lan address (connect <ip>:<port>): this engine ignores 127.0.0.1. the server listens
on every address (+ip 0.0.0.0). -hc_link: the server's own link (srv by default), never the default link a client on
the same machine has.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/, where the halfcraft package is
from halfcraft import REPO, ToolError, engines, game_folder

ENGINE = engines.ENGINES["hl2dm"]
GAME = REPO / "build/server/game-hl2dm"
GAME_LINK = "game-hl2dm"  # in the server's folder; the clients' game folder has this name too
CONTENT_LINK = "hl2c"  # in the server's folder: half-life 2's install


def server_folder(named: str | None) -> Path:
    folder = Path(named or os.environ.get("HALFCRAFT_SRCDS") or "")
    if not named and not os.environ.get("HALFCRAFT_SRCDS"):
        raise ToolError("name the dedicated server's folder: --srcds <folder> or HALFCRAFT_SRCDS (steam app 232370, from steamcmd)")
    if not (folder / "bin/linux64/dedicated_srv.so").is_file():
        raise ToolError(f"{folder} isn't half-life 2: deathmatch's dedicated server (no bin/linux64/dedicated_srv.so)")
    return folder


def hl2_folder(named: str | None) -> Path:
    if named or (named := os.environ.get("HALFCRAFT_HL2")):
        folder = Path(named)
    elif found := engines.in_libraries(Path("Half-Life 2/hl2.sh")):
        folder = found.parent
    else:
        raise ToolError("half-life 2's install not found: --hl2 <folder> or HALFCRAFT_HL2 (its maps are the server's)")
    if not (folder / "hl2/maps").is_dir():
        raise ToolError(f"{folder} has no hl2/maps: it isn't half-life 2's install")
    return folder


def server_gameinfo(client: str) -> str:
    """the client's gameinfo.txt as the dedicated server reads it (the module's docstring says why)."""
    text = client.replace("|appid_220|", f"|all_source_engine_paths|{CONTENT_LINK}/").replace("|appid_320|", "|all_source_engine_paths|")
    return re.sub(r"_dir\.vpk\b", ".vpk", text)


def link(path: Path, target: Path) -> None:
    """path becomes a link to target (replacing a link, never a file or folder)."""
    if path.is_symlink():
        path.unlink()
    elif path.exists():
        raise ToolError(f"{path} is there and isn't a link: move it away")
    path.symlink_to(target)


def layout(srcds: Path, hl2: Path) -> None:
    client_install = ENGINE.find_exe().parent
    for name, source in (("srcds_linux64", client_install / "srcds_linux64"), ("bin/linux64/libsteam_api.so", client_install / "bin/linux64/libsteam_api.so")):
        if not (srcds / name).is_file():
            shutil.copy2(source, srcds / name)
            print(f"{srcds / name}: from {source}")
    if not (ENGINE.game / "gameinfo.txt").is_file():
        raise ToolError(f"no {ENGINE.game} - run make build first")
    GAME.parent.mkdir(parents=True, exist_ok=True)
    game_folder.copy_game_folder(ENGINE, GAME)
    (GAME / "gameinfo.txt").write_text(server_gameinfo((GAME / "gameinfo.txt").read_text(encoding="utf-8")), encoding="utf-8")
    shutil.copy2(GAME / ENGINE.game_bin / "server.so", GAME / ENGINE.game_bin / "server_srv.so")
    libraries = GAME / "srvlib"
    libraries.mkdir(exist_ok=True)
    for library in (srcds / "bin/linux64").glob("*_srv.so"):
        link(libraries / library.name.replace("_srv.so", ".so"), library)
    link(srcds / GAME_LINK, GAME)
    link(srcds / CONTENT_LINK, hl2)
    print(f"server game folder: {GAME} (as {srcds / GAME_LINK}); half-life 2's content: {hl2} (as {srcds / CONTENT_LINK})")


def run(srcds: Path, *, map: str, port: int, players: int, link_id: str, log: Path | None, extra: list[str]) -> int:  # noqa: A002 the map it runs
    command = [
        "./srcds_linux64", "-game", GAME_LINK, "-console", "-nobreakpad", "-hc_link", link_id,
        "+ip", "0.0.0.0",  # noqa: S104 a game server: players connect from the lan
        "-port", str(port), "+sv_lan", "1", "+maxplayers", str(players), *extra, "+map", map,
    ]  # fmt: skip
    env = {**os.environ, "LD_LIBRARY_PATH": f"{srcds / 'bin/linux64'}:{GAME / 'srvlib'}"}
    if log is None:
        return subprocess.run(command, cwd=srcds, env=env, check=False).returncode
    # srcds holds its output back when it isn't on a terminal: script gives it one, and the log gets every line
    log = log.resolve()  # script runs in the server's folder
    log.parent.mkdir(parents=True, exist_ok=True)
    shown = " ".join(command)
    return subprocess.run(["script", "-qfc", shown, str(log)], cwd=srcds, env=env, check=False).returncode


def main() -> None:
    parser = argparse.ArgumentParser(description="halfcraft's source dedicated server (linux)")
    parser.add_argument("what", choices=["layout", "run"])
    parser.add_argument("--srcds", help="the dedicated server's folder (HALFCRAFT_SRCDS)")
    parser.add_argument("--hl2", help="half-life 2's install, for its maps and content (HALFCRAFT_HL2; steam's by default)")
    parser.add_argument("--map", default="d2_coast_01")
    parser.add_argument("--port", type=int, default=27015)
    parser.add_argument("--players", type=int, default=8)
    parser.add_argument("--link", default="srv", help="the server's own link id")
    parser.add_argument("--log", type=Path, help="also write the console to this file")
    parser.add_argument("extra", nargs="*", help="more server arguments, after --")
    options = parser.parse_args()
    if not engines.LINUX:
        raise ToolError("the dedicated server tooling is linux only")
    srcds = server_folder(options.srcds)
    layout(srcds, hl2_folder(options.hl2))
    if options.what == "run":
        raise SystemExit(run(srcds, map=options.map, port=options.port, players=options.players, link_id=options.link, log=options.log, extra=options.extra))


if __name__ == "__main__":
    main()
