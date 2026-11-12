package dev.halfcraft.world;

import dev.halfcraft.link.HostSide;
import dev.halfcraft.link.Proto;
import dev.halfcraft.link.ServerLink;
import dev.halfcraft.mobs.HostNav;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerChunkEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;

/**
 * The HalfCraft server's solid blocks for its Source server (docs/server/PLAN.md): server.so makes each section with
 * any into collision (BlockSolids), so NPCs, props and bullets stop at what players built, as a single player's
 * client.dll hands its server.dll the solids its Minecraft's WorldExporter found. A section of the mirror world goes
 * as a kRenSolids into the server link's render ring when it loads with blocks or a block in it changes: its solid
 * bits, then the ones the gravity gun may tear out (HeldBlocks.takeable). What was sent is kept, so a new source
 * session (srcds restarted) gets it all again, unloaded chunks too.
 */
public final class ServerSolids {
	private static final int BITS_BYTES = 512;
	private static final Set<Long> PENDING = new LinkedHashSet<>(); // sections to look at: SectionPos.asLong
	private static final Map<Long, byte[]> SENT = new HashMap<>(); // sections with solids, as last sent
	private static long session;

	private ServerSolids() {
	}

	public static void init() {
		ServerChunkEvents.CHUNK_LOAD.register((level, chunk, generated) -> {
			if (level.getServer().isDedicatedServer() && HostNav.inMirror(level)) {
				LevelChunkSection[] sections = chunk.getSections();
				for (int i = 0; i < sections.length; i++) {
					if (!sections[i].hasOnlyAir()) {
						PENDING.add(SectionPos.asLong(chunk.getPos().x(), level.getSectionYFromSectionIndex(i), chunk.getPos().z()));
					}
				}
			}
		});
		ServerTickEvents.END_SERVER_TICK.register(ServerSolids::tick);
	}

	/** A block of a chunk changed (LevelChunkMixin): its section goes to server.so again. */
	public static void afterChange(LevelChunk chunk, BlockPos pos) {
		if (chunk.getLevel() instanceof ServerLevel level && level.getServer().isDedicatedServer() && level.getServer().isSameThread() && HostNav.inMirror(level)) {
			PENDING.add(SectionPos.asLong(chunk.getPos().x(), SectionPos.blockToSectionCoord(pos.getY()), chunk.getPos().z()));
		}
	}

	private static void tick(MinecraftServer server) {
		ServerLink link = HostSide.dedicated();
		if (link == null || !link.active()) {
			return;
		}
		if (link.session() != session) {
			// srcds started over: it has no block yet
			session = link.session();
			for (Map.Entry<Long, byte[]> entry : SENT.entrySet()) {
				if (!PENDING.contains(entry.getKey()) && !write(link, entry.getKey(), entry.getValue())) {
					PENDING.add(entry.getKey()); // the ring is full: looked at again
				}
			}
		}
		ServerLevel level = server.overworld();
		for (var it = PENDING.iterator(); it.hasNext();) {
			long key = it.next();
			LevelChunk chunk = level.getChunkSource().getChunkNow(SectionPos.x(key), SectionPos.z(key));
			if (chunk == null) {
				it.remove(); // unloaded before it went: it goes when it loads again
				continue;
			}
			byte[] bits = solids(level, chunk, SectionPos.y(key));
			if (bits == null && !SENT.containsKey(key)) {
				it.remove(); // nothing there, nothing to take back
				continue;
			}
			if (bits != null && java.util.Arrays.equals(bits, SENT.get(key))) {
				it.remove();
				continue;
			}
			if (!write(link, key, bits)) {
				return; // the ring is full: the rest go next tick
			}
			if (bits != null) {
				SENT.put(key, bits);
			} else {
				SENT.remove(key);
			}
			it.remove();
		}
	}

	/** The section's solid bits and takeable bits, or null for none solid. */
	private static byte[] solids(ServerLevel level, LevelChunk chunk, int sy) {
		int index = level.getSectionIndexFromSectionY(sy);
		if (index < 0 || index >= chunk.getSections().length || chunk.getSections()[index].hasOnlyAir()) {
			return null;
		}
		byte[] bits = new byte[2 * BITS_BYTES];
		boolean any = false;
		BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
		int ox = chunk.getPos().getMinBlockX(), oy = SectionPos.sectionToBlockCoord(sy), oz = chunk.getPos().getMinBlockZ();
		for (int y = 0; y < 16; y++) {
			for (int z = 0; z < 16; z++) {
				for (int x = 0; x < 16; x++) {
					pos.set(ox + x, oy + y, oz + z);
					BlockState state = chunk.getBlockState(pos);
					// as WorldExporter: anything with a collision shape stops Half-Life's NPCs
					if (state.isAir() || state.getCollisionShape(level, pos).isEmpty()) {
						continue;
					}
					int bit = x + 16 * z + 256 * y;
					bits[bit >> 3] |= (byte) (1 << (bit & 7));
					if (HeldBlocks.takeable(state, level, pos)) {
						bits[BITS_BYTES + (bit >> 3)] |= (byte) (1 << (bit & 7));
					}
					any = true;
				}
			}
		}
		return any ? bits : null;
	}

	/** One section's kRenSolids: its solid and takeable bits, or none (bits null). */
	private static boolean write(ServerLink link, long key, byte[] bits) {
		int count = 0;
		if (bits != null) {
			for (int i = 0; i < BITS_BYTES; i++) {
				count += Integer.bitCount(bits[i] & 0xFF);
			}
		}
		ByteBuffer header = ByteBuffer.allocate(Proto.REN_SOLIDS_BYTES).order(ByteOrder.LITTLE_ENDIAN).putInt(SectionPos.x(key)).putInt(SectionPos.y(key))
			.putInt(SectionPos.z(key)).putInt(count).flip();
		return link.writeRender(Proto.REN_SOLIDS, header, bits != null ? ByteBuffer.wrap(bits) : ByteBuffer.allocate(0));
	}
}
