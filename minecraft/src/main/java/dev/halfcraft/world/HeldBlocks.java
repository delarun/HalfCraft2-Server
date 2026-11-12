package dev.halfcraft.world;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.link.HostLink;
import dev.halfcraft.link.Proto;
import dev.halfcraft.mobs.HostNav;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.BiConsumer;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.SoundType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.NoteBlockInstrument;
import org.jspecify.annotations.Nullable;

/**
 * Blocks Half-Life's gravity gun tore out of the mirror world (see server.dll's hc_held_blocks.h).
 *
 * <p>While Half-Life throws one around as a physics cube, the block is out of Minecraft's world and
 * remembered here by Half-Life's held slot, with where it came from; WorldExporter tells Half-Life its
 * faces ({@code Proto.WE_HELD_BLOCK}). Where the cube comes to rest the block goes back into the world
 * like falling sand, or drops as an item without room; a cube that goes without coming to rest leaves it
 * as an item where it was. A checkpoint made meanwhile has the block where it was taken from (cubes
 * aren't in Half-Life's saves), and the world closing puts it back there.
 *
 * <p>The changes run on the server thread; the client thread only reads what's held.
 */
public final class HeldBlocks {
	/** A block a cube is: its state, and where it was taken from; or one of Minecraft's dropped items (state air). */
	private record Held(BlockState state, BlockPos origin, @Nullable ItemStack item) {
	}

	private static final Map<Integer, Held> HELD = new ConcurrentHashMap<>();

	private HeldBlocks() {
	}

	/**
	 * Whether the gravity gun may tear the block out: a whole cube (Half-Life draws and throws it as one)
	 * without a block entity (a chest's contents have nowhere to go), softer than obsidian.
	 */
	public static boolean takeable(BlockState state, BlockGetter level, BlockPos pos) {
		if (state.isAir() || state.hasBlockEntity() || !state.getFluidState().isEmpty()) {
			return false;
		}
		float hardness = state.getDestroySpeed(level, pos);
		return hardness >= 0.0F && hardness < Blocks.OBSIDIAN.defaultDestroyTime() && state.isCollisionShapeFullBlock(level, pos);
	}

	/** What each held slot is, for WorldExporter. Any thread. */
	public static void forEachHeld(BiConsumer<Integer, BlockState> action) {
		HELD.forEach((slot, held) -> {
			if (held.item() == null) {
				action.accept(slot, held.state());
			}
		});
	}

	/** The items held slots are (the gravity gun took them as dropped), for WorldExporter. Any thread. */
	public static void forEachHeldItem(BiConsumer<Integer, ItemStack> action) {
		HELD.forEach((slot, held) -> {
			if (held.item() != null) {
				action.accept(slot, held.item());
			}
		});
	}

	/** Where each held block was taken from, and what it is: a checkpoint has them there. Server thread. */
	static void forEachOrigin(BiConsumer<BlockPos, BlockState> action) {
		HELD.values().forEach(held -> {
			if (held.item() == null) {
				action.accept(held.origin(), held.state());
			}
		});
	}

	/** Proto.IN_TAKE_ITEM: the gravity gun took the dropped item with that entity id into the slot. Any thread. */
	public static void takeItem(@Nullable MinecraftServer server, int slot, int entityId) {
		if (server != null) {
			server.execute(() -> takeItemNow(server, slot, entityId));
		}
	}

	/** Proto.IN_TAKE_BLOCK: the gravity gun tore the block at pos out into the slot. Any thread. */
	public static void take(@Nullable MinecraftServer server, int slot, BlockPos pos) {
		if (server != null) {
			server.execute(() -> takeNow(server, slot, pos));
		}
	}

	/** Proto.IN_HELD_BLOCK_LANDED: the slot's cube came to rest in the cell. Any thread. */
	public static void landed(@Nullable MinecraftServer server, int slot, BlockPos cell) {
		if (server != null) {
			server.execute(() -> landedNow(server, slot, cell));
		}
	}

	/** Proto.IN_HELD_BLOCK_LOST: the slot's cube went without coming to rest; it was last in the cell. Any thread. */
	public static void lost(@Nullable MinecraftServer server, int slot, BlockPos cell) {
		if (server != null) {
			server.execute(() -> lostNow(server, slot, cell));
		}
	}

