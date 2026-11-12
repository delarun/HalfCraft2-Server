"""keeps test sessions away from the player's own games: half-life's saves and settings, and minecraft's world.

python tools/game/test_session.py start [--engine hl2dm]   puts build\\game-<engine>\\save and cfg\\config.cfg aside
                                                      (build\\test-session\\<engine>): the test starts without saves
python tools/game/test_session.py stop [--engine hl2dm]    puts them back; the test's own saves and settings go to
                                                      build\\test-session\\<engine>-last
python tools/game/test_session.py status                   what's set aside, and which games run (a minecraft line
                                                      for each link)
python tools/game/test_session.py minecraft                the minecraft test client (make mc-test): its own run
                                                      folder (minecraft\\run\\test) and world (HalfCraftTest)
python tools/game/test_session.py minecraft --link guest   the second pair's (make mc-test LINK=guest, co-op tests):
                                                      Gordon2 on link guest, minecraft\\run\\test-guest and
                                                      HalfCraftTestGuest
python tools/game/test_session.py dev-client               the player's minecraft dev client (make mc-run)

half-life's saves carry checkpoints of the player's minecraft world, and every changelevel autosaves over them,
so a test never plays with them. the game folder itself stays: its strings file is named after it
(resource\\<folder>_english.txt). only one minecraft can hold a link, so neither client starts next to another
one on it. the test clients only start while every engine has a test session on, the player's only while none has,
and a session neither starts nor stops while a minecraft runs on any link (the default one, guest's or
HALFCRAFT_LINK's): otherwise one world plays against the other's saves (it follows their loads and rolls back, and
their autosaves land on the wrong saves). --engine all (the default) does both engines. the test client and the
dev client take HALFCRAFT_LINK's link (tools/halfcraft/link.py): a client started with it set takes it, and only a
minecraft on it counts. the guest's client is on link guest whatever HALFCRAFT_LINK says (minecraft\\build.gradle).
"""

import argparse
import shutil
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools\, where the halfcraft package is
from halfcraft import REPO, ToolError, engines, folders, gradle, protocol
from halfcraft.engines import system as windows  # windows.py, or posix.py on linux: the same names
from halfcraft.link import LinkNames, link_names, this_link

SESSIONS = REPO / "build/test-session"
PLAYER_LOG = REPO / "minecraft/run/logs/latest.log"
TEST_LOG = REPO / "minecraft/run/test/logs/latest.log"
# the guest pair's link id in co-op tests on one pc, the one link with a minecraft run of its own (runTestGuestClient)
GUEST = "guest"
GUEST_LOG = REPO / "minecraft/run/test-guest/logs/latest.log"


def __getattr__(name: str) -> str:
    """MINECRAFT_MUTEX, worked out each time it's read (PEP 562) rather than at import, so a bad HALFCRAFT_LINK is a
    ToolError where it's read, not one that stops every action: what minecraft (HostLink.announceRunning) holds while
    it runs on HALFCRAFT_LINK's link (the dev client, the test client or a release's). tools/game/run_minecraft.py
    imports it."""
    if name == "MINECRAFT_MUTEX":
        return this_link(protocol.load()).minecraft_mutex
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


@dataclass(frozen=True)
class Session:
    """where one engine's saves and settings are, and where a test session puts them."""

    engine: engines.Engine

    @property
    def save(self) -> Path:
        return self.engine.game / "save"

    @property
    def config(self) -> Path:
        return self.engine.game / "cfg/config.cfg"

    @property
    def keep(self) -> Path:  # the player's, while a session is on
        return SESSIONS / self.engine.name

    @property
    def last(self) -> Path:  # the last session's own
        return SESSIONS / f"{self.engine.name}-last"

    @property
    def notes(self) -> Path:  # when it started, what it set aside, and whether stop parked the test's
        return self.keep / "session.txt"

    def since(self) -> str:
        return self.notes.read_text(encoding="ascii").splitlines()[0]

    def note(self, *lines: str) -> None:
        with self.notes.open("a", encoding="ascii", newline="\r\n") as notes:
            notes.writelines(f"{line}\n" for line in lines)


def test_links() -> list[LinkNames]:
    """the links a minecraft may run on in a test: the default one, guest's and HALFCRAFT_LINK's (a ToolError when
    that isn't a link id)."""
    schema = protocol.load()
    default = str(schema.constants["kMappingName"].value)
    found = [link_names(default, None), link_names(default, GUEST), this_link(schema)]
    return list({names.mapping: names for names in found}.values())


def described(names: LinkNames) -> str:
    return f"link {names.id}" if names.id else "the default link"


def client_on(names: LinkNames) -> str:
    """which minecraft runs on that link: "none", "test" (make mc-test's), "test-guest" (make mc-test LINK=guest's),
    "player" (the dev client in minecraft\\run) or "other" (a release's). the link's mutex says whether one does; the
    run folder whose latest.log it keeps open for writing says which.
    """
    if not windows.mutex_exists(names.minecraft_mutex):
        return "none"
    if names.id == GUEST:
        return "test-guest" if windows.in_use(GUEST_LOG) else "other"
    if windows.in_use(TEST_LOG):
        return "test"
    if windows.in_use(PLAYER_LOG):
        return "player"
    return "other"


def minecraft_client(link: str | None = None) -> str:
    """client_on the link with this id, HALFCRAFT_LINK's when None."""
    return client_on(this_link(protocol.load(), link))


def ensure_game_stopped() -> None:
    if running := engines.running():
        raise ToolError(f"{running[1].name} runs: quit it first (it writes save\\ and cfg\\config.cfg)")


