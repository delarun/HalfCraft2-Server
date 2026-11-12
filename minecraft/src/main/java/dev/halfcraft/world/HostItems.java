package dev.halfcraft.world;

import static dev.halfcraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.halfcraft.HostPlayers;
import dev.halfcraft.link.HostSide;
import dev.halfcraft.mobs.HostNav;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.VarHandle;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.item.ItemEntity;

/**
 * Minecraft's dropped items near the player(s), for Half-Life's gravity gun (ItemTable in the protocol): server.dll
 * keeps a stand-in for each that the gun can grab, and Proto.IN_TAKE_ITEM takes the item into a held slot
 * (HeldBlocks). The host's player's in a single player's game, every player's on the HalfCraft server.
 */
public final class HostItems {
	private static final VarHandle INT = JAVA_INT.varHandle();
	// blocks around a player an item is listed in
	private static final double RANGE = 24.0;
	private static boolean written;

	private HostItems() {
	}

	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(HostItems::tick);
	}

	private static void tick(MinecraftServer server) {
		List<ServerPlayer> players = new ArrayList<>();
		if (HostSide.active()) {
			if (HostSide.dedicated() != null) {
				server.getPlayerList().getPlayers().stream().filter(p -> HostNav.inMirror(p.level()) && !p.isSpectator()).forEach(players::add);
			} else if (HostPlayers.host(server) instanceof ServerPlayer host && HostNav.inMirror(host.level())) {
				players.add(host);
			}
		}
		if (players.isEmpty() && !written) {
			return;
		}
		Set<ItemEntity> found = new HashSet<>();
		for (ServerPlayer player : players) {
			found.addAll(player.level().getEntitiesOfClass(ItemEntity.class, player.getBoundingBox().inflate(RANGE), item -> item.isAlive() && !item.getItem().isEmpty()));
		}
		List<ItemEntity> items = new ArrayList<>(found);
		items.sort(Comparator.comparingDouble(item -> players.stream().mapToDouble(item::distanceToSqr).min().orElse(0.0)));
		write(items);
		written = !items.isEmpty();
	}

	/** Seqlock write of the item table. */
	private static void write(List<ItemEntity> items) {
		MemorySegment s = HostSide.segment();
		if (s == null) {
			return;
		}
		long b = OFF_ITEM_TABLE;
		int seq = s.get(JAVA_INT, b + IT_SEQ);
		INT.setRelease(s, b + IT_SEQ, seq + 1);
		VarHandle.storeStoreFence();
		int count = Math.min(items.size(), MAX_ITEMS);
		s.set(JAVA_INT, b + IT_COUNT, count);
		for (int i = 0; i < count; i++) {
			ItemEntity item = items.get(i);
			long r = b + IT_RECORDS + (long) i * IT_RECORD_BYTES;
			s.set(JAVA_INT, r + IT_R_ID, item.getId());
			s.set(JAVA_INT, r + IT_R_COUNT, item.getItem().getCount());
			s.set(JAVA_FLOAT, r + IT_R_X, (float) item.getX());
			s.set(JAVA_FLOAT, r + IT_R_Y, (float) item.getY());
			s.set(JAVA_FLOAT, r + IT_R_Z, (float) item.getZ());
		}
		INT.setRelease(s, b + IT_SEQ, seq + 2);
	}
}
