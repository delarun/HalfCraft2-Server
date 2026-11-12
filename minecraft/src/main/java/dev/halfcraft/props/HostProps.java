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
 * ({@link PropItem}), and that drops where Minecraft's hits broke such a prop down (Proto.IN_PROP_DROPPED), the map's
 * own props of those models too.
 */
public final class HostProps {
	/** Ticks a player waits between two props they put down: server.dll spawns one a click. */
	static final int PLACE_COOLDOWN_TICKS = 10;
	private static final Map<Integer, PropItem> ITEMS = new HashMap<>();

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
	}

	private static void register(int kind, String path) {
		ResourceKey<Item> key = ResourceKey.create(Registries.ITEM, Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, path));
		ITEMS.put(kind, Registry.register(BuiltInRegistries.ITEM, key, new PropItem(new Item.Properties().setId(key).stacksTo(16), kind)));
	}

	/** A single player's (InputBridge): a prop broke down into its item at (x, y, z). Any thread. */
	public static void dropped(@Nullable MinecraftServer server, int kind, double x, double y, double z) {
		if (server != null) {
			server.execute(() -> {
				ServerPlayer host = HostPlayers.host(server);
				if (host != null) {
					drop(host.level(), kind, x, y, z);
				}
			});
		}
	}

	/** The halfcraft server's (ServerPlayers): a prop broke down into its item at (x, y, z). Server thread. */
	public static void droppedOnServer(MinecraftServer server, int kind, double x, double y, double z) {
		if (HostSide.dedicated() != null) {
			drop(server.overworld(), kind, x, y, z);
		}
	}

	private static void drop(ServerLevel level, int kind, double x, double y, double z) {
		PropItem item = ITEMS.get(kind);
		if (item == null || !Double.isFinite(x) || !Double.isFinite(y) || !Double.isFinite(z) || !level.isLoaded(net.minecraft.core.BlockPos.containing(x, y, z))) {
			return;
		}
		ItemEntity entity = new ItemEntity(level, x, y, z, new ItemStack(item));
		entity.setDefaultPickUpDelay();
		level.addFreshEntity(entity);
		HalfCraft.LOG.info("HalfCraft: a Half-Life prop broke down into {} at {} {} {}", BuiltInRegistries.ITEM.getKey(item), Math.round(x), Math.round(y), Math.round(z));
	}
}
