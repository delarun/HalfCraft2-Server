package dev.halfcraft.world;

import static dev.halfcraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.halfcraft.HalfCraft;
import io.netty.buffer.ByteBuf;
import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;

/**
 * Half-Life's collision relayed to each player's Minecraft on the HalfCraft server (docs/server/PLAN.md). The server
 * reads the Source server's collision ring (HostCollision, from a ServerLink) and this keeps the last message of each
 * kind for each region; every tick each player is sent the regions around them that changed since they last got
 * them, nearest first, in {@link Payload}s. A clear sends a clear and everything again. The client applies them as
 * its own ring's messages (HostCollision.applyRelayed), so its movement predicts against the same triangles.
 */
public final class CollisionRelay {
	public static final CustomPacketPayload.Type<Payload> TYPE = new CustomPacketPayload.Type<>(Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "collision"));
	// regions around a player, as the source side streams them (CollisionStreamer::RADIUS, and its rows below and above)
	private static final int RADIUS = 8;
	private static final int BELOW = 6;
	private static final int ABOVE = 4;
	// a tick's budget for one player: a payload stays well under the protocol's 8 MiB large payload limit
	private static final int MAX_BYTES_PER_TICK = 2 << 20;
	private static final int MAX_PAYLOAD_BYTES = 8 << 20;

	/** One collision message: its ColType and its payload, as the ring had them. */
	public record Message(int type, byte[] bytes) {
	}

	/** Messages to a player's Minecraft. */
	public record Payload(List<Message> messages) implements CustomPacketPayload {
		public static final StreamCodec<ByteBuf, Payload> CODEC = new StreamCodec<>() {
			@Override
			public Payload decode(ByteBuf buf) {
				int count = ByteBufCodecs.VAR_INT.decode(buf);
				List<Message> messages = new ArrayList<>(count);
				for (int i = 0; i < count; i++) {
					int type = ByteBufCodecs.VAR_INT.decode(buf);
					messages.add(new Message(type, ByteBufCodecs.byteArray(MAX_PAYLOAD_BYTES).decode(buf)));
				}
				return new Payload(messages);
			}

			@Override
			public void encode(ByteBuf buf, Payload payload) {
				ByteBufCodecs.VAR_INT.encode(buf, payload.messages().size());
				for (Message message : payload.messages()) {
					ByteBufCodecs.VAR_INT.encode(buf, message.type());
					ByteBufCodecs.byteArray(MAX_PAYLOAD_BYTES).encode(buf, message.bytes());
				}
			}
		};

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** A region's last messages, and the version they came with. */
	private static final class Region {
		byte @org.jspecify.annotations.Nullable [] shapes;
		byte @org.jspecify.annotations.Nullable [] tris;
		byte @org.jspecify.annotations.Nullable [] voids;
		long version;
	}

	/** What a player was sent: the clear they had last, and each region's version. */
	private static final class Sent {
		long clears = -1;
		final Map<Long, Long> versions = new HashMap<>();
	}

	private static final Map<Long, Region> REGIONS = new ConcurrentHashMap<>();
	private static final Map<UUID, Sent> SENT = new HashMap<>();
	private static volatile long versions;
	private static volatile long clears;
	private static volatile int epoch;
	private static volatile boolean recording;

	private CollisionRelay() {
	}

	/** Both sides: the payload's type. */
	public static void register() {
		PayloadTypeRegistry.clientboundPlay().registerLarge(TYPE, Payload.CODEC, MAX_PAYLOAD_BYTES);
	}

	/** The server: record what the collision ring brings from now on. */
	public static void startRecording() {
		recording = true;
	}

	/** The collision thread, for each message of the ring it read (the server's only). */
	static void record(int type, MemorySegment s, long payload, int bytes) {
		if (!recording) {
			return;
		}
		byte[] copy = s.asSlice(payload, bytes).toArray(JAVA_BYTE);
		if (type == COL_CLEAR) {
			REGIONS.clear();
			epoch = s.get(JAVA_INT, payload);
			clears++;
			return;
		}
		long key = regionKey(s.get(JAVA_INT, payload + COL_REGION_MIN_X), s.get(JAVA_INT, payload + COL_REGION_MIN_Y), s.get(JAVA_INT, payload + COL_REGION_MIN_Z));
		Region region = REGIONS.computeIfAbsent(key, k -> new Region());
		synchronized (region) {
			switch (type) {
				case COL_REGION -> region.shapes = copy;
				case COL_TRIS -> region.tris = copy;
				case COL_VOID -> region.voids = copy;
				default -> {
					return;
				}
			}
			region.version = ++versions;
		}
	}

	private static long regionKey(int minX, int minY, int minZ) {
		return net.minecraft.core.BlockPos.asLong(Math.floorDiv(minX, HostCollision.REGION_SIZE), Math.floorDiv(minY, HostCollision.REGION_SIZE),
			Math.floorDiv(minZ, HostCollision.REGION_SIZE));
	}

	/** The server, every tick: each player gets the changed regions around them, nearest first. */
	public static void tick(MinecraftServer server) {
		if (!recording) {
			return;
		}
		SENT.keySet().retainAll(server.getPlayerList().getPlayers().stream().map(ServerPlayer::getUUID).toList());
		for (ServerPlayer player : server.getPlayerList().getPlayers()) {
			if (ServerPlayNetworking.canSend(player, TYPE)) {
				send(player, SENT.computeIfAbsent(player.getUUID(), id -> new Sent()));
			}
		}
	}

	private static void send(ServerPlayer player, Sent sent) {
		List<Message> messages = new ArrayList<>();
		if (sent.clears != clears) {
			sent.clears = clears;
			sent.versions.clear();
			byte[] clear = new byte[4];
			int e = epoch;
			clear[0] = (byte) e;
			clear[1] = (byte) (e >>> 8);
			clear[2] = (byte) (e >>> 16);
			clear[3] = (byte) (e >>> 24);
			messages.add(new Message(COL_CLEAR, clear));
		}
		int prx = Math.floorDiv(player.getBlockX(), HostCollision.REGION_SIZE);
		int pry = Math.floorDiv(player.getBlockY(), HostCollision.REGION_SIZE);
		int prz = Math.floorDiv(player.getBlockZ(), HostCollision.REGION_SIZE);
		List<long[]> due = new ArrayList<>(); // key, version, distance
		for (Map.Entry<Long, Region> entry : REGIONS.entrySet()) {
			long key = entry.getKey();
			int dx = net.minecraft.core.BlockPos.getX(key) - prx;
			int dy = net.minecraft.core.BlockPos.getY(key) - pry;
			int dz = net.minecraft.core.BlockPos.getZ(key) - prz;
			if (Math.abs(dx) > RADIUS || Math.abs(dz) > RADIUS || dy < -BELOW || dy > ABOVE) {
				continue;
			}
			long version = entry.getValue().version;
			Long had = sent.versions.get(key);
			if (had == null || had < version) {
				due.add(new long[] { key, version, (long) dx * dx + (long) dy * dy + (long) dz * dz });
			}
		}
		due.sort((a, b) -> Long.compare(a[2], b[2]));
		int bytes = 0;
		for (long[] d : due) {
			if (bytes >= MAX_BYTES_PER_TICK) {
				break;
			}
			Region region = REGIONS.get(d[0]);
			if (region == null) {
				continue;
			}
			synchronized (region) {
				// voids first: the shapes' cells and the triangles go where the void is known
				for (int type : new int[] { COL_VOID, COL_REGION, COL_TRIS }) {
					byte[] message = type == COL_VOID ? region.voids : type == COL_REGION ? region.shapes : region.tris;
					if (message != null) {
						messages.add(new Message(type, message));
						bytes += message.length;
					}
				}
				sent.versions.put(d[0], region.version);
			}
		}
		if (!messages.isEmpty()) {
			ServerPlayNetworking.send(player, new Payload(messages));
		}
	}
}
