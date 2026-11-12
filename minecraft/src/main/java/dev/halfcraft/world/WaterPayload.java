package dev.halfcraft.world;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.link.HostSide;
import io.netty.buffer.ByteBuf;
import java.util.Arrays;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerPlayer;

/**
 * A player of the HalfCraft server sends it Half-Life's water around them (docs/server/PLAN.md): the grid of water
 * surfaces over the block columns around the player that their client.dll finds (WaterGrid in the protocol), whenever
 * it changes. The server has no Half-Life of its own to ask, and without it held the player's air full while their
 * Minecraft, swimming, used it up (HostWater.playerGrid).
 */
public record WaterPayload(int originX, int originZ, int size, float[] surface) implements CustomPacketPayload {
	public static final Type<WaterPayload> TYPE = new Type<>(Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "water"));
	// a grid's side at most (blocks): client.dll's is Proto.WATER_GRID_SIZE
	private static final int MAX_SIZE = 64;
	public static final StreamCodec<ByteBuf, WaterPayload> CODEC = new StreamCodec<>() {
		@Override
		public WaterPayload decode(ByteBuf buf) {
			int originX = ByteBufCodecs.VAR_INT.decode(buf);
			int originZ = ByteBufCodecs.VAR_INT.decode(buf);
			int size = Math.clamp(ByteBufCodecs.VAR_INT.decode(buf), 0, MAX_SIZE);
			float[] surface = new float[size * size];
			for (int i = 0; i < surface.length; i++) {
				surface[i] = buf.readFloat();
			}
			return new WaterPayload(originX, originZ, size, surface);
		}

		@Override
		public void encode(ByteBuf buf, WaterPayload payload) {
			ByteBufCodecs.VAR_INT.encode(buf, payload.originX());
			ByteBufCodecs.VAR_INT.encode(buf, payload.originZ());
			ByteBufCodecs.VAR_INT.encode(buf, payload.size());
			for (float height : payload.surface()) {
				buf.writeFloat(height);
			}
		}
	};

	@Override
	public Type<? extends CustomPacketPayload> type() {
		return TYPE;
	}

	/** Both sides: the payload's type; the server: its handler (the halfcraft server's only). */
	public static void register() {
		PayloadTypeRegistry.serverboundPlay().register(TYPE, CODEC);
		ServerPlayNetworking.registerGlobalReceiver(TYPE, (payload, context) -> payload.apply(context.player()));
		ServerPlayConnectionEvents.DISCONNECT.register((handler, server) -> HostWater.playerGrid(handler.getPlayer().getUUID(), null));
	}

	/** The client's: its grid as a payload. */
	public static WaterPayload of(WaterColumns.Grid grid) {
		return new WaterPayload(grid.originX(), grid.originZ(), grid.size(), grid.surface());
	}

	/** Whether it says what the grid does. */
	public boolean same(WaterColumns.Grid grid) {
		return grid.originX() == this.originX && grid.originZ() == this.originZ && grid.size() == this.size && Arrays.equals(grid.surface(), this.surface);
	}

	private void apply(ServerPlayer player) {
		if (HostSide.dedicated() == null || this.surface.length != this.size * this.size) {
			return; // a single player's server has its Half-Life's own grid
		}
		HostWater.playerGrid(player.getUUID(), new WaterColumns.Grid(this.originX, this.originZ, this.size, this.surface.clone(), Double.NEGATIVE_INFINITY));
	}
}
