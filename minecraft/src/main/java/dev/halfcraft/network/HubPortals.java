package dev.halfcraft.network;

import com.google.gson.Gson;
import dev.halfcraft.HalfCraft;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.network.chat.Component;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.DyeColor;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.StandingSignBlock;
import net.minecraft.world.level.block.WallSignBlock;
import net.minecraft.world.level.block.entity.SignBlockEntity;
import net.minecraft.world.level.block.entity.SignText;
import net.minecraft.world.level.block.entity.SignTextSlot;
import net.minecraft.world.level.block.state.BlockState;
import org.jspecify.annotations.Nullable;

/**
 * The hub's portals (Network): a platform of Minecraft blocks where the first player came into the hub, a row of
 * portals for each of Half-Life 2's chapters, one portal for each of its maps, named on a sign over it. Walking into a
 * portal's opening sends the player to that map's pair. Built once; where it is is kept in the network's folder
 * (hub.json), so the server finds its portals again after a restart.
 */
public final class HubPortals {
	/** Where the hub is: the spot it was built around (feet), and each portal opening's cells by map. */
	private record Layout(int x, int y, int z, Map<String, String> openings, @Nullable String facing) {
		/** The way the rows go from the spot (north for a hub kept before it had one). */
		Direction forward() {
			Direction named = facing == null ? null : Direction.byName(facing);
			return named != null && named.getAxis().isHorizontal() ? named : Direction.NORTH;
		}
	}

	private static final Gson GSON = new Gson();
	private static final int PORTAL_STEP = 4; // blocks between two portals' openings along a row
	private static final int ROW_STEP = 7; // blocks between two chapters' rows
	private static final int FIRST_ROW = 6; // blocks from the spot to the first row
	private static final int SIGN_LINE = 15; // characters a sign's line holds

	private static @Nullable Layout layout;
	private static boolean loaded;

	private HubPortals() {
	}

	private static Path file() {
		return Path.of(System.getenv("HALFCRAFT_NETWORK")).getParent().resolve("hub.json");
	}

	private static void load() {
		loaded = true;
		try {
			layout = GSON.fromJson(Files.readString(file()), Layout.class);
		} catch (IOException | RuntimeException e) {
			layout = null;
		}
	}

	/** Each hub tick: the hub built where the first player stands, then the players in the portals' openings go. */
	static void tick(MinecraftServer server) {
		if (!loaded) {
			load();
		}
		if (server.getTickCount() % 10 != 0) {
			return;
		}
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (layout == null) {
			for (ServerPlayer player : players) {
				// once its Half-Life spawn put it somewhere: a few seconds in
				if (player.tickCount > 200 && player.onGround()) {
					build(player.level(), player.blockPosition(), player.getDirection());
					break;
				}
			}
			return;
		}
		for (ServerPlayer player : players) {
			BlockPos feet = player.blockPosition();
			label(player, feet);
			String to = layout.openings().get(key(feet));
			if (to == null) {
				to = layout.openings().get(key(feet.above()));
			}
			if (to != null) {
				Network.send(player, to, null);
			}
		}
	}

	/** How near a portal's opening a player is told where it goes (over the hotbar). */
	private static final int LABEL_REACH = 3;

	/**
	 * The map and chapter of the portal nearest the player, over the hotbar while they're near one: Half-Life draws
	 * Minecraft's blocks, not the words on its signs.
	 */
	private static void label(ServerPlayer player, BlockPos feet) {
		String nearest = null;
		int best = Integer.MAX_VALUE;
		for (int dx = -LABEL_REACH; dx <= LABEL_REACH; dx++) {
			for (int dz = -LABEL_REACH; dz <= LABEL_REACH; dz++) {
				String to = layout.openings().get(key(feet.offset(dx, 0, dz)));
				int distance = dx * dx + dz * dz;
				if (to != null && distance < best) {
					best = distance;
					nearest = to;
				}
			}
		}
		Network.Pair pair = nearest != null ? Network.pair(nearest) : null;
		if (pair != null) {
			player.sendSystemMessage(Component.literal(pair.map() + "  —  " + pair.chapter()), true);
		}
	}

	/** /hub on the hub: back where it was built. */
	static void toSpawn(ServerPlayer player) {
		if (layout == null) {
			player.sendSystemMessage(Component.literal("Хаб ещё строится"));
			return;
		}
		player.teleportTo(player.level(), layout.x() + 0.5, layout.y(), layout.z() + 0.5, Set.of(), layout.forward().toYRot(), 0.0F, true);
	}

	private static String key(BlockPos pos) {
		return pos.getX() + "," + pos.getY() + "," + pos.getZ();
	}

	/** /hubhere: the hub, where it was, goes, and is built again where the player stands, its rows the way they look. */
	static void buildHere(ServerPlayer player) {
		ServerLevel level = player.level();
		if (layout != null) {
			clear(level, new BlockPos(layout.x(), layout.y(), layout.z()), layout.forward());
		}
		build(level, player.blockPosition(), player.getDirection());
		player.sendSystemMessage(Component.literal("Хаб построен здесь, ряды порталов вперёд"));
	}

	/** The block {@code side} blocks to the right of the spot, {@code ahead} blocks ahead of it and {@code up} above. */
	private static BlockPos at(BlockPos spot, Direction forward, int side, int up, int ahead) {
		Direction right = forward.getClockWise();
		return spot.offset(right.getStepX() * side + forward.getStepX() * ahead, up, right.getStepZ() * side + forward.getStepZ() * ahead);
	}

	private static int halfWidth() {
		int widest = 1;
		for (String chapter : Network.chapters()) {
			widest = Math.max(widest, mapsOf(chapter).size());
		}
		return widest * PORTAL_STEP / 2 + 3;
	}

