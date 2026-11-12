"""starts halfcraft's half-life 2 from its dev game folder (build\\game-<engine>, make build) on one of the two
engines (tools/halfcraft/engines.py); that engine's steam app must be installed. minecraft: make mc-run (the dev
client); it links up by itself once the game runs.

python tools/game/run_hl2.py                          hl2:dm's 64-bit engine, windowed 1280x720, main menu
python tools/game/run_hl2.py --engine hl2             half-life 2's own 32-bit engine
python tools/game/run_hl2.py --map d1_trainstation_02 straight into a map
python tools/game/run_hl2.py --fullscreen
python tools/game/run_hl2.py --width 2560 --height 1080
python tools/game/run_hl2.py --link guest             a second copy beside the first, on a link of its own (co-op
                                                      tests: make mc-test LINK=guest is its minecraft)
python tools/game/run_hl2.py --wait                   returns only when the game exits (tools/game/hidden_desktop.py
                                                      keeps its desktop while it waits; a game alone there dies)
python tools/game/run_hl2.py -- +hc_debug_blocks 1    more engine arguments after --

every start has -multirun, so no copy holds source's one-game mutex (a second copy, or another project's hl2.exe,
needs it free). the game's pid goes into its game folder: hl2.pid, or hl2-<tag>.pid on a link (engines.pid_file),
where make cmd finds it. the default link's copy logs to console.log (-condebug); a link's to console-<tag>.log
(+con_logfile, which starts after the engine's init: its first lines are missing), so two copies don't mix one file.
it refuses only while a copy runs on the same link: the default link's is the one in hl2.pid, or any game make run
didn't start (HalfCraft.exe's, a player's).
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools\, where the halfcraft package is
from halfcraft import REPO, ToolError, engines
from halfcraft.link import file_tag


def console_log(engine: engines.Engine, link: str | None) -> Path:
    """where the copy on this link logs its console."""
    return engine.game / ("console.log" if link is None else f"console-{file_tag(link)}.log")


def launch_arguments(
    engine: engines.Engine,
    *,
    link: str | None,
    fullscreen: bool,
    width: int,
    height: int,
    map: str | None,  # noqa: A002 the --map it starts in, as the option names it
    extra: list[str],
) -> list[str]:
    """the engine's command line, after its exe. link: the link id (None: the default link; "" or a bad id is a ToolError)."""
    # -insecure: no vac (halfcraft's dlls are no game's own; release's HalfCraft.exe starts it so too)
    arguments = ["-game", str(engine.game), "-multirun", "-novid", "-insecure", "+con_enable", "1", "+developer", "1"]
    if link is None:
        arguments.append("-condebug")
    else:
        arguments += ["-hc_link", link, "+con_logfile", console_log(engine, link).name]
    if not fullscreen:
        arguments += ["-windowed", "-w", str(width), "-h", str(height)]
    arguments += extra
    if map:
        arguments += ["+map", map]
    return arguments


def main() -> None:
    parser = argparse.ArgumentParser(description="start halfcraft's half-life 2 from its dev game folder")
    parser.add_argument("--engine", choices=list(engines.ENGINES), default="hl2dm")
    parser.add_argument("--map", help="straight into this map")
    parser.add_argument("--fullscreen", action="store_true")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--link", help="a link id: a copy beside the default link's, with a minecraft of its own")
    parser.add_argument("--wait", action="store_true", help="return only when the game exits")
    parser.add_argument("extra", nargs="*", help="more engine arguments, after --")
    options = parser.parse_args()

    engine = engines.ENGINES[options.engine]
    link = options.link
    arguments = launch_arguments(
        engine, link=link, fullscreen=options.fullscreen, width=options.width, height=options.height, map=options.map, extra=options.extra
    )
    if not (engine.game / engine.game_bin / engine.dlls()[0].name).is_file():
        raise ToolError(f"no build in {engine.game} - run make build ENGINE={engine.name} first")
    where = "the default link" if link is None else f"link {link}"
    if found := engines.game_on_link(link):
        raise ToolError(f"{found[0].name} already runs on {where} (pid {found[1]}): quit it first, or start this copy on a link of its own (--link)")
    if link is None and (others := engines.unrecorded_games()):
        game = others[0][1]
        raise ToolError(f"{game.name} (pid {game.pid}) runs, and make run didn't start it: it's on the default link. quit it, or use --link")
    exe = engine.find_exe()
    if engines.LINUX:
        run_linux(engine, exe, arguments, link, wait=options.wait)
        return

    # on its own: no handles of this console, and no ctrl+c from it
    flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    game = subprocess.Popen(
        [exe, *arguments], cwd=exe.parent, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=flags
    )
    engines.pid_file(engine, link).write_text(f"{game.pid}\n", encoding="ascii")
    print(f"started {exe} on {where}, pid {game.pid}; console log: {console_log(engine, link)}", flush=True)
    if options.wait:
        print(f"the game (pid {game.pid}) exited ({game.wait()})", flush=True)


def run_linux(engine: engines.Engine, exe: Path, arguments: list[str], link: str | None, *, wait: bool) -> None:
    """linux: the engine runs in the steam runtime's sniper container, through its hl2mp.sh. what starts is that
    container's launcher; the game's own pid (the one in the pid file) is the hl2mp_linux64 that appears after it."""
    script = exe.with_name("hl2mp.sh")
    # the container shares the home folder and the game's own; the repo and the libraries may be elsewhere
    shared = sorted({str(REPO), str(exe.resolve().parents[3])})  # the repo; <library>/steamapps/common/<game>/<exe>
    env = {**os.environ, "SteamAppId": str(engine.app), "SteamGameId": str(engine.app), "PRESSURE_VESSEL_FILESYSTEMS_RW": ":".join(shared)}
    before = {process.pid for process in engines.system.processes(exe.name)}
    since = engines.system.now()
    launcher = subprocess.Popen(
        [engines.sniper_run(), "--", script, *arguments],
        cwd=exe.parent,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    game = None
    for _ in range(120):
        fresh = [p for p in engines.system.processes(exe.name) if p.pid not in before and (p.started or since) >= since - 10_000_000]
        if fresh:
            game = fresh[0].pid
            break
        if launcher.poll() is not None:
            raise ToolError(f"the sniper container exited ({launcher.returncode}) before {exe.name} started")
        time.sleep(0.5)
    if game is None:
        raise ToolError(f"{exe.name} didn't start within a minute")
    engines.pid_file(engine, link).write_text(f"{game}\n", encoding="ascii")
    where = "the default link" if link is None else f"link {link}"
    print(f"started {exe} on {where}, pid {game}; console log: {console_log(engine, link)}", flush=True)
    if wait:
        print(f"the game (pid {game}) exited ({launcher.wait()})", flush=True)


if __name__ == "__main__":
    main()
