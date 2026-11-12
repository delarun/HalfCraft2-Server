package dev.halfcraft.world;

import dev.halfcraft.link.HostSide;
import dev.halfcraft.link.Proto;
import dev.halfcraft.link.ServerLink;
import dev.halfcraft.mobs.HostNav;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Set;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerChunkEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.chunk.LevelChunk;

/**
 * The HalfCraft server's dug cells for its Source server (docs/server/PLAN.md): server.so cuts them out of the
 * collision it streams, so a hole has inner faces to dig on, as a single player's client.dll hands its server.dll the
 * cells its Minecraft dug. A changed section goes as a kRenDug into the server link's render ring; a new source
 * session (srcds restarted) gets every loaded chunk's dug sections again.
 */
public final class ServerDug {
	private static final Set<Long> LOADED = new HashSet<>(); // chunks of the mirror world with cells dug, loaded
	private static final Set<Long> PENDING = new LinkedHashSet<>(); // sections to send: BlockPos.asLong(cx, sy, cz)
	private static long session;

	private ServerDug() {
	}

	public static void init() {
		ServerChunkEvents.CHUNK_LOAD.register((level, chunk, generated) -> {
			if (HostNav.inMirror(level) && chunk.getAttached(HostDig.DUG) != null) {
				LOADED.add(chunk.getPos().pack());
				queue(chunk);
			}
		});
		ServerChunkEvents.CHUNK_UNLOAD.register((level, chunk) -> LOADED.remove(chunk.getPos().pack()));
		ServerTickEvents.END_SERVER_TICK.register(ServerDug::tick);
	}

	/** A cell of a chunk was dug (HostDig): its section goes to server.so. Server thread. */
	static void changed(LevelChunk chunk, BlockPos pos) {
		LOADED.add(chunk.getPos().pack());
		PENDING.add(BlockPos.asLong(chunk.getPos().x(), pos.getY() >> 4, chunk.getPos().z()));
	}

	private static void queue(LevelChunk chunk) {
		DugColumn column = chunk.getAttached(HostDig.DUG);
		if (column != null) {
			for (DugColumn.DugSection section : column.sections()) {
				PENDING.add(BlockPos.asLong(chunk.getPos().x(), section.sectionY(), chunk.getPos().z()));
			}
		}
	}

	private static void tick(MinecraftServer server) {
		ServerLink link = HostSide.dedicated();
		if (link == null || !link.active()) {
			return;
		}
		ServerLevel level = server.overworld();
		if (link.session() != session) {
			// srcds started over: it knows no hole yet
			session = link.session();
			for (long packed : LOADED) {
				LevelChunk chunk = level.getChunkSource().getChunkNow(ChunkPos.getX(packed), ChunkPos.getZ(packed));
				if (chunk != null) {
					queue(chunk);
				}
			}
		}
		for (var it = PENDING.iterator(); it.hasNext();) {
			long key = it.next();
			int sx = BlockPos.getX(key), sy = BlockPos.getY(key), sz = BlockPos.getZ(key);
			LevelChunk chunk = level.getChunkSource().getChunkNow(sx, sz);
			long[] bits = chunk != null ? HostDig.sectionBits(chunk, sy) : null;
			int count = 0;
			if (bits != null) {
				for (long word : bits) {
					count += Long.bitCount(word);
				}
			}
			ByteBuffer header = ByteBuffer.allocate(Proto.REN_DUG_BYTES).order(ByteOrder.LITTLE_ENDIAN).putInt(sx).putInt(sy).putInt(sz).putInt(count).flip();
			ByteBuffer body = count > 0 ? ByteBuffer.wrap(DugBits.toBytes(bits)) : ByteBuffer.allocate(0);
			if (!link.writeRender(Proto.REN_DUG, header, body)) {
				return; // the ring is full: the rest go next tick
			}
			it.remove();
		}
	}
}
