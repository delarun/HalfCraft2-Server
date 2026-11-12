package dev.halfcraft.mixin;

import dev.halfcraft.world.HostDig;
import dev.halfcraft.world.Rollback;
import dev.halfcraft.world.ServerSolids;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Every block change in the mirror world goes past the rollback's log (Rollback) and the reveals of dug cells (HostDig) and, on the
 * HalfCraft server, the solids its Source server gets (ServerSolids). */
@Mixin(LevelChunk.class)
public abstract class LevelChunkMixin {
	@Inject(method = "setBlockState", at = @At("HEAD"))
	private void halfcraft$before(BlockPos pos, BlockState state, int flags, CallbackInfoReturnable<BlockState> cir) {
		Rollback.beforeChange((LevelChunk) (Object) this, pos);
	}

	@Inject(method = "setBlockState", at = @At("RETURN"))
	private void halfcraft$after(BlockPos pos, BlockState state, int flags, CallbackInfoReturnable<BlockState> cir) {
		Rollback.afterChange((LevelChunk) (Object) this, pos);
		HostDig.afterChange((LevelChunk) (Object) this, pos, cir.getReturnValue(), state);
		ServerSolids.afterChange((LevelChunk) (Object) this, pos);
	}
}
