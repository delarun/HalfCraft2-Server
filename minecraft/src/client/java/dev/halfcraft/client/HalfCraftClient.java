package dev.halfcraft.client;

import dev.halfcraft.combat.HostCombat;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.minecraft.client.renderer.entity.EntityRenderers;
import net.minecraft.client.renderer.entity.NoopRenderer;

public final class HalfCraftClient implements ClientModInitializer {
	@Override
	public void onInitializeClient() {
		dev.halfcraft.link.HostLink.announceRunning();
		HostCommands.register();

		ClientTickEvents.END_CLIENT_TICK.register(HostClient::clientTick);
		ClientTickEvents.END_CLIENT_TICK.register(HostDigClient::tick);
		HostScreenshots.init();
		HostHints.init();
		HostPickups.init();
		HostCaptions.init();
		// Half-Life draws the real NPC; its Minecraft stand-in is only a hitbox.
		EntityRenderers.register(HostCombat.HOST_ACTOR, NoopRenderer::new);
		// Players (client-side movement AND the integrated server's re-check of it) use the smooth
		// triangle collider, never Half-Life's voxels; otherwise the server sees the smooth position
		// dip into a voxel and teleports the player back every few ticks.
		dev.halfcraft.world.HostCollision.setSmoothCollider(e -> e instanceof net.minecraft.world.entity.player.Player && HostClient.linked());
		// on the halfcraft server, half-life's collision comes from the server, which relays its source server's
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.registerGlobalReceiver(dev.halfcraft.world.CollisionRelay.TYPE, (payload, context) -> {
			for (dev.halfcraft.world.CollisionRelay.Message message : payload.messages()) {
				dev.halfcraft.world.HostCollision.applyRelayed(message.type(), message.bytes());
			}
		});
		WeaponHud.init();
	}
}
