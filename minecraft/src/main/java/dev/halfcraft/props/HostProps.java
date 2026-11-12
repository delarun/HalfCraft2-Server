package dev.halfcraft.props;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.HostPlayers;
import dev.halfcraft.link.HostSide;
import dev.halfcraft.link.Proto;
import java.util.HashMap;
import java.util.Map;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import org.jspecify.annotations.Nullable;

/**
 * The Half-Life props Minecraft crafts (Proto.PROP_*, server.dll's core/hc_props.h has their models): one item each,
 * crafted from what the prop is made of (data/halfcraft/recipe), that puts its prop into Half-Life's world
 * ({@link PropItem}). Minecraft's hits break any of Half-Life's objects but its static ones down into an item
 * (Proto.STR_PROP_DROPPED), into the inventory of the player who struck: a crafted kind's, or {@code halfcraft:hl_prop}
 * carrying the object's model and kind ({@link PropData}), which puts that object back.
 */
public final class HostProps {
	/** Ticks a player waits between two props they put down: server.dll spawns one a click. */
	static final int PLACE_COOLDOWN_TICKS = 10;
	private static final Map<Integer, PropItem> ITEMS = new HashMap<>();
	/** The model and kind of object a Half-Life object's item puts back. */
	public static final net.minecraft.core.component.DataComponentType<PropData> PROP_DATA = net.minecraft.core.component.DataComponentType.<PropData>builder()
		.persistent(PropData.CODEC).networkSynchronized(PropData.STREAM_CODEC).build();
	private static PropItem hlProp;

	private HostProps() {
	}

	public static void init() {
		register(Proto.PROP_WOOD_CRATE, "prop_wood_crate");
		register(Proto.PROP_OIL_DRUM, "prop_oil_drum");
		register(Proto.PROP_EXPLOSIVE_DRUM, "prop_explosive_drum");
		register(Proto.PROP_CINDER_BLOCK, "prop_cinder_block");
		register(Proto.PROP_CHAIR, "prop_chair");
		register(Proto.PROP_TABLE, "prop_table");
		register(Proto.PROP_TIRE, "prop_tire");
		register(Proto.PROP_RADIATOR, "prop_radiator");
		register(Proto.PROP_BARRIER, "prop_barrier");
		Registry.register(BuiltInRegistries.DATA_COMPONENT_TYPE, Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "prop"), PROP_DATA);
		// any other object of Half-Life's: its model rides in the item (PROP_DATA)
		ResourceKey<Item> key = ResourceKey.create(Registries.ITEM, Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "hl_prop"));
		hlProp = Registry.register(BuiltInRegistries.ITEM, key, new PropItem(new Item.Properties().setId(key).stacksTo(16), Proto.PROP_NONE));
	}

	private static void register(int kind, String path) {
		ResourceKey<Item> key = ResourceKey.create(Registries.ITEM, Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, path));
		ITEMS.put(kind, Registry.register(BuiltInRegistries.ITEM, key, new PropItem(new Item.Properties().setId(key).stacksTo(16), kind)));
	}

	/** A single player's (HostStrings, Proto.STR_PROP_DROPPED): an object broke down into its item; the host's player gets it. Any thread. */
	public static void dropped(@Nullable MinecraftServer server, String text) {
		if (server != null) {
			server.execute(() -> {
				ServerPlayer host = HostPlayers.host(server);
				if (host != null) {
					give(host, text);
				}
			});
		}
	}

	/** The halfcraft server's (ServerPlayers): an object broke down into its item, for the player who struck. Server thread. */
	public static void droppedOnServer(@Nullable ServerPlayer player, String text) {
		if (player != null && HostSide.dedicated() != null) {
			give(player, text);
		}
	}

	/** "<kind> <class> <x> <y> <z> <model>": its item into the player's inventory, or where it was when that's full. */
	private static void give(ServerPlayer player, String text) {
		String[] parts = text.split(" ", 6);
		if (parts.length < 6) {
			return;
		}
		int kind, propClass;
		double x, y, z;
		try {
			kind = Integer.parseInt(parts[0]);
			propClass = Integer.parseInt(parts[1]);
			x = Double.parseDouble(parts[2]);
			y = Double.parseDouble(parts[3]);
			z = Double.parseDouble(parts[4]);
		} catch (NumberFormatException e) {
			return;
		}
		ItemStack stack;
		if (ITEMS.containsKey(kind)) {
			stack = new ItemStack(ITEMS.get(kind));
		} else {
			stack = new ItemStack(hlProp);
			stack.set(PROP_DATA, new PropData(parts[5], propClass));
		}
		String what = stack.getHoverName().getString();
		if (!player.getInventory().add(stack) && !stack.isEmpty() && Double.isFinite(x + y + z)) {
			ServerLevel level = player.level();
			ItemEntity entity = new ItemEntity(level, x, y, z, stack);
			entity.setDefaultPickUpDelay();
			level.addFreshEntity(entity);
		}
		HalfCraft.LOG.info("HalfCraft: {} broke {} down into {}", player.getName().getString(), parts[5], what);
	}
}
