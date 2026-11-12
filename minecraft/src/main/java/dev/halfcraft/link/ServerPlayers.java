package dev.halfcraft.link;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.combat.HostCombat;
import io.netty.buffer.ByteBuf;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * The HalfCraft server's players as its Source server knows them (docs/server/PLAN.md, S2): each player's Minecraft
 * tells the server their Source userid (halfcraft:source_player, from HostState), so a hurt or a heal server.so sends
 * for a userid lands on that player, their death tells server.so which Source player died, and their health goes back
 * to server.so (PlayerVitals) for their Source player.
 */
public final class ServerPlayers {
	private static final Map<Integer, UUID> BY_USER_ID = new HashMap<>();
	private static final Map<UUID, Integer> USER_IDS = new HashMap<>();
	// the input ring's state: the player the next hurt, hurt-from or heal is for, and where the next hurt came from
	private static int forUserId;
	private static @Nullable Vec3 hurtFrom;

	/** A player's Minecraft names their Source player. */
	public record SourcePlayerPayload(int userId) implements CustomPacketPayload {
		public static final Type<SourcePlayerPayload> TYPE = new Type<>(Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "source_player"));
		public static final StreamCodec<ByteBuf, SourcePlayerPayload> CODEC = StreamCodec.composite(ByteBufCodecs.VAR_INT, SourcePlayerPayload::userId,
			SourcePlayerPayload::new);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	private ServerPlayers() {
	}

	/** Both sides: the payload; the server: its handler and the players leaving. */
	public static void register() {
		PayloadTypeRegistry.serverboundPlay().register(SourcePlayerPayload.TYPE, SourcePlayerPayload.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(SourcePlayerPayload.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			forget(player.getUUID());
			if (payload.userId() > 0) {
				BY_USER_ID.put(payload.userId(), player.getUUID());
				USER_IDS.put(player.getUUID(), payload.userId());
				HalfCraft.LOG.info("HalfCraft: {} is source player {}", player.getName().getString(), payload.userId());
			}
		});
		ServerPlayConnectionEvents.DISCONNECT.register((handler, server) -> forget(handler.getPlayer().getUUID()));
	}

	private static void forget(UUID player) {
		Integer had = USER_IDS.remove(player);
		if (had != null) {
			BY_USER_ID.remove(had);
		}
	}

	/** A player's Source userid, 0 for none (not told yet, or single player). */
	public static int userId(ServerPlayer player) {
		return USER_IDS.getOrDefault(player.getUUID(), 0);
	}

	/** The server, each tick: server.so's hurts and heals, the world's events (blasts, bullets, mobs hurt, the gravity gun's blocks), and the players' health for it. */
	public static void tick(MinecraftServer server, ServerLink link) {
		link.drainInput((type, code, a, b, c) -> {
			switch (type) {
				case Proto.IN_FOR -> {
					forUserId = a;
					hurtFrom = null;
				}
				case Proto.IN_HURT_FROM -> hurtFrom = new Vec3(Float.intBitsToFloat(a), Float.intBitsToFloat(b), Float.intBitsToFloat(c));
				case Proto.IN_HURT -> {
					ServerPlayer player = target(server);
					if (player != null) {
						HostCombat.hurtPlayer(player, code, a / 100.0F, b, c, hurtFrom);
					}
					hurtFrom = null;
				}
				case Proto.IN_HEAL -> {
					ServerPlayer player = target(server);
					if (player != null) {
						HostCombat.healPlayer(player, code, a / 100.0F);
					}
				}
				// the world's: as a single player's InputBridge hands them to its integrated server
				case Proto.IN_HURT_MOB -> dev.halfcraft.mobs.HostMobs.hurtOnServer(server, code, a, b / 100.0F, c, target(server));
				case Proto.IN_BLAST -> dev.halfcraft.world.HostBlockDamage.blast(server, dev.halfcraft.combat.HostHurts.coordinate(a),
					dev.halfcraft.combat.HostHurts.coordinate(b), dev.halfcraft.combat.HostHurts.coordinate(c), code / 100.0F);
				case Proto.IN_BULLET_HIT -> dev.halfcraft.world.HostBlockDamage.bulletHit(server, new net.minecraft.core.BlockPos(a, b, c));
				case Proto.IN_TAKE_BLOCK -> dev.halfcraft.world.HeldBlocks.take(server, code, new net.minecraft.core.BlockPos(a, b, c));
				case Proto.IN_HELD_BLOCK_LANDED -> dev.halfcraft.world.HeldBlocks.landed(server, code, new net.minecraft.core.BlockPos(a, b, c));
				case Proto.IN_PROP_DROPPED -> dev.halfcraft.props.HostProps.droppedOnServer(server, code, dev.halfcraft.combat.HostHurts.coordinate(a),
					dev.halfcraft.combat.HostHurts.coordinate(b), dev.halfcraft.combat.HostHurts.coordinate(c));
				case Proto.IN_HELD_BLOCK_LOST -> dev.halfcraft.world.HeldBlocks.lost(server, code, new net.minecraft.core.BlockPos(a, b, c));
				default -> {
				}
			}
		});
		List<ServerLink.Vitals> vitals = new ArrayList<>();
		for (ServerPlayer player : server.getPlayerList().getPlayers()) {
			int userId = userId(player);
			if (userId > 0) {
				int flags = Proto.PV_IN_WORLD | (player.isDeadOrDying() ? Proto.PV_DEAD : 0);
				vitals.add(new ServerLink.Vitals(userId, flags, player.getHealth(), player.getMaxHealth(), player.getAbsorptionAmount()));
			}
		}
		link.writePlayerVitals(vitals);
	}

	private static @Nullable ServerPlayer target(MinecraftServer server) {
		UUID id = BY_USER_ID.get(forUserId);
		return id != null ? server.getPlayerList().getPlayer(id) : null;
	}
}
