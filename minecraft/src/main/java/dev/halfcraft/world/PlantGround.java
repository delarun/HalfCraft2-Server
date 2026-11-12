package dev.halfcraft.world;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.link.Proto;
import dev.halfcraft.mobs.HostNav;
import java.util.ArrayList;
import java.util.List;
import net.fabricmc.fabric.api.event.player.UseBlockCallback;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundSource;
import net.minecraft.tags.ItemTags;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.VegetationBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.Vec3;

/**
 * Planting on Half-Life's ground: its terrain is triangles, not blocks, so a sapling, a flower or grass had nothing to
 * stand on and a hoe nothing to till. Used on the top of Half-Life's dirt or grass (or sand), the cell the surface is in
 * becomes Minecraft's own ground (HostDig.fill: dug out of Half-Life, a grass block or sand in its place), and the plant
 * goes on it, or a hoe makes it farmland. Server side, from the hit Minecraft's client sends (its point on the surface).
 */
public final class PlantGround {
	private PlantGround() {
	}

	public static void init() {
		UseBlockCallback.EVENT.register((player, level, hand, hit) -> {
			if (level.isClientSide() || !(player instanceof ServerPlayer server) || !(level instanceof ServerLevel serverLevel)) {
				return InteractionResult.PASS;
			}
			return use(serverLevel, server, player.getItemInHand(hand), hit);
		});
	}

	private static InteractionResult use(ServerLevel level, ServerPlayer player, ItemStack stack, BlockHitResult hit) {
		boolean plant = stack.getItem() instanceof BlockItem item && item.getBlock() instanceof VegetationBlock;
		boolean hoe = stack.is(ItemTags.HOES);
		if ((!plant && !hoe) || hit.getDirection() != Direction.UP || !HostNav.inMirror(level) || !player.mayBuild()) {
			return InteractionResult.PASS;
		}
		// the cell the surface is in, under the point hit, and the one over it
		Vec3 at = hit.getLocation();
		BlockPos ground = BlockPos.containing(at.x, at.y - 0.01, at.z);
		BlockPos above = ground.above();
		if (!level.isLoaded(ground) || !level.getBlockState(ground).isAir() || !level.getBlockState(above).isAir()) {
			return InteractionResult.PASS; // Minecraft's own blocks there: vanilla's business
		}
		int material = groundMaterial(at);
		if (material != Proto.DIG_DIRT && material != Proto.DIG_SAND) {
			return InteractionResult.PASS; // concrete, wood, metal: nothing grows on them
		}
		if (hoe) {
			if (material != Proto.DIG_DIRT) {
				return InteractionResult.PASS;
			}
			HostDig.fill(level, ground, Blocks.FARMLAND.defaultBlockState());
			level.playSound(null, ground, net.minecraft.sounds.SoundEvents.HOE_TILL.value(), SoundSource.BLOCKS, 1.0F, 1.0F);
			if (!player.isCreative()) {
				stack.hurtAndBreak(1, player, net.minecraft.world.entity.EquipmentSlot.MAINHAND);
			}
			HalfCraft.LOG.info("HalfCraft: {} tilled Half-Life's ground at {}", player.getName().getString(), ground.toShortString());
			return InteractionResult.SUCCESS;
		}
		BlockState plantState = ((BlockItem) stack.getItem()).getBlock().defaultBlockState();
		HostDig.fill(level, ground, material == Proto.DIG_SAND ? Blocks.SAND.defaultBlockState() : Blocks.GRASS_BLOCK.defaultBlockState());
		if (!plantState.canSurvive(level, above)) {
			return InteractionResult.FAIL; // it doesn't grow on that: the ground stays Minecraft's
		}
		level.setBlockAndUpdate(above, plantState);
		var sound = plantState.getSoundType();
		level.playSound(null, above, sound.getPlaceSound(), SoundSource.BLOCKS, (sound.getVolume() + 1.0F) / 2.0F, sound.getPitch() * 0.8F);
		if (!player.isCreative()) {
			stack.shrink(1);
		}
		HalfCraft.LOG.info("HalfCraft: {} planted {} on Half-Life's ground at {}", player.getName().getString(), plantState.getBlock().getName().getString(),
			above.toShortString());
		return InteractionResult.SUCCESS;
	}

	/** The dig material of Half-Life's ground at the point (its diggable triangle nearest), DIG_NONE for none. */
	private static int groundMaterial(Vec3 at) {
		List<HostTri> tris = new ArrayList<>();
		HostCollision.trianglesNear(new AABB(at.x - 0.05, at.y - 0.05, at.z - 0.05, at.x + 0.05, at.y + 0.05, at.z + 0.05), tris);
		for (HostTri tri : tris) {
			if (tri.diggable()) {
				return tri.digMaterial;
			}
		}
		return Proto.DIG_NONE;
	}
}
