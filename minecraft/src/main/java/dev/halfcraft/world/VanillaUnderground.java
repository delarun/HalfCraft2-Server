package dev.halfcraft.world;

import dev.halfcraft.HalfCraft;
import it.unimi.dsi.fastutil.longs.Long2ObjectLinkedOpenHashMap;
import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.levelgen.Heightmap;
import org.jspecify.annotations.Nullable;

/**
 * Minecraft's own underground under Half-Life's map: a hidden level (halfcraft:underground, the data pack's dimension)
 * that Minecraft generates as its overworld, from the world's seed, and nobody enters. A cell of the mirror world's
 * natural rock is revealed as the block at the same x and z there, {@link #SHIFT} blocks up: its stone, deepslate, ores,
 * caves, aquifers, geodes and the rest, as a Minecraft world has them. Over that level's own ground (its surface, its
 * seas) the rock is stone: the mirror world's ground is Half-Life's. Its chunks are generated when a reveal first asks
 * for them, in the background: the reveal waits for them (HostDig retries). Server thread only.
 */
public final class VanillaUnderground {
	public static final ResourceKey<Level> LEVEL = ResourceKey.create(Registries.DIMENSION, Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "underground"));
	/** Blocks up from the mirror world to the underground level: Half-Life's ground (y 0-30) lies at Minecraft's sea level. */
	static final int SHIFT = Underground.SHIFT;
	/** Blocks under its own surface the underground level's blocks start: its topsoil and its grass stay out. */
	private static final int UNDER_SURFACE = 4;
	/** The underground level's chunks kept at hand, most recently used last. */
	private static final int CHUNKS_KEPT = 512;
	private static final Long2ObjectLinkedOpenHashMap<LevelChunk> CHUNKS = new Long2ObjectLinkedOpenHashMap<>();
	private static final LongOpenHashSet WAITING = new LongOpenHashSet();
	private static boolean missingLogged;
	private static int generated;

	private VanillaUnderground() {
	}

	/** Whether the server has the underground level (the mod's data pack is on). */
	public static boolean available(MinecraftServer server) {
		ServerLevel level = server.getLevel(LEVEL);
		if (level == null && !missingLogged) {
			missingLogged = true;
			HalfCraft.LOG.warn("HalfCraft: no {} level: the rock under Half-Life's map is HalfCraft's own underground", LEVEL.identifier());
		}
		return level != null;
	}

	/**
	 * The block Minecraft's underground has for a mirror world cell, or null while its chunk is being generated (asked
	 * for now: ask again a little later).
	 */
	public static @Nullable BlockState at(MinecraftServer server, int x, int y, int z) {
		ServerLevel level = server.getLevel(LEVEL);
		if (level == null) {
			return null;
		}
		int cx = x >> 4, cz = z >> 4;
		long key = ChunkPos.pack(cx, cz);
		LevelChunk chunk = CHUNKS.getAndMoveToLast(key);
		if (chunk == null) {
			chunk = level.getChunkSource().getChunkNow(cx, cz);
			if (chunk == null) {
				request(server, level, cx, cz, key);
				return null;
			}
			keep(key, chunk);
		}
		int uy = y + SHIFT;
		if (uy < level.getMinY() + 5) {
			return Blocks.DEEPSLATE.defaultBlockState(); // its bedrock and under it: deepslate goes on
		}
		int surface = chunk.getHeight(Heightmap.Types.OCEAN_FLOOR, x & 15, z & 15);
		if (uy > surface - UNDER_SURFACE) {
			return uy < 0 ? Blocks.DEEPSLATE.defaultBlockState() : Blocks.STONE.defaultBlockState();
		}
		return chunk.getBlockState(new BlockPos(x, uy, z));
	}

	private static void request(MinecraftServer server, ServerLevel level, int cx, int cz, long key) {
		if (!WAITING.add(key)) {
			return;
		}
		level.getChunkSource().getChunkFuture(cx, cz, ChunkStatus.FULL, true).thenAcceptAsync(result -> {
			WAITING.remove(key);
			result.ifSuccess(access -> {
				if (access instanceof LevelChunk chunk) {
					keep(key, chunk);
					if (++generated % 50 == 1) {
						HalfCraft.LOG.info("HalfCraft: Minecraft's underground: chunk {} {} ready ({} so far)", cx, cz, generated);
					}
				}
			});
		}, server);
	}

	private static void keep(long key, LevelChunk chunk) {
		CHUNKS.putAndMoveToLast(key, chunk);
		while (CHUNKS.size() > CHUNKS_KEPT) {
			CHUNKS.removeFirst();
		}
	}

	/** The server stopped: no chunk of its is kept. */
	public static void clear() {
		CHUNKS.clear();
		WAITING.clear();
		missingLogged = false;
	}
}
