"""what linux knows about other processes, through /proc: the part of windows.py's api the tools share (engines.py,
commands.py), in the same units (a process's start in windows' 100 ns ticks since 1601, utc), so the code using it
reads the same on both systems."""

import fcntl
import os
import signal
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

# 1970-01-01 in windows' ticks (100 ns since 1601), for Process.started against a file's time
UNIX_EPOCH_TICKS = 116444736000000000
_TICKS_PER_SECOND = 10_000_000
_PROC = Path("/proc")


@dataclass(frozen=True)
class Process:
    pid: int
    name: str  # its exe's file name, e.g. hl2mp_linux64
    started: int | None  # when, in windows' 100 ns ticks; None when /proc won't say


def _boot_seconds() -> float | None:
    """when the machine started, in unix seconds (/proc/stat's btime)."""
    try:
        for line in (_PROC / "stat").read_text(encoding="ascii").splitlines():
            if line.startswith("btime "):
                return float(line.split()[1])
    except OSError:
        return None
    return None


def started(pid: int) -> int | None:
    """when a process started, in windows' 100 ns ticks (utc); None when /proc won't say."""
    try:
        stat = (_PROC / str(pid) / "stat").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    # the command's name is in parentheses and may hold spaces: the fields come after its last ')'
    fields = stat[stat.rfind(")") + 2 :].split()
    boot = _boot_seconds()
    if boot is None or len(fields) < 20:
        return None
    seconds = boot + int(fields[19]) / os.sysconf("SC_CLK_TCK")  # field 22: starttime, in clock ticks since boot
    return int(seconds * _TICKS_PER_SECOND) + UNIX_EPOCH_TICKS


def now() -> int:
    """the time in windows' 100 ns ticks, the unit of Process.started (both utc)."""
    return time.time_ns() // 100 + UNIX_EPOCH_TICKS


def exe_path(pid: int) -> str | None:
    """the full path of a process's exe, None when /proc won't say (another user's, or one that has exited)."""
    try:
        return str((_PROC / str(pid) / "exe").readlink())
    except OSError:
        return None


def runs_exe(pid: int, exe: Path) -> bool:
    """whether a process runs this very exe (not just one of its name): the paths resolved."""
    path = exe_path(pid)
    return path is not None and Path(path).resolve() == exe.resolve()


def processes(*names: str) -> list[Process]:
    """the running processes whose exe has one of these names, oldest first."""
    wanted = set(names)
    found = []
    for entry in _PROC.iterdir():
        if not entry.name.isdigit():
            continue
        path = exe_path(int(entry.name))
        if path is not None and Path(path).name in wanted:
            found.append(Process(int(entry.name), Path(path).name, started(int(entry.name))))
    return sorted(found, key=lambda process: (process.started is None, process.started or 0))


def _children(pid: int) -> list[int]:
    try:
        tasks = (_PROC / str(pid) / "task").iterdir()
        return [int(child) for task in tasks for child in (task / "children").read_text(encoding="ascii").split()]
    except OSError:
        return []


def kill_tree(pid: int) -> None:
    """ends a process and every process it started, the children first gathered so none is orphaned unseen."""
    tree = [pid]
    for parent in tree:
        tree += _children(parent)
    for each in reversed(tree):
        try:
            os.kill(each, signal.SIGKILL)
        except OSError:
            continue


def mutex_exists(name: str) -> bool:
    """whether some process holds a link's mark (windows' named mutex; here a lock, fcntl's, as java's FileLock takes
    it, on /dev/shm/<its posix name>: link.py's minecraft_mutex, e.g. Local\\HalfCraft_v1_minecraft)."""
    file = Path("/dev/shm") / name[name.rfind("\\") + 1 :]  # noqa: S108 the shared memory's folder, not a temporary file
    try:
        fd = os.open(file, os.O_RDONLY | os.O_CLOEXEC)
    except OSError:
        return False
    try:
        # struct flock: l_type, l_whence (shorts), l_start, l_len (off_t), l_pid (int), as x86_64 lays it out
        query = struct.pack("hhqqi4x", fcntl.F_WRLCK, os.SEEK_SET, 0, 0, 0)
        answer = fcntl.fcntl(fd, fcntl.F_GETLK, query)
        return struct.unpack("hhqqi4x", answer)[0] != fcntl.F_UNLCK
    finally:
        os.close(fd)


def in_use(path: Path) -> bool:
    """whether another process has the file open for writing (a running minecraft's latest.log)."""
    try:
        target = path.resolve(strict=True)
    except OSError:
        return False
    for entry in _PROC.iterdir():
        if not entry.name.isdigit() or int(entry.name) == os.getpid():
            continue
        try:
            fds = list((entry / "fd").iterdir())
        except OSError:
            continue
        for fd in fds:
            try:
                if fd.readlink() != target:
                    continue
                flags = int((entry / "fdinfo" / fd.name).read_text(encoding="ascii").split("flags:")[1].split()[0], 8)
            except (OSError, IndexError, ValueError):
                continue
            if flags & (os.O_WRONLY | os.O_RDWR):
                return True
    return False


def open_shared(path: Path) -> BinaryIO | None:
    """a file opened for reading that its writer can still write, rename or delete (a game's log), or None."""
    try:
        return path.open("rb")
    except FileNotFoundError:
        return None