	/** Half-Life loaded a save: its cubes went with the level, and the checkpoint has their blocks. Server thread. */
	static void forget() {
		HELD.clear();
		HeldSync.changed();
	}

	/** The world is closing: each held block goes back where it was taken from, or lies there as an item. */
	public static void putAllBack(MinecraftServer server) {
		ServerLevel level = server.overworld();
		HELD.values().forEach(held -> {
			if (held.item() != null) {
				dropItem(level, held.item(), held.origin());
			} else {
				put(level, held.state(), held.origin());
			}
		});
		HELD.clear();
		HeldSync.changed();
	}

	private static void takeNow(MinecraftServer server, int slot, BlockPos pos) {
		ServerLevel level = mirror(server);
		BlockState state = level != null && level.isLoaded(pos) ? level.getBlockState(pos) : Blocks.AIR.defaultBlockState();
		if (level == null || !takeable(state, level, pos)) {
			// changed since Half-Life saw it (or never was one it could take)
			dev.halfcraft.link.HostSide.pushEvent(Proto.EV_HELD_BLOCK, slot, 0.0F, 0.0F, 0.0F, 0.0F, Proto.HELD_REFUSED, 0);
			return;
		}
		// out of the world as if broken (its neighbours react), with nothing dropped
		level.setBlock(pos, Blocks.AIR.defaultBlockState(), Block.UPDATE_ALL);
		HELD.put(slot, new Held(state, pos.immutable(), null));
		HeldSync.changed();
		int material = material(state);
		dev.halfcraft.link.HostSide.pushEvent(Proto.EV_HELD_BLOCK, slot, 0.0F, 0.0F, 0.0F, 0.0F, Proto.HELD_TAKEN, material);
		HalfCraft.LOG.info("HalfCraft: the gravity gun tore {} out at {} (held slot {}, material {})", name(state), pos.toShortString(), slot, material);
	}

	private static void takeItemNow(MinecraftServer server, int slot, int entityId) {
		ServerLevel level = mirror(server);
		if (level == null || !(level.getEntity(entityId) instanceof ItemEntity entity) || !entity.isAlive() || entity.getItem().isEmpty()) {
			// picked up or gone since Half-Life saw it
			dev.halfcraft.link.HostSide.pushEvent(Proto.EV_HELD_BLOCK, slot, 0.0F, 0.0F, 0.0F, 0.0F, Proto.HELD_REFUSED, 0);
			return;
		}
		ItemStack stack = entity.getItem().copy();
		entity.discard();
		HELD.put(slot, new Held(Blocks.AIR.defaultBlockState(), entity.blockPosition(), stack));
		HeldSync.changed();
		int material = stack.getItem() instanceof BlockItem block ? material(block.getBlock().defaultBlockState()) : Proto.MAT_WOOD;
		dev.halfcraft.link.HostSide.pushEvent(Proto.EV_HELD_BLOCK, slot, 0.0F, 0.0F, 0.0F, 0.0F, Proto.HELD_TAKEN, material);
		HalfCraft.LOG.info("HalfCraft: the gravity gun took {} x{} (held slot {})", BuiltInRegistries.ITEM.getKey(stack.getItem()), stack.getCount(), slot);
	}

	private static void landedNow(MinecraftServer server, int slot, BlockPos cell) {
		Held held = HELD.remove(slot);
		HeldSync.changed();
		ServerLevel level = mirror(server);
		if (held == null || level == null) {
			// nothing remembered for it (Minecraft started over meanwhile): the cube just goes
			dev.halfcraft.link.HostSide.pushEvent(Proto.EV_HELD_BLOCK, slot, 0.0F, 0.0F, 0.0F, 0.0F, Proto.HELD_DROPPED, 0);
			return;
		}
		if (held.item() != null) {
			// an item drops where it came to rest, as one
			dropItem(level, held.item(), cell);
			dev.halfcraft.link.HostSide.pushEvent(Proto.EV_HELD_BLOCK, slot, 0.0F, 0.0F, 0.0F, 0.0F, Proto.HELD_DROPPED, 0);
			return;
		}
		boolean placed = put(level, held.state(), cell);
		dev.halfcraft.link.HostSide.pushEvent(Proto.EV_HELD_BLOCK, slot, 0.0F, 0.0F, 0.0F, 0.0F, placed ? Proto.HELD_PLACED : Proto.HELD_DROPPED, 0);
		HalfCraft.LOG.info("HalfCraft: held slot {} came to rest at {}: {} {}", slot, cell.toShortString(), name(held.state()),
			placed ? "back in the world" : "dropped, no room");
	}

