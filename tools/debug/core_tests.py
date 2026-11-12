"""runs the c++ core's own tests: plain checks over source\\src\\core's headers, the code server.dll and client.dll
share, with no sdk and no game. builds tools\\debug\\core_tests.cpp into build\\core_tests (warnings are errors; with g++ on linux) and
runs it.

python tools/debug/core_tests.py

exits 0 when every check passes, 1 when one fails (the failed ones are listed), 2 when the tests can't run.
"""

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools\, where the halfcraft package is
from halfcraft import REPO, ToolError, commands, visual_studio

OUT = REPO / "build/core_tests"
LINUX = os.name != "nt"
EXE = OUT / ("core_tests" if LINUX else "core_tests.exe")
SOURCE = REPO / "tools/debug/core_tests.cpp"
COMPILE_SECONDS = 600
RUN_SECONDS = 600


def build() -> None:
    """compiles the tests when they or the headers they take from core\\ and protocol\\ changed since."""
    sources = [SOURCE, *(REPO / "source/src/core").glob("hc_*.h"), REPO / "protocol/halfcraft_protocol.h"]
    if EXE.is_file() and all(source.stat().st_mtime <= EXE.stat().st_mtime for source in sources):
        return
    if LINUX:
        OUT.mkdir(parents=True, exist_ok=True)
        gcc = ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", REPO / "source/src", "-I", REPO / "protocol", "-o", EXE, SOURCE]
        commands.run(gcc, timeout=COMPILE_SECONDS, what="core_tests build")
        return
    vs = visual_studio.installation("Microsoft.VisualStudio.Component.VC.Tools.x86.x64")
    if not vs:
        raise ToolError("visual studio with the c++ compiler not found")
    OUT.mkdir(parents=True, exist_ok=True)
    cl = f'cl /nologo /O2 /W4 /WX /EHsc /std:c++17 /I "{REPO}\\source\\src" /I "{REPO}\\protocol" /Fo"{OUT}\\\\" /Fe"{EXE}" "{SOURCE}" || exit /b 1'
    visual_studio.run_script(vs, OUT / "build.cmd", [cl], timeout=COMPILE_SECONDS, what="core_tests build")


def main() -> int:
    build()
    return commands.run([EXE], timeout=RUN_SECONDS, what="core_tests", check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
