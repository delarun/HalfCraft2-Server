"""the two engines halfcraft runs on, what they need, and which of their games run.

hl2    half-life 2's own engine: 32-bit hl2.exe (app 220), dlls from source-sdk-2013-sp (windows only)
hl2dm  half-life 2: deathmatch's: 64-bit hl2mp_win64.exe (app 320), dlls from source-sdk-2013; on linux
       hl2mp_linux64 with .so files from the same tree (tools/halfcraft/linux.py builds them)

make run (tools/game/run_hl2.py) writes the pid of each game it starts into the game folder: hl2.pid on the default
link, hl2-<tag>.pid on a link of its own (link.file_tag), so a tool reaches one of two copies (a co-op test's host and
guest) by its link.
"""

import os
import re
from dataclasses import dataclass
from pathlib import Path

from halfcraft import REPO, ToolError
from halfcraft.link import file_tag

LINUX = os.name != "nt"
if LINUX:
    from halfcraft import posix as system
else:
    from halfcraft import windows as system


@dataclass(frozen=True)
class Engine:
    name: str
    bin: str  # where its sdk tree publishes the episodic dlls (they run half-life 2 and both episodes, as valve's hl2_complete does)
    game_bin: str  # the game folder's subfolder its engine loads them from
    app: int  # the steam app whose folder has the engine
    steam_folder: str
    exe: str
    # the shader dll its sdk tree builds next to client.dll (the engine loads it from game_bin too): its world shaders
    # leave the cells minecraft dug undrawn. None: none (minecraft digs nothing out of this engine's walls)
    shader_dll: str | None = None

    @property
    def game(self) -> Path:
        """its dev game folder (make build lays it out, make run plays from it)."""
        return REPO / f"build/game-{self.name}"

    def dlls(self) -> list[Path]:
        """client.dll, server.dll and the shader dll as its sdk tree's build publishes them (.so on linux)."""
        names = ("client", "server", *((self.shader_dll,) if self.shader_dll else ()))
        return [REPO / self.bin / f"{dll}{'.so' if LINUX else '.dll'}" for dll in names]

    def symbols(self, dll: Path) -> list[Path]:
        """the files that hold a dll's debug symbols, beside it: its .pdb on windows; none on linux (in the .so)."""
        return [] if LINUX else [dll.with_suffix(".pdb")]

    def find_exe(self) -> Path:
        """the engine's exe in whichever steam library has it (steamapps\\libraryfolders.vdf lists them)."""
        if exe := in_libraries(Path(self.steam_folder) / self.exe):
            return exe
        raise ToolError(f"{self.steam_folder} isn't installed (steam app {self.app})")


def in_libraries(relative: Path) -> Path | None:
    """a file of steamapps/common in whichever steam library has it (steamapps\\libraryfolders.vdf lists them)."""
    steam = steam_root()
    libraries = [steam]
    if (vdf := steam / "steamapps/libraryfolders.vdf").is_file():
        listed = re.findall(r'"path"\s+"([^"]+)"', vdf.read_text(encoding="utf-8", errors="replace"))
        libraries += [Path(path.replace("\\\\", "\\")) for path in listed]
    for library in libraries:
        if (found := library / "steamapps/common" / relative).is_file():
            return found
    return None


def sniper_run() -> Path:
    """linux: the steam runtime's sniper container's entry (SteamLinuxRuntime_sniper/run), which the linux engine needs
    to run in: its hl2mp.sh refuses outside it."""
    if run := in_libraries(Path("SteamLinuxRuntime_sniper/run")):
        return run
    raise ToolError("the steam linux runtime 3.0 (sniper) isn't installed: steam installs it with half-life 2: deathmatch")


def steam_root() -> Path:
    """where steam is installed: the registry's SteamPath on windows, ~/.local/share/Steam (or ~/.steam/steam) on linux."""
    if LINUX:
        for root in (Path.home() / ".local/share/Steam", Path.home() / ".steam/steam"):
            if (root / "steamapps").is_dir():
                return root
        raise ToolError("steam not found (no ~/.local/share/Steam or ~/.steam/steam)")
    import winreg  # noqa: PLC0415 windows only

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
            return Path(winreg.QueryValueEx(key, "SteamPath")[0])
    except OSError:
        raise ToolError("steam not found") from None


