"""prints what the game and minecraft are telling each other right now (shared memory, read-only).

python tools/debug/hc_state.py [seconds]
"""

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools\, where the halfcraft package is
from halfcraft import protocol
from halfcraft.link import open_link


def dotted(ip: int) -> str:
    """a.b.c.d for the address HostState holds as a << 24 | b << 16 | c << 8 | d."""
    return ".".join(str(ip >> shift & 0xFF) for shift in (24, 16, 8, 0))


def main() -> None:
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
    schema = protocol.load()
    link = open_link(schema)
    regions, structs = schema.regions, schema.structs
    end = time.time() + seconds
    while True:
        header = structs["Header"].read(link, regions["Header"].at)
        host = structs["HostState"].read(link, regions["HostState"].at)
        mc = structs["McState"].read(link, regions["McState"].at)
        print(
            f"host pid {header['hostPid']} flags {host['flags']:#x} world {host['worldId']:08x} epoch {host['collisionEpoch']} "
            f"pos ({host['posX']:.2f} {host['posY']:.2f} {host['posZ']:.2f}) look ({host['yaw']:.1f} {host['pitch']:.1f}) "
            f"tp {host['teleportSeq']} view {host['viewportW']}x{host['viewportH']}"
        )
        print(
            f"mc   pid {header['mcPid']} flags {mc['flags']:#x} pos ({mc['x']:.2f} {mc['y']:.2f} {mc['z']:.2f}) "
            f"look ({mc['yaw']:.1f} {mc['pitch']:.1f}) eye {mc['eyeHeight']:.2f} ack {mc['teleportAck']}"
        )
        print(f"coop mode {host['coopMode']} ip {dotted(host['coopIp'])} port {host['coopPort']}; minecraft coop {mc['coopState']} port {mc['coopPort']}")
        if time.time() >= end:
            break
        time.sleep(0.5)


if __name__ == "__main__":
    main()
