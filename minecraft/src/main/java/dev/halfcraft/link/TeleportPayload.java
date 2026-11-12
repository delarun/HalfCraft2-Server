package dev.halfcraft.link;

import dev.halfcraft.HalfCraft;
import io.netty.buffer.ByteBuf;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerPlayer;

/**
 * Half-Life moved a player of the HalfCraft server (a teleport, a level's arrival, a ladder or a ride): their
 * Minecraft asks the server to put its player there too, as a single player's client tells its integrated server
 * (HostClient.requestTeleport). Without it the server put the player back where it had them, and the client moved
 * them again, over and over.
 */
public record TeleportPayload(double x, double y, double z, float yaw, float pitch) implements CustomPacketPayload {
	public static final Type<TeleportPayload> TYPE = new Type<>(Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "teleport"));
	public static final StreamCodec<ByteBuf, TeleportPayload> CODEC = StreamCodec.composite(ByteBufCodecs.DOUBLE, TeleportPayload::x, ByteBufCodecs.DOUBLE,
		TeleportPayload::y, ByteBufCodecs.DOUBLE, TeleportPayload::z, ByteBufCodecs.FLOAT, TeleportPayload::yaw, ByteBufCodecs.FLOAT, TeleportPayload::pitch,
		TeleportPayload::new);
	// minecraft's world border
	private static final double LIMIT = 3.0E7;
	private static boolean saidBad;

	@Override
	public Type<? extends CustomPacketPayload> type() {
		return TYPE;
	}

	/** Both sides: the payload's type; the server: its handler. */
	public static void register() {
		PayloadTypeRegistry.serverboundPlay().register(TYPE, CODEC);
		ServerPlayNetworking.registerGlobalReceiver(TYPE, (payload, context) -> payload.apply(context.player()));
	}

	/** Whether every coordinate is a number inside the world. */
	public boolean sane() {
		return inside(this.x) && inside(this.y) && inside(this.z) && Float.isFinite(this.yaw) && Float.isFinite(this.pitch);
	}

	private static boolean inside(double v) {
		return Double.isFinite(v) && Math.abs(v) <= LIMIT;
	}

	private void apply(ServerPlayer player) {
		if (!this.sane()) {
			if (!saidBad) {
				saidBad = true;
				HalfCraft.LOG.warn("HalfCraft: {} asked for a teleport to {} {} {}: dropped", player.getName().getString(), this.x, this.y, this.z);
			}
			return;
		}
		// half-life's own: not one of minecraft's jumps for it to take
		HostTeleports.byHost(() -> player.teleportTo(this.x, this.y, this.z));
		player.setYRot(this.yaw);
		player.setXRot(this.pitch);
		player.resetFallDistance();
	}
}
