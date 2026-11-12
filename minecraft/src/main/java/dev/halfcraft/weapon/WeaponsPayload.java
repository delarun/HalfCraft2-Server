package dev.halfcraft.weapon;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.link.Proto;
import dev.halfcraft.link.ProtoStructs;
import dev.halfcraft.link.WeaponTable;
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
import net.minecraft.server.level.ServerPlayer;

/**
 * A player of the HalfCraft server tells it their Half-Life weapons (docs/server/PLAN.md): their client.dll writes the
 * weapon table their Source server's server.dll writes in a single player's game, and their Minecraft sends what the
 * server's HostWeapons needs of it, the weapons and the one out. Ammo stays with the client, which draws it. Only
 * stand-in items come of it: a weapon the Source server doesn't give the player can't come out there.
 */
public record WeaponsPayload(boolean live, int active, List<Integer> weapons) implements CustomPacketPayload {
	public static final Type<WeaponsPayload> TYPE = new Type<>(Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "weapons"));
	public static final StreamCodec<ByteBuf, WeaponsPayload> CODEC = StreamCodec.composite(ByteBufCodecs.BOOL, WeaponsPayload::live, ByteBufCodecs.VAR_INT,
		WeaponsPayload::active, ByteBufCodecs.VAR_INT.apply(ByteBufCodecs.list(Proto.WT_MAX_WEAPONS)), WeaponsPayload::weapons, WeaponsPayload::new);
	private static final Map<UUID, WeaponTable> TABLES = new HashMap<>();

	@Override
	public Type<? extends CustomPacketPayload> type() {
		return TYPE;
	}

	/** Both sides: the payload's type; the server: its handler. */
	public static void register() {
		PayloadTypeRegistry.serverboundPlay().register(TYPE, CODEC);
		ServerPlayNetworking.registerGlobalReceiver(TYPE, (payload, context) -> {
			TABLES.put(context.player().getUUID(), payload.table());
			HalfCraft.LOG.info("HalfCraft: {} has {} of Half-Life's weapons ({} out)", context.player().getName().getString(), payload.weapons().size(),
				payload.active());
		});
		ServerPlayConnectionEvents.DISCONNECT.register((handler, server) -> TABLES.remove(handler.getPlayer().getUUID()));
	}

	/** The client's side: what it sends of its own table. */
	public static WeaponsPayload of(WeaponTable table) {
		return new WeaponsPayload(table.live(), table.active(), table.weapons().stream().map(WeaponTable.Weapon::id).toList());
	}

	/** A player's table as their Minecraft said last; NONE before it did. Server thread. */
	static WeaponTable tableOf(ServerPlayer player) {
		return TABLES.getOrDefault(player.getUUID(), WeaponTable.NONE);
	}

	private WeaponTable table() {
		List<WeaponTable.Weapon> records = new ArrayList<>();
		for (int id : this.weapons) {
			records.add(new ProtoStructs.WeaponRecord(id, -1, -1, -1, -1, -1, -1, 0));
		}
		return new WeaponTable(this.live, this.active, List.copyOf(records));
	}
}
