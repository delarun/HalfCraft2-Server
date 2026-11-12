package dev.halfcraft;

import dev.halfcraft.link.ServerLink;
import dev.halfcraft.world.HostCollision;
import java.util.ArrayList;
import java.util.List;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;

/**
 * The HalfCraft server's Minecraft (docs/server/PLAN.md): a dedicated server linked to the Source dedicated servers'
 * server.so on the links HALFCRAFT_SERVER_LINKS names (comma separated, srv by default). It reads their collision
 * (the first one's, for now: one map) and its players collide with Half-Life's triangles, as a single player's
 * integrated server does with its client's.
 */
public final class HalfCraftServer {
	public static final String VARIABLE = "HALFCRAFT_SERVER_LINKS";
	private static final List<ServerLink> LINKS = new ArrayList<>();

	private HalfCraftServer() {
	}

	static void init() {
		ServerLifecycleEvents.SERVER_STARTED.register(HalfCraftServer::started);
		ServerTickEvents.END_SERVER_TICK.register(server -> {
			for (ServerLink link : LINKS) {
				link.tick();
			}
		});
	}

	/** The server links, empty but on a dedicated server. */
	public static List<ServerLink> links() {
		return LINKS;
	}

	private static void started(MinecraftServer server) {
		if (!server.isDedicatedServer()) {
			return;
		}
		String named = System.getenv(VARIABLE);
		for (String id : (named == null || named.isBlank() ? "srv" : named).split(",")) {
			ServerLink link = ServerLink.of(id.strip());
			if (link != null) {
				LINKS.add(link);
			}
		}
		if (LINKS.isEmpty()) {
			HalfCraft.LOG.error("HalfCraft: no server link in {}: this server links to no Source server", VARIABLE);
			return;
		}
		ServerLink first = LINKS.getFirst();
		HostCollision.startConsumer(first);
		HostCollision.setSmoothCollider(entity -> entity instanceof ServerPlayer && first.active());
		HalfCraft.LOG.info("HalfCraft: the halfcraft server's minecraft, on server link(s) {}", LINKS.stream().map(ServerLink::id).toList());
	}
}
