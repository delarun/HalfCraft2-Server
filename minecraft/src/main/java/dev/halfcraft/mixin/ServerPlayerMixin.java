package dev.halfcraft.mixin;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.HostPlayers;
import dev.halfcraft.combat.HostCombat;
import dev.halfcraft.combat.HostActorEntity;
import dev.halfcraft.link.Proto;
import dev.halfcraft.link.HostLink;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.server.level.ServerPlayer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ServerPlayer.class)
public abstract class ServerPlayerMixin {
	/** Critical hits on a Half-Life actor are flagged so Half-Life can play them up. */
	@Inject(method = "crit", at = @At("HEAD"))
	private void halfcraft$critHost(Entity entity, CallbackInfo ci) {
		if (entity instanceof HostActorEntity proxy) {
			proxy.markCritical();
		}
	}

	/** The host dying in Minecraft is dying in Half-Life; a guest's death is Minecraft's own business. */
	@Inject(method = "die", at = @At("HEAD"))
	private void halfcraft$diesInHost(DamageSource source, CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		if (!HostPlayers.isHost(self)) {
			HalfCraft.LOG.info("HalfCraft: {} died in Minecraft (a guest: Half-Life isn't told)", self.getName().getString());
			return;
		}
		int attacker = HostCombat.attackerActorId(source);
		if (HostLink.active()) {
			HostLink.pushEvent(Proto.EV_PLAYER_DIED, attacker, 0, 0, 0, 0, 0);
			HalfCraft.LOG.info("HalfCraft: player died ({}); telling Half-Life", source.getMsgId());
		}
	}
}
