# HalfCraft 2 Server: design and milestones

- Date: 2026-11-12
- Goal: a persistent server people join whenever they like. Every player runs Half-Life 2: Deathmatch's engine with
  HalfCraft's client and a hidden Minecraft, as in single player. The main map is the whole Highway 17 chapter
  (`d2_coast_01` … `d2_coast_08`). Level transitions are Minecraft portals.
- Platform: the server runs on Linux. Clients run on Linux (built and tested here) and Windows (upstream's build, not
  tested in this fork yet).
- Builds on: upstream's co-op M1 work (`cooptest` branch, `docs/superpowers` spec at `69f293c`): the link id, the
  host's player helper, Minecraft's collision relay, the C2S teleport and dig payloads.

## What runs where

```
 SERVER MACHINE (linux)                                           EACH PLAYER (linux or windows)
 srcds_linux64 -game game-hl2dm  (one per Highway 17 map)          hl2mp_linux64 / hl2mp_win64 (client.so/.dll)
   server.so ── server link (shared memory, /dev/shm) ──┐            │ client link (as today, client regions only)
                                                          │            ▼
 Minecraft dedicated server (Fabric, HalfCraft mod)  <────┘         hidden Minecraft client (HalfCraft mod)
   one world, the maps side by side in slots along x  ◄── TCP ───► joins the server on its own
   collision from server.so, relayed to each client (S2C)
   digging, teleports from clients (C2S)
```

- **Source.** One `srcds_linux64` per map, each with HalfCraft's `server.so`. A Source server runs one map, so a
  chapter of 8 maps is 8 servers, each on its own port. A player is on one map at a time.
- **Minecraft.** One dedicated Fabric server. Every Half-Life map already has its own slot along x in HalfCraft's mirror
  world (`Rollback.mapEntered`, `SLOT_CHUNKS`), so all 8 maps live in one world. A player in Minecraft is where their
  Source player is.
- **Server link.** Today `server.dll` attaches to the mapping `client.dll` creates in the same process. In a dedicated
  server there is no `client.dll`: `server.so` creates its own mapping (link id `srv-<map>`) with the server's regions
  (collision ring, actor table, event ring, weapon tables, mob table, its debug). The Minecraft server opens one per map.
- **Client link.** Unchanged, less the server's regions: host state, input, overlay, render ring, world entities, water,
  Minecraft's state.
- **A portal.** At each `trigger_changelevel`, the Minecraft server places a portal. Stepping in moves the Minecraft
  player to the next map's slot and tells the client to `connect` its Source to that map's server.

## What has to change (from the code as it is)

### Minecraft mod
- `fabric.mod.json` is `"environment": "client"`: the mod must load on a dedicated server too. Client-only code stays in
  the client entrypoint.
- The server side opens the server links itself. The collision consumer (`HostCollision`, today started by
  `HostClient`) runs on the server, and so does the smooth triangle collider for every player (today only the client
  turns it on: `HalfCraftClient.java:173`).
- Collision reaches each client from the server (upstream's `CollisionRelay`, S2C), nearest regions first. Clients
  need it for their own movement prediction.
- What the client sends the integrated server today through `getSingleplayerServer()` becomes either a C2S payload
  (teleport, dig, `hc_mc`) or comes from `server.so` over the server link (hurts, heals, blasts, bullets, held blocks).
- Server code that takes "the first player" (`HostCombat`, `HostMobs`, `HostBlockDamage`) works per player and per map
  slot. `WeaponTable` is per player.
- `Rollback` (single-player saves) is off on the server: the world simply persists.

### Source (`server.so`)
- No local player: the 7 `UTIL_GetLocalPlayer()` uses become per-player (`for_each_player`), and Valve's helpers
  (`UTIL_GetLocalPlayer`, `AI_GetSinglePlayer`) need a choice for NPCs and scripts (nearest player).
- The 14 functions `server.dll` takes from `client.dll` in the same process (`find_export("client.dll", …)`: solids,
  dug cells, hazards, jump landed, push hurt/heal, set holding, …) aren't there on a dedicated server. Each one comes
  from the Minecraft server over the server link instead, or from the client over the network.
- Collision streams around every player (upstream M1: `CollisionStreamer` centres).
- Spawns, respawn and solidity of players (upstream M1's Valve-code fixes).

## Milestones

| | Name | Done when |
|---|---|---|
| **S0** | Source dedicated runs the campaign | `srcds_linux64` with HalfCraft's `server.so` runs `d2_coast_01`; two clients (two copies on one PC) connect, walk, shoot NPCs, die and respawn. Minecraft is off. |
| **S1** | One Minecraft server | The mod loads on a dedicated Fabric server, which links to `server.so` and relays collision. Each client's Minecraft joins it by itself. Two players move with their own Minecraft, see each other, and place and break blocks both see. Digging works for both. |
| **S2** | Per-player game | Health, hurts, heals, weapons, death and respawn per player. NPCs and bullets collide with Minecraft blocks and dug holes (from the Minecraft server). Mobs and actors near any player. |
| **S3** | Highway 17 | Eight servers, eight slots, portals at the transitions. A player walks from `d2_coast_01` to `d2_coast_08` and back. The buggy. |
| **S4** | Running it | One command (systemd units or compose) starts and stops all of it. Configuration, a whitelist, backups. A client package for players. |

## Decisions (the user's, 2026-11-12)

| Question | Decision |
|---|---|
| Minecraft accounts | **Online mode**: every player has a licensed Minecraft Java; names are Microsoft accounts. |
| The story | **A sandbox with NPCs**: the maps' story triggers are taken out; enemies (antlions, soldiers) spawn and respawn. Highway 17 as an open world to build and survive in. |
| Minecraft's game mode | **Survival** (creative for admins). |
| Players at once | **Up to 8** (`maxplayers 8` on every map's server). |