	private static int depth() {
		return FIRST_ROW + Network.chapters().size() * ROW_STEP + 2;
	}

	/** Everything a hub built at {@code spot} the way {@code forward} put there, gone. */
	private static void clear(ServerLevel level, BlockPos spot, Direction forward) {
		int halfWidth = halfWidth(), depth = depth();
		for (int side = -halfWidth; side <= halfWidth; side++) {
			for (int ahead = -2; ahead < depth; ahead++) {
				for (int up = -1; up <= 5; up++) {
					level.setBlock(at(spot, forward, side, up, ahead), Blocks.AIR.defaultBlockState(), 2);
				}
			}
		}
	}

	/**
	 * The hub around {@code spot}: the chapters' rows go {@code forward} from it, each row's portals across, centred on
	 * the spot; a floor under all of it, the room over it cleared of Minecraft's blocks.
	 */
	private static void build(ServerLevel level, BlockPos spot, Direction forward) {
		List<String> chapters = Network.chapters();
		int halfWidth = halfWidth(), depth = depth();
		BlockState floor = Blocks.POLISHED_ANDESITE.defaultBlockState();
		for (int side = -halfWidth; side <= halfWidth; side++) {
			for (int ahead = -2; ahead < depth; ahead++) {
				level.setBlockAndUpdate(at(spot, forward, side, -1, ahead), floor);
				for (int up = 0; up <= 5; up++) {
					level.setBlockAndUpdate(at(spot, forward, side, up, ahead), Blocks.AIR.defaultBlockState());
				}
			}
		}
		Map<String, String> openings = new HashMap<>();
		for (int row = 0; row < chapters.size(); row++) {
			String chapter = chapters.get(row);
			List<Network.Pair> maps = mapsOf(chapter);
			int ahead = FIRST_ROW + row * ROW_STEP;
			int first = -(maps.size() - 1) * PORTAL_STEP / 2;
			// the chapter's name, at the row's left end, facing the spot
			BlockPos title = at(spot, forward, first - 3, 0, ahead - 1);
			int rotation = (forward.getOpposite().get2DDataValue() * 4 + 8) % 16;
			level.setBlockAndUpdate(title, Blocks.DARK_OAK_SIGN.defaultBlockState().setValue(StandingSignBlock.ROTATION, rotation));
			sign(level, title, (row + 1) + ". " + chapter, DyeColor.YELLOW);
			for (int i = 0; i < maps.size(); i++) {
				portal(level, spot, forward, first + i * PORTAL_STEP, ahead, maps.get(i), openings);
			}
		}
		layout = new Layout(spot.getX(), spot.getY(), spot.getZ(), openings, forward.getSerializedName());
		try {
			Files.writeString(file(), GSON.toJson(layout));
		} catch (IOException e) {
			HalfCraft.LOG.error("HalfCraft: can't keep where the hub is", e);
		}
		HalfCraft.LOG.info("HalfCraft: the hub built around {}, its rows {}: {} chapters, {} portals", spot.toShortString(), forward.getSerializedName(),
			chapters.size(), openings.size() / 2);
	}

	private static List<Network.Pair> mapsOf(String chapter) {
		List<Network.Pair> maps = new ArrayList<>();
		for (Network.Pair pair : Network.pairs()) {
			if (pair.chapter().equals(chapter)) {
				maps.add(pair);
			}
		}
		return maps;
	}

	/**
	 * A portal: a frame of obsidian round a 1 by 2 opening {@code side} blocks across and {@code ahead} blocks ahead of
	 * the spot, its map's name on a sign over it, facing the spot. Not a block that shines: Half-Life lights each light
	 * near the player with shadow maps, and 68 portals' crying obsidian made it crawl.
	 */
	private static void portal(ServerLevel level, BlockPos spot, Direction forward, int side, int ahead, Network.Pair pair, Map<String, String> openings) {
		BlockState frame = Blocks.OBSIDIAN.defaultBlockState();
		for (int across = -1; across <= 1; across++) {
			for (int up = -1; up <= 2; up++) {
				BlockPos pos = at(spot, forward, side + across, up, ahead);
				boolean opening = across == 0 && (up == 0 || up == 1);
				level.setBlockAndUpdate(pos, opening ? Blocks.AIR.defaultBlockState() : frame);
				if (opening) {
					openings.put(key(pos), pair.name());
				}
			}
		}
		BlockPos sign = at(spot, forward, side, 2, ahead - 1);
		level.setBlockAndUpdate(sign, Blocks.OAK_WALL_SIGN.defaultBlockState().setValue(WallSignBlock.FACING, forward.getOpposite()));
		sign(level, sign, pair.map(), DyeColor.BLACK);
	}

	private static void sign(ServerLevel level, BlockPos pos, String text, DyeColor colour) {
		if (!(level.getBlockEntity(pos) instanceof SignBlockEntity sign)) {
			return;
		}
		List<Component> lines = new ArrayList<>();
		String rest = text;
		while (lines.size() < 4) {
			if (rest.length() <= SIGN_LINE) {
				lines.add(Component.literal(rest));
				rest = "";
			} else {
				int cut = rest.lastIndexOf(' ', SIGN_LINE);
				cut = cut > 0 ? cut : SIGN_LINE;
				lines.add(Component.literal(rest.substring(0, cut)));
				rest = rest.substring(cut).strip();
			}
		}
		SignText front = new SignText(lines, lines, colour, colour != DyeColor.BLACK);
		sign.setText(front, SignTextSlot.FRONT);
		sign.setChanged();
		level.sendBlockUpdated(pos, sign.getBlockState(), sign.getBlockState(), 3);
	}
}
