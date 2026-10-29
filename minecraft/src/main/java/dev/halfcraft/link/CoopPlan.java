package dev.halfcraft.link;

import org.jspecify.annotations.Nullable;

/**
 * What Minecraft does about Half-Life's co-op mode (HostState's coopMode, coopIp and coopPort), one
 * step a frame: a host opens its own world to LAN on the port it's given, a guest leaves its own world
 * and joins the host's, and either goes back when Half-Life goes solo. A step that fails (the port is
 * taken, the join is refused or dropped) is tried again after {@link Proto#COOP_RETRY_MS}. Pure logic
 * with no Minecraft types: CoopClient fills in {@link Now} and does the {@link Action}.
 */
public final class CoopPlan {
	private CoopPlan() {
	}

	/** The one thing to do this frame. */
	public enum Action {
		/** Nothing to do. */
		NONE,
		/** Open the own world to LAN on the wanted port. */
		OPEN,
		/** Close the own world to LAN. */
		CLOSE,
		/** Leave the world Minecraft is in, or stop the join under way, for the title screen. */
		LEAVE,
		/** Join the host's world at {@link #address}. */
		JOIN,
		/** A guest with nothing to do yet: no address, a join under way, or a failure not long ago. */
		WAIT
	}

	/** What Half-Life wants: its CoopMode, and the host's IPv4 (a guest's) and the Minecraft port. */
	public record Wanted(int mode, int ip, int port) {
	}

	/**
	 * Where Minecraft is: its own world open (its integrated server runs, or is starting), that world
	 * open to LAN and on which port, the address of another's world it's in (null when none), a join
	 * under way, and when the last open or join failed (0: no failure).
	 */
	public record Now(boolean ownWorld, boolean published, int publishedPort, @Nullable String remote, boolean connecting, long failedAtMs) {
	}

	/** The step to take now (nowMs on the clock failedAtMs is on). */
	public static Action next(Wanted wanted, Now now, long nowMs) {
		return switch (wanted.mode()) {
			case Proto.COOP_HOST -> host(wanted, now, nowMs);
			case Proto.COOP_GUEST -> guest(wanted, now, nowMs);
			default -> solo(now);
		};
	}

	/** What Half-Life is told (McState's coopState): one of Proto's COOP_IDLE to COOP_FAILED. */
	public static int state(Wanted wanted, Now now) {
		return switch (wanted.mode()) {
			case Proto.COOP_HOST -> now.published() ? Proto.COOP_OPEN : failed(now) ? Proto.COOP_FAILED : Proto.COOP_IDLE;
			case Proto.COOP_GUEST -> {
				if (address(wanted.ip(), wanted.port()).equals(now.remote())) {
					yield Proto.COOP_JOINED;
				}
				yield now.connecting() ? Proto.COOP_JOINING : failed(now) ? Proto.COOP_FAILED : Proto.COOP_IDLE;
			}
			default -> Proto.COOP_IDLE;
		};
	}

	/** a.b.c.d:port, from an IPv4 as HostState's coopIp holds it (a << 24 | b << 16 | c << 8 | d). */
	public static String address(int ip, int port) {
		return dotted(ip) + ":" + port;
	}

	/** a.b.c.d, from an IPv4 as HostState's coopIp holds it. */
	public static String dotted(int ip) {
		return (ip >>> 24) + "." + (ip >>> 16 & 0xFF) + "." + (ip >>> 8 & 0xFF) + "." + (ip & 0xFF);
	}

	/** Whether Minecraft keeps a world of its own: anything but a guest, which plays in the host's. */
	public static boolean wantsOwnWorld(Wanted wanted) {
		return wanted.mode() != Proto.COOP_GUEST;
	}

	private static Action solo(Now now) {
		if (now.remote() != null || now.connecting()) {
			return Action.LEAVE;
		}
		return now.published() ? Action.CLOSE : Action.NONE;
	}

	private static Action host(Wanted wanted, Now now, long nowMs) {
		if (now.remote() != null || now.connecting()) {
			return Action.LEAVE;
		}
		if (!now.ownWorld()) {
			return Action.NONE;
		}
		if (now.published()) {
			return now.publishedPort() != wanted.port() ? Action.CLOSE : Action.NONE;
		}
		return wanted.port() > 0 && retryDue(now, nowMs) ? Action.OPEN : Action.NONE;
	}

	private static Action guest(Wanted wanted, Now now, long nowMs) {
		if (wanted.port() == 0 || wanted.ip() == 0) {
			return Action.WAIT;
		}
		if (now.ownWorld()) {
			return Action.LEAVE;
		}
		if (now.remote() != null) {
			return now.remote().equals(address(wanted.ip(), wanted.port())) ? Action.NONE : Action.LEAVE;
		}
		if (now.connecting()) {
			return Action.WAIT;
		}
		return retryDue(now, nowMs) ? Action.JOIN : Action.WAIT;
	}

	private static boolean failed(Now now) {
		return now.failedAtMs() != 0;
	}

	private static boolean retryDue(Now now, long nowMs) {
		return !failed(now) || nowMs - now.failedAtMs() >= Proto.COOP_RETRY_MS;
	}
}