if LINUX:
    # half-life 2's own engine has no build of halfcraft on linux: deathmatch's only, where digging works too
    ENGINES = {
        "hl2dm": Engine(
            "hl2dm",
            "source-sdk-2013/game/mod_ep1/bin/linux64",
            "bin/linux64",
            320,
            "Half-Life 2 Deathmatch",
            "hl2mp_linux64",
            shader_dll="game_shader_generic_halfcraft",
        ),
    }
else:
    ENGINES = {
        "hl2": Engine("hl2", "source-sdk-2013-sp/sp/game/mod_episodic/bin", "bin", 220, "Half-Life 2", "hl2.exe"),
        "hl2dm": Engine(
            "hl2dm",
            "source-sdk-2013/game/mod_ep1/bin/x64",
            "bin/x64",
            320,
            "Half-Life 2 Deathmatch",
            "hl2mp_win64.exe",
            shader_dll="game_shader_generic_halfcraft",
        ),
    }


def chosen(engine: str) -> list[Engine]:
    """one engine, or both for "all"."""
    return list(ENGINES.values()) if engine == "all" else [ENGINES[engine]]  # argparse offers ENGINES' names only


def installed() -> dict[Engine, Path]:
    """the engines a steam library has, with their exes."""
    found = {}
    for engine in ENGINES.values():
        try:
            found[engine] = engine.find_exe()
        except ToolError:
            continue  # not installed, so it isn't running either
    return found


def games() -> list[tuple[Engine, system.Process]]:
    """the running games and their engines, oldest first: the processes of a steam install of one of the engines.
    another project's hl2.exe (a private-engine build, from some other folder) isn't one: it doesn't load halfcraft's
    dlls or write its saves, and these tools must never touch it.
    """
    exes = {engine.exe.lower(): (engine, exe) for engine, exe in installed().items()}
    found = []
    for process in system.processes(*(engine.exe for engine in installed())):  # oldest first
        engine, exe = exes[process.name.lower()]
        if system.runs_exe(process.pid, exe):
            found.append((engine, process))
    return found


def running() -> tuple[Engine, system.Process] | None:
    """the running game and its engine (the oldest one, when two copies run), or None."""
    return next(iter(games()), None)


def pid_file(engine: Engine, link: str | None) -> Path:
    """where make run writes the pid of the engine's game it starts on the link with this id: hl2.pid in the game
    folder for None (the default link), hl2-<tag>.pid for an id. a ToolError for "" or anything else that isn't an id:
    only None is the default link."""
    return engine.game / ("hl2.pid" if link is None else f"hl2-{file_tag(link)}.pid")


def live_pid(pid_file: Path, exe: Path) -> int | None:
    """the pid in the file when it is a live process of this very exe that started before the file was written (so not
    a pid windows has given to another copy since), else None: no file, no number in it, a dead pid, another exe."""
    try:
        text = pid_file.read_text(encoding="ascii").strip()
        written = pid_file.stat().st_mtime_ns // 100 + system.UNIX_EPOCH_TICKS
    except (OSError, UnicodeDecodeError):
        return None
    if not text.isdigit():
        return None
    pid = int(text)
    if not system.runs_exe(pid, exe):  # also False for one that has exited
        return None
    started = system.started(pid)
    return pid if started is not None and started <= written else None


def game_pid(engine: Engine, link: str | None) -> int | None:
    """the pid of the engine's game make run started on this link, while it runs (live_pid)."""
    return live_pid(pid_file(engine, link), engine.find_exe())


def game_on_link(link: str | None) -> tuple[Engine, int] | None:
    """the game make run started on this link that still runs, of either engine (a link's names are the same for
    both): its engine and pid, or None."""
    if link is not None:
        file_tag(link)  # a ToolError for a bad id, even with no engine installed
    for engine, exe in installed().items():
        if pid := live_pid(pid_file(engine, link), exe):
            return engine, pid
    return None


def unrecorded_games() -> list[tuple[Engine, system.Process]]:
    """the running games no pid file of make run's names (HalfCraft.exe's, a player's, one started by hand): they
    are on the default link, as far as these tools can tell."""
    exes = installed()
    recorded = {pid for engine, exe in exes.items() for file in engine.game.glob("hl2*.pid") if (pid := live_pid(file, exe))}
    return [(engine, process) for engine, process in games() if process.pid not in recorded]
