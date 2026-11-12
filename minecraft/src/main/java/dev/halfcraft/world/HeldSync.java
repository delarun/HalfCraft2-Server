package dev.halfcraft.world;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.link.HostSide;
import io.netty.buffer.ByteBuf;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;

/**
 * What the gravity gun's held slots are, from the HalfCraft server to its players (docs/server/PLAN.md): each player's
 * client.dll draws every held cube with the faces their Minecraft's WorldExporter lists for its slot, and only the
 * server knows them (HeldBlocks). The whole list goes to everyone whenever it changes, and to a player who joins.
 */
public final class HeldSync {
	public static final CustomPacketPayload.Type<Payload> TYPE = new CustomPacketPayload.Type<>(Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "held"));
	private static volatile boolean dirty;
	// the client's: the held slots as its server said last
	private static final Map<Integer, BlockState> CLIENT_BLOCKS = new ConcurrentHashMap<>();
	private static final Map<Integer, ItemStack> CLIENT_ITEMS = new ConcurrentHashMap<>();

	/** One held slot: a block (its state's id, 0 for none) or an item (its id, "" for none). */
	public record Entry(int slot, int blockState, String item) {
	}

	public record Payload(List<Entry> entries) implements CustomPacketPayload {
		public static final StreamCodec<ByteBuf, Payload> CODEC = new StreamCodec<>() {
			@Override
			public Payload decode(ByteBuf buf) {
				int count = Math.min(ByteBufCodecs.VAR_INT.decode(buf), 64);
				List<Entry> entries = new ArrayList<>(count);
				for (int i = 0; i < count; i++) {
					entries.add(new Entry(ByteBufCodecs.VAR_INT.decode(buf), ByteBufCodecs.VAR_INT.decode(buf), ByteBufCodecs.stringUtf8(256).decode(buf)));
				}
				return new Payload(entries);
			}

			@Override
			public void encode(ByteBuf buf, Payload payload) {
				ByteBufCodecs.VAR_INT.encode(buf, payload.entries().size());
				for (Entry entry : payload.entries()) {
					ByteBufCodecs.VAR_INT.encode(buf, entry.slot());
					ByteBufCodecs.VAR_INT.encode(buf, entry.blockState());
					ByteBufCodecs.stringUtf8(256).encode(buf, entry.item());
				}
			}
		};

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	private HeldSync() {
	}

	/** Both sides: the payload's type; the server: sending it. */
	public static void register() {
		PayloadTypeRegistry.clientboundPlay().register(TYPE, Payload.CODEC);
		ServerTickEvents.END_SERVER_TICK.register(server -> {
			if (dirty && HostSide.dedicated() != null) {
				dirty = false;
				Payload payload = current();
				for (ServerPlayer player : server.getPlayerList().getPlayers()) {
					send(player, payload);
				}
			}
		});
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> {
			if (HostSide.dedicated() != null) {
				send(handler.getPlayer(), current());
			}
		});
	}

	/** HeldBlocks changed what a slot holds. Any thread. */
	static void changed() {
		dirty = true;
	}

	private static void send(ServerPlayer player, Payload payload) {
		if (ServerPlayNetworking.canSend(player, TYPE)) {
			ServerPlayNetworking.send(player, payload);
		}
	}

	private static Payload current() {
		List<Entry> entries = new ArrayList<>();
		HeldBlocks.forEachHeld((slot, state) -> entries.add(new Entry(slot, Block.getId(state), "")));
		HeldBlocks.forEachHeldItem((slot, stack) -> entries.add(new Entry(slot, 0, BuiltInRegistries.ITEM.getKey(stack.getItem()).toString())));
		return new Payload(entries);
	}

	/** The client: its server's list came. */
	public static void received(Payload payload) {
		CLIENT_BLOCKS.clear();
		CLIENT_ITEMS.clear();
		for (Entry entry : payload.entries()) {
			if (!entry.item().isEmpty()) {
				Identifier id = Identifier.tryParse(entry.item());
				Item item = id != null ? BuiltInRegistries.ITEM.getValue(id) : Items.AIR;
				if (item != Items.AIR) {
					CLIENT_ITEMS.put(entry.slot(), new ItemStack(item));
				}
			} else {
				CLIENT_BLOCKS.put(entry.slot(), Block.stateById(entry.blockState()));
			}
		}
	}

	/** The client: left its server. */
	public static void clear() {
		CLIENT_BLOCKS.clear();
		CLIENT_ITEMS.clear();
	}

	/** The client, on another machine's server: the held blocks its server listed. */
	public static Map<Integer, BlockState> clientBlocks() {
		return CLIENT_BLOCKS;
	}

	/** The client, on another machine's server: the held items its server listed. */
	public static Map<Integer, ItemStack> clientItems() {
		return CLIENT_ITEMS;
	}
}
