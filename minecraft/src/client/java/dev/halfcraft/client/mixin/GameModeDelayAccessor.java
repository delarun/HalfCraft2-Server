package dev.halfcraft.client.mixin;

import net.minecraft.client.multiplayer.MultiPlayerGameMode;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Minecraft's destroy delay, which mining Half-Life's walls shares (HostDigClient): one click, one block. */
@Mixin(MultiPlayerGameMode.class)
public interface GameModeDelayAccessor {
	@Accessor("destroyDelay")
	int halfcraft$getDestroyDelay();

	@Accessor("destroyDelay")
	void halfcraft$setDestroyDelay(int delay);
}
