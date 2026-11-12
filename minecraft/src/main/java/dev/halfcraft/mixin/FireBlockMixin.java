package dev.halfcraft.mixin;

import dev.halfcraft.world.HostFire;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.LevelReader;
import net.minecraft.world.level.block.FireBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Half-Life's wood burns as Minecraft's planks do (HostFire). */
@Mixin(FireBlock.class)
public abstract class FireBlockMixin {
	// fire stays next to Half-Life's wood, as next to a flammable block
	@Inject(method = "isValidFireLocation", at = @At("RETURN"), cancellable = true)
	private void halfcraft$nextToHostWood(BlockGetter level, BlockPos pos, CallbackInfoReturnable<Boolean> cir) {
		if (!cir.getReturnValueZ() && HostFire.woodNear(level, pos)) {
			cir.setReturnValue(true);
		}
	}

	// the air next to Half-Life's wood catches fire, as next to planks
	@Inject(method = "getIgniteOdds(Lnet/minecraft/world/level/LevelReader;Lnet/minecraft/core/BlockPos;)I", at = @At("RETURN"), cancellable = true)
	private void halfcraft$hostWoodCatches(LevelReader level, BlockPos pos, CallbackInfoReturnable<Integer> cir) {
		if (cir.getReturnValueI() < HostFire.IGNITE_ODDS && level.isEmptyBlock(pos) && HostFire.woodNear(level, pos)) {
			cir.setReturnValue(HostFire.IGNITE_ODDS);
		}
	}

	// and it burns away
	@Inject(method = "tick", at = @At("TAIL"))
	private void halfcraft$burnHostWood(BlockState state, ServerLevel level, BlockPos pos, RandomSource random, CallbackInfo ci) {
		HostFire.burnNear(level, pos, random);
	}
}
