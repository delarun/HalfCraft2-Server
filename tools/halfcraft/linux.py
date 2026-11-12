"""halfcraft's half-life 2 side built on linux, for half-life 2: deathmatch's linux engine (hl2mp_linux64): what
build.py does with visual studio on windows, in containers here.

- the world shaders: the sdk's ShaderCompile2.exe (windows only) runs under wine in a container with no network and
  only the shader folders mounted (tools/build/docker/wine-shaders.Dockerfile). it compiles with microsoft's
  d3dcompiler_47.dll, which wine doesn't ship: HALFCRAFT_D3DCOMPILER names one, or one of D3DCOMPILERS is taken.
  checked: its .vcs files are byte for byte those a windows build makes.
- client.so, server.so and game_shader_generic_halfcraft.so: valve's vpc and ninja in the steam runtime's sniper sdk
  image (what hl2dm-sp's own buildallprojects runs, with podman), the repo mounted at its own path so the patch's
  $HALFCRAFTROOT reaches source/. halfcraft's own code builds without warnings: one there fails the build.
"""

import os
import re
import shutil
import struct
import subprocess
from collections.abc import Callable
from pathlib import Path

from halfcraft import REPO, ToolError, commands, sdk, shaders

SNIPER = "registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest"
WINE_IMAGE = "halfcraft-wine-shaders"
WINE_DOCKERFILE = REPO / "tools/build/docker/wine-shaders.Dockerfile"
COMPILER = REPO / "build/shadercompile"  # ShaderCompile2.exe and d3dcompiler_47.dll, mounted read-only
STAGE = REPO / "build/shaders"
LOGS = REPO / "build/logs"
NINJA = "_vpc_/ninja/halfcraft_release"
# microsoft's d3dcompiler_47.dll (64-bit) where linux machines tend to have one: PortProton's d3d_extras, winetricks'
# cache of the one it installs
D3DCOMPILERS = (
    Path.home() / "Apps/PortProton/data/tmp/plugins_v20/d3d_extras/x64/d3dcompiler_47.dll",
    Path.home() / "PortProton/data/tmp/plugins_v20/d3d_extras/x64/d3dcompiler_47.dll",
    Path.home() / ".cache/winetricks/d3dcompiler_47/d3dcompiler_47.dll",
)
SHADER_SECONDS = 4 * 3600  # hc_lightmappedgeneric_ps2x.fxc takes 43 minutes on windows; wine is slower
BUILD_SECONDS = 3600
DOCKER_SECONDS = 1800
OURS = re.compile(r"(?:^|[\s/])(?:\.\./)*source/src/\S+:\d+:\d+: (?:warning|error):")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def docker(*arguments: str | Path, timeout: float, what: str, capture: bool = False, check: bool = True) -> subprocess.CompletedProcess[str]:
    return commands.run(["docker", *arguments], timeout=timeout, what=what, capture=capture, check=check)


def user() -> list[str]:
    """the container runs as this user, so what it writes into the repo is theirs."""
    return ["--user", f"{os.getuid()}:{os.getgid()}"]


def d3dcompiler() -> Path:
    """microsoft's d3dcompiler_47.dll: HALFCRAFT_D3DCOMPILER's, else the first of D3DCOMPILERS there is."""
    if named := os.environ.get("HALFCRAFT_D3DCOMPILER"):
        if not Path(named).is_file():
            raise ToolError(f"HALFCRAFT_D3DCOMPILER names {named}, which isn't there")
        return Path(named)
    for candidate in D3DCOMPILERS:
        if candidate.is_file():
            return candidate
    raise ToolError(
        "no d3dcompiler_47.dll (microsoft's, 64-bit) for the shader compile: set HALFCRAFT_D3DCOMPILER to one "
        "(winetricks d3dcompiler_47 caches it in ~/.cache/winetricks)"
    )


def wine_image() -> None:
    """the wine image, built once (the only step that needs the network)."""
    if docker("image", "inspect", WINE_IMAGE, timeout=60, what="docker image inspect", capture=True, check=False).returncode == 0:
        return
    docker("build", "-q", "-t", WINE_IMAGE, "-f", WINE_DOCKERFILE, WINE_DOCKERFILE.parent, timeout=DOCKER_SECONDS, what="docker build of the wine image")


