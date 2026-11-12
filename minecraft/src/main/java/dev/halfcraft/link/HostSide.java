package dev.halfcraft.link;

import dev.halfcraft.HalfCraftServer;
import java.util.List;
import org.jspecify.annotations.Nullable;

/**
 * Half-Life as Minecraft's server side sees it: a single player's integrated server talks to its client's Half-Life
 * (HostLink), the HalfCraft server to its Source dedicated server's server.so (ServerLink, the first one for now).
 * Server-side code (combat, mobs, explosions, arrows, the gravity gun's blocks) goes through here.
 */
public final class HostSide {
	private HostSide() {
	}

	/** The HalfCraft server's link, or null in single player. */
	public static @Nullable ServerLink dedicated() {
		List<ServerLink> links = HalfCraftServer.links();
		return links.isEmpty() ? null : links.getFirst();
	}

	/** A live Half-Life is on the other end. */
	public static boolean active() {
		ServerLink link = dedicated();
		return link != null ? link.active() : HostLink.active();
	}

	/** Half-Life's actors near its player(s). */
	public static java.lang.foreign.@Nullable MemorySegment segment() {
		ServerLink link = dedicated();
		return link != null ? link.segment() : HostLink.segment();
	}

	/** Half-Life's actors near its player(s). */
	public static boolean readActors(List<ProtoStructs.ActorRecord> out) {
		ServerLink link = dedicated();
		return link != null ? link.readActors(out) : HostLink.readActors(out);
	}

	public static void pushEvent(int type, int actorId, float a, float b, float c, float d, int flags) {
		pushEvent(type, actorId, a, b, c, d, flags, 0, 0);
	}

	public static void pushEvent(int type, int actorId, float a, float b, float c, float d, int flags, int weapon) {
		pushEvent(type, actorId, a, b, c, d, flags, weapon, 0);
	}

	public static void pushEvent(int type, int actorId, float a, float b, float c, float d, int flags, int weapon, int attackerId) {
		pushEvent(type, actorId, a, b, c, d, flags, weapon, attackerId, 0);
	}

	/**
	 * Queues an event for Half-Life. Any thread.
	 *
	 * @param playerId the Source userid of the player it's about (McEvent.playerId), for the halfcraft server's
	 */
	public static void pushEvent(int type, int actorId, float a, float b, float c, float d, int flags, int weapon, int attackerId, int playerId) {
		ServerLink link = dedicated();
		if (link != null) {
			link.pushEvent(type, actorId, a, b, c, d, flags, weapon, attackerId, playerId);
		} else {
			HostLink.pushEvent(type, actorId, a, b, c, d, flags, weapon, attackerId);
		}
	}
}
