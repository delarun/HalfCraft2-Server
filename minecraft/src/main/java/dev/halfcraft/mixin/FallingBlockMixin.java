package dev.halfcraft.mixin;

import dev.halfcraft.world.HostCollision;
import dev.halfcraft.world.HostDig;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.FallingBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Sand and gravel rest on Half-Life ground instead of falling forever through it, and on the rock under it and the void
 * beyond the map (not dug): revealed sand over that rock fell, and the cell it left revealed more sand around, on and on.
 */
@Mixin(FallingBlock.class)
public abstract class FallingBlockMixin {
	@Inject(method = "tick", at = @At("HEAD"), cancellable = true)
	private void halfcraft$restOnHost(BlockState state, ServerLevel level, BlockPos pos, RandomSource random, CallbackInfo ci) {
		if (HostCollision.supportsFromBelow(pos)) {
			ci.cancel();
			return;
		}
		long[] rock = HostDig.voidLayers(level, pos.below());
		if (rock != null && rock[7] != 0L) {
			ci.cancel();
		}
	}
}
