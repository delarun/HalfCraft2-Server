"""console commands into a running halfcraft dedicated server, through source's rcon (tcp, the server's port).

python tools/server/rcon.py status
python tools/server/rcon.py --port 27016 "ent_text prop_vehicle_jeep" status

the password is build/server/rcon.txt, which tools/server/srcds.py makes once (random) and gives every server it runs.
"""

import argparse
import secrets
import socket
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/, where the halfcraft package is
from halfcraft import REPO, ToolError

PASSWORD = REPO / "build/server/rcon.txt"
AUTH, AUTH_RESPONSE, EXEC, RESPONSE = 3, 2, 2, 0
SECONDS = 10


def password() -> str:
    """the servers' rcon password, made the first time."""
    if not PASSWORD.is_file():
        PASSWORD.parent.mkdir(parents=True, exist_ok=True)
        PASSWORD.write_text(secrets.token_urlsafe(24) + "\n", encoding="ascii")
        PASSWORD.chmod(0o600)
    return PASSWORD.read_text(encoding="ascii").strip()


def packet(request: int, kind: int, body: str) -> bytes:
    payload = struct.pack("<ii", request, kind) + body.encode("utf-8") + b"\0\0"
    return struct.pack("<i", len(payload)) + payload


def receive(sock: socket.socket) -> tuple[int, int, str]:
    def exactly(count: int) -> bytes:
        data = b""
        while len(data) < count:
            if not (chunk := sock.recv(count - len(data))):
                raise ToolError("the server closed the rcon connection")
            data += chunk
        return data

    (size,) = struct.unpack("<i", exactly(4))
    request, kind = struct.unpack("<ii", exactly(8))
    body = exactly(size - 8)
    return request, kind, body[:-2].decode("utf-8", errors="replace")


def run(commands: list[str], *, host: str, port: int) -> str:
    """runs each command and returns what the server answered, in order."""
    with socket.create_connection((host, port), timeout=SECONDS) as sock:
        sock.sendall(packet(1, AUTH, password()))
        while True:
            request, kind, _ = receive(sock)
            if kind == AUTH_RESPONSE:
                if request == -1:
                    raise ToolError("the server refused the rcon password (it was started before build/server/rcon.txt?)")
                break
        out = []
        for number, command in enumerate(commands, start=10):
            # an empty RESPONSE request after the command: its echo marks the end of a reply split over packets
            sock.sendall(packet(number, EXEC, command) + packet(number + 1000, RESPONSE, ""))
            while True:
                request, _, body = receive(sock)
                if request == number + 1000:
                    break
                out.append(body)
        return "".join(out)


def main() -> None:
    parser = argparse.ArgumentParser(description="rcon commands into halfcraft's dedicated server")
    parser.add_argument("commands", nargs="+")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=27015)
    options = parser.parse_args()
    print(run(options.commands, host=options.host, port=options.port), end="")


if __name__ == "__main__":
    main()
