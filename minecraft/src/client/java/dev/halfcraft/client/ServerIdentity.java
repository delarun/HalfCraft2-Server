package dev.halfcraft.client;

import dev.halfcraft.link.ServerPlayers;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.Minecraft;

/**
 * On the HalfCraft server, this Minecraft tells the server which Source player it is (HostState's sourceUserId, which
 * client.dll writes): the server's hurts, heals and deaths then reach the right player (ServerPlayers). And it tells
 * it their Half-Life weapons as client.dll writes them (WeaponsPayload), for the stand-ins in their inventory, and
 * Half-Life's water around them (WaterPayload), for their air and swimming on the server.
 */
final class ServerIdentity {
	private static int sent;
	private static dev.halfcraft.weapon.@org.jspecify.annotations.Nullable WeaponsPayload weaponsSent;
	private static dev.halfcraft.world.@org.jspecify.annotations.Nullable WaterPayload waterSent;

	private ServerIdentity() {
	}

	static void init() {
		ClientPlayConnectionEvents.JOIN.register((handler, sender, minecraft) -> {
			sent = 0;
			weaponsSent = null;
			waterSent = null;
		});
		ClientTickEvents.END_CLIENT_TICK.register(ServerIdentity::tick);
		// what the gravity gun's held cubes are, which only the server knows
		ClientPlayNetworking.registerGlobalReceiver(dev.halfcraft.world.HeldSync.TYPE, (payload, context) -> dev.halfcraft.world.HeldSync.received(payload));
		ClientPlayConnectionEvents.DISCONNECT.register((handler, minecraft) -> dev.halfcraft.world.HeldSync.clear());
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
		if (ClientPlayNetworking.canSend(dev.halfcraft.weapon.WeaponsPayload.TYPE)) {
			var weapons = dev.halfcraft.weapon.WeaponsPayload.of(dev.halfcraft.link.WeaponTable.current());
			if (!weapons.equals(weaponsSent)) {
				weaponsSent = weapons;
				ClientPlayNetworking.send(weapons);
			}
		}
		var water = dev.halfcraft.world.HostWater.playerGrid();
		if (water != null && ClientPlayNetworking.canSend(dev.halfcraft.world.WaterPayload.TYPE) && (waterSent == null || !waterSent.same(water))) {
			waterSent = dev.halfcraft.world.WaterPayload.of(water);
			ClientPlayNetworking.send(waterSent);
		}
	}
}
