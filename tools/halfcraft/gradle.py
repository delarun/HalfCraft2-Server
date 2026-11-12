"""the minecraft mod's gradle, on a JDK 25 (the mod needs one) whatever JAVA_HOME points at, with halfcraft's
version (halfcraft.version).
"""

import os
import re
from collections.abc import Sequence
from pathlib import Path

from halfcraft import REPO, ToolError, commands
from halfcraft.version import version

MINECRAFT = REPO / "minecraft"
JDK_VENDORS = ("Eclipse Adoptium", "Java", "Microsoft", "Zulu", "Amazon Corretto")
# a build, its tests or a check; the clients run as long as they're played
SECONDS = 3600
CLIENTS = {"runClient", "runTestClient", "runTestGuestClient"}


# gradle (gradle/wrapper/gradle-wrapper.properties) runs on these JDKs: a newer one fails in its build script ("Unsupported
# class file major version")
NEWEST_GRADLE_JDK = 26
LINUX = os.name != "nt"


def jdk_version(jdk: Path) -> int | None:
    """a JDK's feature version (25), from its release file; None for what isn't a JDK."""
    release = jdk / "release"
    if not (jdk / ("bin/javac" if LINUX else "bin/javac.exe")).is_file() or not release.is_file():
        return None
    found = re.search(r'JAVA_VERSION="(\d+)', release.read_text(encoding="utf-8", errors="replace"))
    return int(found[1]) if found else None


def find_jdk() -> Path:
    """JAVA_HOME if it's 25 or newer (and gradle runs on it), else one installed: windows' program files, newest first;
    linux's /usr/lib/jvm, oldest from 25 first (a distribution's default java can be newer than gradle runs on)."""
    candidates = [Path(home)] if (home := os.environ.get("JAVA_HOME")) else []
    if LINUX:
        jvm = Path("/usr/lib/jvm")
        installed = [jdk for jdk in jvm.iterdir() if jdk.is_dir() and not jdk.is_symlink()] if jvm.is_dir() else []
        candidates += sorted(installed, key=lambda jdk: jdk_version(jdk) or 0)
    else:
        # PROGRAMW6432 is the 64-bit program files even in a process a 32-bit one started (ezwinports' make)
        program_files = Path(os.environ.get("PROGRAMW6432") or os.environ["PROGRAMFILES"])
        for vendor in JDK_VENDORS:
            if (root := program_files / vendor).is_dir():
                candidates += sorted((jdk for jdk in root.iterdir() if jdk.is_dir()), key=lambda jdk: jdk.name, reverse=True)
    for jdk in candidates:
        if (found := jdk_version(jdk)) and 25 <= found <= NEWEST_GRADLE_JDK:
            return jdk
    raise ToolError(f"no JDK 25 to {NEWEST_GRADLE_JDK} found: set JAVA_HOME to one")


def run(tasks: Sequence[str]) -> None:
    """runs gradle tasks in minecraft\\ and waits for them."""
    env = {**os.environ, "JAVA_HOME": str(find_jdk())}
    timeout = None if CLIENTS.intersection(tasks) else SECONDS
    command = [MINECRAFT / ("gradlew" if LINUX else "gradlew.bat"), *tasks, "--no-configuration-cache", f"-Pversion={version()}"]
    if not LINUX and os.environ.get("HALFCRAFT_DESKTOP"):  # windows.HIDDEN_DESKTOP_ENV
        # tools/game/hidden_desktop.py's: a daemon started there would start every later client there too
        command.append("--no-daemon")
    commands.run(command, timeout=timeout, what=f"gradle {' '.join(tasks)}", cwd=MINECRAFT, env=env)