def ensure_minecraft_stopped(why: str, link: str | None = None, *, any_link: bool = False) -> None:
    """a ToolError saying why when a minecraft runs on the link with this id (HALFCRAFT_LINK's when None), or on any
    of the test_links with any_link."""
    for names in test_links() if any_link else [this_link(protocol.load(), link)]:
        if (client := client_on(names)) != "none":
            raise ToolError(f"the {client} minecraft runs on {described(names)}: {why}")


def with_session(sessions: list[Session], on: bool) -> list[str]:
    """the engines (with a game folder) that have a test session on, or haven't."""
    return [session.engine.name for session in sessions if session.engine.game.is_dir() and session.keep.is_dir() == on]


def start(sessions: list[Session]) -> None:
    ensure_game_stopped()
    ensure_minecraft_stopped("its world would follow the test's loads. quit it, start the session, then the test client (make mc-test)", any_link=True)
    todo = []
    for session in sessions:
        if not session.engine.game.is_dir():
            print(f"{session.engine.name}: no game folder ({session.engine.game}); skipped")
        elif session.keep.is_dir():
            raise ToolError(f"{session.engine.name}: a test session is already on (since {session.since()}); stop it first")
        else:
            todo.append(session)
    for session in todo:
        session.keep.mkdir(parents=True)
        session.note(datetime.now().strftime("%Y-%m-%dT%H:%M:%S"))
        had_save, had_config = session.save.is_dir(), session.config.is_file()
        if had_save:
            shutil.move(session.save, session.keep / "save")
        if had_config:
            shutil.copy2(session.config, session.keep / "config.cfg")
        session.note(f"save={had_save}", f"config={had_config}")
        print(f"{session.engine.name}: test session on; the player's saves and settings are in {session.keep}")


def stop(sessions: list[Session]) -> None:
    ensure_game_stopped()
    ensure_minecraft_stopped("quit it first: with the player's saves back, its world would play against them", any_link=True)
    for session in sessions:
        name = session.engine.name
        if not session.keep.is_dir():
            print(f"{name}: no test session")
            continue
        # the test's own saves and settings go aside once. "parked" says they went, so a stop run again after
        # one that broke off never takes the player's (already back) for the test's
        parked = session.notes.is_file() and "parked" in (line.lower() for line in session.notes.read_text(encoding="ascii").splitlines())
        if not parked:
            folders.remove_tree(session.last)
            session.last.mkdir()
            if session.save.exists():
                shutil.move(session.save, session.last / "save")
            if session.config.exists():
                shutil.move(session.config, session.last / "config.cfg")
            session.note("parked")
        # whatever is still in the session folder is the player's: it goes back, never over anything
        for kept, back in ((session.keep / "save", session.save), (session.keep / "config.cfg", session.config)):
            if not kept.exists():
                continue
            if back.exists():
                raise ToolError(f"{name}: {back} came back since the last stop; move it away and stop again (the player's is in {kept})")
            shutil.move(kept, back)
        session.notes.unlink()
        session.keep.rmdir()
        print(f"{name}: test session off; the player's saves and settings are back, the test's are in {session.last}")


def status(sessions: list[Session]) -> None:
    for session in sessions:
        print(f"{session.engine.name}: test session on since {session.since()}" if session.keep.is_dir() else f"{session.engine.name}: no test session")
    running = engines.running()
    print(f"half-life: {f'{running[1].name} runs' if running else 'not running'}")
    for names in test_links():
        print(f"minecraft: {client_on(names)} on {described(names)}")


def test_client(sessions: list[Session], link: str | None = None) -> None:
    """make mc-test's client (runTestClient) on HALFCRAFT_LINK's link, or with link guest the guest pair's
    (runTestGuestClient), which is on link guest whatever HALFCRAFT_LINK says (minecraft\\build.gradle)."""
    if link is not None and link != GUEST:
        raise ToolError(f"--link {link!r}: only {GUEST} has a minecraft test client of its own (runTestGuestClient)")
    task, log = ("runTestClient", TEST_LOG) if link is None else ("runTestGuestClient", GUEST_LOG)
    ensure_minecraft_stopped("two would fight over the one link", link)
    if windows.in_use(log):  # one on another link, from the same run folder: two can't hold one world
        raise ToolError(f"another minecraft plays from {log.parent.parent}: quit it first")
    if missing := with_session(sessions, on=False):
        raise ToolError(f"no test session for {', '.join(missing)}: the test world would autosave over the player's saves. make test-start first")
    gradle.run([task])


def dev_client(sessions: list[Session]) -> None:
    ensure_minecraft_stopped("two would fight over the one link")
    if on := with_session(sessions, on=True):
        raise ToolError(f"a test session is on for {', '.join(on)}: the player's world would play against the test's saves. make test-stop first")
    gradle.run(["runClient"])


ACTIONS = {"start": start, "stop": stop, "status": status, "minecraft": test_client, "dev-client": dev_client}


def main() -> None:
    parser = argparse.ArgumentParser(description="keep test sessions away from the player's own games")
    parser.add_argument("action", choices=list(ACTIONS))
    parser.add_argument("--engine", choices=["all", *engines.ENGINES], default="all")
    parser.add_argument("--link", help=f"minecraft only: {GUEST}, the second pair's client")
    options = parser.parse_args()
    sessions = [Session(engine) for engine in engines.chosen(options.engine)]
    if options.action == "minecraft":
        test_client(sessions, options.link)
    elif options.link is not None:
        raise ToolError(f"--link is for minecraft only: {options.action} covers every link")
    else:
        ACTIONS[options.action](sessions)


if __name__ == "__main__":
    main()
