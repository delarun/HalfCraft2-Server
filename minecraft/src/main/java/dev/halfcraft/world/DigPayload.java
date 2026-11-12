package dev.halfcraft.world;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.mobs.HostNav;
import io.netty.buffer.ByteBuf;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.core.BlockPos;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerPlayer;

/**
 * A player of the HalfCraft server mined through a cell of Half-Life's wall: their Minecraft asks the server to
 * open it (HostDig.open, which checks the reach, the right to build, the chunk and the cell itself), as a single
 * player's client has its integrated server open it (HostDigClient). The dug cell syncs to every player.
 */
public record DigPayload(BlockPos pos, int material) implements CustomPacketPayload {
	public static final Type<DigPayload> TYPE = new Type<>(Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "dig_open"));
	public static final StreamCodec<ByteBuf, DigPayload> CODEC = StreamCodec.composite(BlockPos.STREAM_CODEC, DigPayload::pos, ByteBufCodecs.VAR_INT,
		DigPayload::material, DigPayload::new);

	@Override
	public Type<? extends CustomPacketPayload> type() {
		return TYPE;
	}

	/** Both sides: the payload's type; the server: its handler. */
	public static void register() {
		PayloadTypeRegistry.serverboundPlay().register(TYPE, CODEC);
		ServerPlayNetworking.registerGlobalReceiver(TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			if (HostNav.inMirror(player.level())) {
				HostDig.open(player.level(), player, payload.pos(), payload.material());
			}
		});
	}
}
