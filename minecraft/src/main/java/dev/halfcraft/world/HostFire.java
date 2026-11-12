package dev.halfcraft.world;

import dev.halfcraft.link.Proto;
import dev.halfcraft.mobs.HostNav;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.FireBlock;
import net.minecraft.world.phys.AABB;

/**
 * Half-Life's wood burns as Minecraft's planks do (FireBlockMixin): fire stays on it, spreads into the air next to it,
 * and burns its cells away, dug out of Half-Life's walls (the wood behind revealed as planks, which go on burning).
 * A cell is Half-Life's wood when its diggable triangles are of planks.
 */
public final class HostFire {
	/** Minecraft's planks' odds (FireBlock.bootStrap): catching fire, and burning away. */
	public static final int IGNITE_ODDS = 5, BURN_ODDS = 20;

	private HostFire() {
	}

	/** Whether the cell is Half-Life's wood, not dug yet. */
	public static boolean isWood(BlockGetter getter, BlockPos pos) {
		if (!(getter instanceof Level level) || !HostNav.inMirror(level) || !HostCollision.hasGeometry(pos) || !level.isLoaded(pos)
			|| !level.getBlockState(pos).isAir() || HostDig.isDugAt(level, pos)) {
			return false;
		}
		List<HostTri> tris = new ArrayList<>();
		HostCollision.trianglesNear(new AABB(pos), tris);
		for (HostTri tri : tris) {
			if (tri.diggable() && tri.digMaterial == Proto.DIG_PLANKS) {
				return true;
			}
		}
		return false;
	}

	/** Whether Half-Life's wood is next to the cell. */
	public static boolean woodNear(BlockGetter getter, BlockPos pos) {
		for (Direction side : Direction.values()) {
			if (isWood(getter, pos.relative(side))) {
				return true;
			}
		}
		return false;
	}

	/**
	 * FireBlock.tick, after: the fire burns Half-Life's wood next to it as FireBlock.checkBurnOut burns a block (300 to
	 * the sides, 250 up and down; the fire's age makes it less likely to spread on into the burnt cell).
	 */
	public static void burnNear(ServerLevel level, BlockPos fire, RandomSource random) {
		if (!HostNav.inMirror(level)) {
			return;
		}
		int age = level.getBlockState(fire).getBlock() instanceof FireBlock ? level.getBlockState(fire).getValue(FireBlock.AGE) : 0;
		for (Direction side : Direction.values()) {
			BlockPos pos = fire.relative(side);
			int chance = side.getAxis() == Direction.Axis.Y ? 250 : 300;
			if (random.nextInt(chance) >= BURN_ODDS || !isWood(level, pos)) {
				continue;
			}
			if (!HostDig.burn(level, pos)) {
				continue;
			}
			if (random.nextInt(age + 10) < 5 && !level.isRainingAt(pos)) {
				level.setBlockAndUpdate(pos, Blocks.FIRE.defaultBlockState().setValue(FireBlock.AGE, Math.min(15, age + random.nextInt(5) / 4)));
			}
		}
	}
}