def compile_shader(source: str) -> str:
    """ShaderCompile2 on one .fxc in STAGE, in the wine container; its output."""
    uid, gid = os.getuid(), os.getgid()
    # wine wants a prefix its user owns: a tmpfs home of theirs, made afresh each run
    script = f"wineboot --init >/dev/null 2>&1; wine /sc/ShaderCompile2.exe -ver 20b -shaderpath 'Z:\\shaders' {source}"
    run = [
        "run", "--rm", "--network", "none", *user(),
        "--tmpfs", f"/home/hc:uid={uid},gid={gid},mode=0700,exec",
        "-e", "HOME=/home/hc", "-e", "WINEPREFIX=/home/hc/wine",
        "-v", f"{COMPILER}:/sc:ro", "-v", f"{STAGE}:/shaders",
        WINE_IMAGE, "sh", "-c", script,
    ]  # fmt: skip
    compiled = docker(*run, timeout=SHADER_SECONDS, what=f"ShaderCompile2 on {source}", capture=True, check=False)
    return f"{compiled.stdout}\n{compiled.stderr}"


def build_shaders(engines: list, shader_errors: Callable[[str], int | None]) -> None:
    """build.build_shaders on linux: the same stage, the same checks, ShaderCompile2 under wine."""
    src = sdk.TREES["hl2dm"].source()
    stdshaders = src / "materialsystem/stdshaders"
    STAGE.mkdir(parents=True, exist_ok=True)
    COMPILER.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "devtools/bin/ShaderCompile2.exe", COMPILER)
    shutil.copy2(d3dcompiler(), COMPILER / "d3dcompiler_47.dll")
    wine_image()
    for header in stdshaders.glob("*.h"):
        shutil.copy2(header, STAGE)
    shaders.OUT.mkdir(exist_ok=True)
    dll = any(engine.shader_dll for engine in engines)
    for shader in shaders.SHADERS:
        if shader.dll and not dll:
            continue
        shutil.copy2(stdshaders / shader.source, STAGE)
        output = compile_shader(shader.source)
        vcs = STAGE / f"shaders/fxc/{shader.name}.vcs"
        if errors := shader_errors(output):
            said = [line.strip() for line in re.split(r"[\r\n]", output) if re.search(r"\berror\b", line, re.IGNORECASE) and "WARNINGS/ERRORS" not in line]
            print("\n".join(said))
            raise ToolError(f"ShaderCompile2 found {errors} error(s) in {shader.source}")
        if not vcs.is_file():
            raise ToolError(f"ShaderCompile2 made no {vcs.name} from {shader.source}:\n{output.strip()[-2000:]}")
        if shader.combos is not None and struct.unpack_from("<iii", vcs.read_bytes()) != (6, shader.combos, shader.dynamic):
            raise ToolError(f"{shader.name}.vcs doesn't have the stock combo layout")
        shutil.copy2(vcs, shaders.OUT)


def build_hl2dm(*, projects: bool, shader_dll: str | None) -> None:
    """client.so, server.so and the shader .so in the sniper sdk image: vpc writes the ninja file (projects), ninja
    builds, and a warning or error in halfcraft's own files fails the build."""
    src = sdk.TREES["hl2dm"].source()
    groups = ["+game", *((f"+{shader_dll}",) if shader_dll else ())]
    vpc = f"devtools/bin/vpc /episodic /linux64 /ninja /define:SOURCESDK {' '.join(groups)} /mksln {NINJA}"
    steps = [vpc] if projects or not (src / f"{NINJA}.ninja").is_file() else []
    steps.append(f"ninja -f {NINJA}.ninja -j$(nproc)")
    run = [
        "run", "--rm", "--network", "none", *user(), "-e", "HOME=/tmp", "-e", "VPC_NINJA_BUILD_MODE=release",
        "-v", f"{REPO}:{REPO}", "-w", src, SNIPER, "bash", "-c", " && ".join(steps),
    ]  # fmt: skip
    LOGS.mkdir(parents=True, exist_ok=True)
    built = docker(*run, timeout=BUILD_SECONDS, what="the linux build (vpc and ninja in the sniper sdk)", capture=True, check=False)
    output = ANSI.sub("", f"{built.stdout}\n{built.stderr}")
    (LOGS / "hl2dm-linux64.log").write_text(output, encoding="utf-8")
    ours = sorted({line.strip() for line in output.splitlines() if OURS.search(line)})
    if ours:
        print("\n".join(ours))
        raise ToolError(f"halfcraft's code has {len(ours)} warnings or errors (above; the whole log: {LOGS / 'hl2dm-linux64.log'})")
    if built.returncode != 0:
        print("\n".join(output.splitlines()[-40:]))
        raise ToolError(f"the linux build failed ({built.returncode}); the whole log: {LOGS / 'hl2dm-linux64.log'}")
