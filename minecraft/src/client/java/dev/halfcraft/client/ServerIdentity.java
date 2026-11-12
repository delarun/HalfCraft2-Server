package dev.halfcraft.client;

import dev.halfcraft.link.ServerPlayers;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.Minecraft;

/**
 * On the HalfCraft server, this Minecraft tells the server which Source player it is (HostState's sourceUserId, which
 * client.dll writes): the server's hurts, heals and deaths then reach the right player (ServerPlayers).
 */
final class ServerIdentity {
	private static int sent;

	private ServerIdentity() {
	}

	static void init() {
		ClientPlayConnectionEvents.JOIN.register((handler, sender, minecraft) -> sent = 0);
		ClientTickEvents.END_CLIENT_TICK.register(ServerIdentity::tick);
	}

	private static void tick(Minecraft minecraft) {
		if (!HostClient.linked() || minecraft.getSingleplayerServer() != null || minecraft.getConnection() == null
			|| !ClientPlayNetworking.canSend(ServerPlayers.SourcePlayerPayload.TYPE)) {
			return;
		}
		int userId = HostClient.host().sourceUserId;
		if (userId != sent) {
			sent = userId;
			ClientPlayNetworking.send(new ServerPlayers.SourcePlayerPayload(userId));
		}
	}
}