	private static void lostNow(MinecraftServer server, int slot, BlockPos cell) {
		Held held = HELD.remove(slot);
		HeldSync.changed();
		ServerLevel level = mirror(server);
		if (held == null || level == null) {
			return;
		}
		if (held.item() != null) {
			dropItem(level, held.item(), cell);
			return;
		}
		drop(level, held.state(), cell);
		HalfCraft.LOG.info("HalfCraft: held slot {} went in Half-Life: {} dropped at {}", slot, name(held.state()), cell.toShortString());
	}

	/**
	 * The block into the cell, like falling sand landing: if what's there gives way (air, water, grass)
	 * and the block can stand there; otherwise it drops as an item. @return whether it's in the world
	 */
	private static boolean put(ServerLevel level, BlockState state, BlockPos cell) {
		if (level.isInWorldBounds(cell) && level.getBlockState(cell).canBeReplaced() && state.canSurvive(level, cell)) {
			level.setBlock(cell, state, Block.UPDATE_ALL);
			return true;
		}
		drop(level, state, cell);
		return false;
	}

	/** A held item back into the world, still, in the middle of the cell's floor. */
	private static void dropItem(ServerLevel level, ItemStack stack, BlockPos cell) {
		ItemEntity entity = new ItemEntity(level, cell.getX() + 0.5, cell.getY() + 0.1, cell.getZ() + 0.5, stack, 0.0, 0.0, 0.0);
		entity.setPickUpDelay(10);
		level.addFreshEntity(entity);
	}

	private static void drop(ServerLevel level, BlockState state, BlockPos cell) {
		if (state.getBlock().asItem() != Items.AIR) {
			Block.popResource(level, cell, new ItemStack(state.getBlock()));
		}
	}

	private static @Nullable ServerLevel mirror(MinecraftServer server) {
		ServerLevel level = server.overworld();
		return HostNav.inMirror(level) ? level : null;
	}

	private static String name(BlockState state) {
		return BuiltInRegistries.BLOCK.getKey(state.getBlock()).toString();
	}

	/** What the block is made of, for Half-Life's physics (Proto.MAT_*): by the way it sounds. */
	static int material(BlockState state) {
		SoundType sound = state.getSoundType();
		if (sound == SoundType.GLASS) {
			return Proto.MAT_GLASS;
		}
		if (sound == SoundType.SNOW || sound == SoundType.POWDER_SNOW) {
			return Proto.MAT_SNOW;
		}
		if (sound == SoundType.WOOL) {
			return Proto.MAT_WOOL;
		}
		if (sound == SoundType.SAND || sound == SoundType.SUSPICIOUS_SAND || sound == SoundType.SOUL_SAND) {
			return Proto.MAT_SAND;
		}
		if (state.is(Blocks.GRAVEL) || sound == SoundType.SUSPICIOUS_GRAVEL) {
			return Proto.MAT_GRAVEL;
		}
		// Minecraft's dirt sounds like gravel
		if (sound == SoundType.GRAVEL || sound == SoundType.ROOTED_DIRT || sound == SoundType.MUD || sound == SoundType.SOUL_SOIL) {
			return Proto.MAT_DIRT;
		}
		if (sound == SoundType.GRASS || sound == SoundType.AZALEA_LEAVES || sound == SoundType.CHERRY_LEAVES || sound == SoundType.MOSS) {
			return Proto.MAT_GRASS;
		}
		if (sound == SoundType.METAL || sound == SoundType.IRON || sound == SoundType.ANVIL || sound == SoundType.COPPER || sound == SoundType.NETHERITE_BLOCK) {
			return Proto.MAT_METAL;
		}
		// every wooden block plays the note block's bass
		return state.instrument() == NoteBlockInstrument.BASS ? Proto.MAT_WOOD : Proto.MAT_STONE;
	}
}
