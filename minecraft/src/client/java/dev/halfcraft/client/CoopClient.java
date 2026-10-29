package dev.halfcraft.client;

import com.mojang.blaze3d.platform.InputConstants;
import dev.halfcraft.HalfCraft;
import dev.halfcraft.link.CoopPlan;
import dev.halfcraft.link.HostLink;
import dev.halfcraft.link.Proto;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.events.GuiEventListener;
import net.minecraft.client.gui.screens.ConnectScreen;
import net.minecraft.client.gui.screens.DisconnectedScreen;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.gui.screens.TitleScreen;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.multiplayer.ServerData;
import net.minecraft.client.multiplayer.resolver.ServerAddress;
import net.minecraft.client.server.IntegratedServer;
import net.minecraft.server.MinecraftServer;
import org.jspecify.annotations.Nullable;

/**
 * Minecraft's side of a co-op session, from HostState's co-op fields: the host opens its own world
 * to LAN on the port it's given, a guest leaves its own world and joins the host's, and either goes
 * back when Half-Life goes solo. {@link CoopPlan} decides; this does it, once a frame on the render
 * thread (HostClient.beginFrame, before MirrorWorld, which opens no world of its own for a guest).
 * McState's coopState and coopPort report how it went.
 */
public final class CoopClient {
	private static final long RETRY_S = Proto.COOP_RETRY_MS / 1000;

	private static CoopPlan.Wanted wanted = new CoopPlan.Wanted(Proto.COOP_SOLO, 0, 0);
	private static int state = Proto.COOP_IDLE;
	private static int publishedPort;
	private static long failedAtMs; // the last open or join that failed (0: none since the last success or new target)
	private static @Nullable String joining; // a join started and not yet in the host's world, nor failed

	private CoopClient() {
	}

	/** Once a frame, after HostClient read the HostState: one step toward what Half-Life wants. */
	public static void frame(Minecraft minecraft, HostLink.HostState host) {
		if (host.coopMode != wanted.mode() || host.coopIp != wanted.ip() || host.coopPort != wanted.port()) {
			wanted = new CoopPlan.Wanted(host.coopMode, host.coopIp, host.coopPort);
			failedAtMs = 0; // a new target is tried at once
			HalfCraft.LOG.info("HalfCraft: Half-Life's co-op: {}", mode());
		}
		long nowMs = System.currentTimeMillis();
		Screen screen = minecraft.gui.screen();
		// a guest's join refused, timed out or dropped (the host quit, a kick, a duplicate login): try again
		// later, from the title screen. MirrorWorld takes everyone else's (it opens no world for a guest)
		if (wanted.mode() == Proto.COOP_GUEST && screen instanceof DisconnectedScreen && minecraft.level == null) {
			HalfCraft.LOG.info("HalfCraft: out of the host's world at {}: {}; trying again in {} s", target(), screen.getNarrationMessage().getString(), RETRY_S);
			failedAtMs = nowMs;
			joining = null;
			screen = new TitleScreen();
			minecraft.gui.setScreen(screen);
			MirrorWorld.reset();
		}

		IntegratedServer server = minecraft.getSingleplayerServer();
		boolean published = server != null && server.isPublished();
		String remote = remote(minecraft);
		boolean connecting = screen instanceof ConnectScreen;
		if (joining != null && !connecting) {
			// the connect screen went without a failure screen and without the host's world (something
			// else took its place): counted as a failure, so it can't turn into a join every frame
			if (remote == null && wanted.mode() == Proto.COOP_GUEST) {
				HalfCraft.LOG.warn("HalfCraft: the join to {} stopped on {}; trying again in {} s", joining, screen == null ? "no screen" : screen.getClass().getName(), RETRY_S);
				failedAtMs = nowMs;
			}
			joining = null;
		}
		CoopPlan.Now now = new CoopPlan.Now(server != null, published, published ? server.getPort() : 0, remote, connecting, failedAtMs);

		int wasState = state;
		state = CoopPlan.state(wanted, now);
		publishedPort = state == Proto.COOP_OPEN ? now.publishedPort() : 0;
		if (state == Proto.COOP_OPEN || state == Proto.COOP_JOINED) {
			failedAtMs = 0;
		}
		if (state == Proto.COOP_JOINED && wasState != Proto.COOP_JOINED) {
			HalfCraft.LOG.info("HalfCraft: in the host's world at {}", remote);
		}

		// the resource loading overlay at startup: nothing until it has gone, as MirrorWorld does
		if (minecraft.gui.overlay() != null) {
			return;
		}
		switch (CoopPlan.next(wanted, now, nowMs)) {
			case OPEN -> open(minecraft, server, wanted.port(), nowMs);
			case CLOSE -> {
				HalfCraft.LOG.info("HalfCraft: closing the world to LAN on port {} (Half-Life is {})", now.publishedPort(), mode());
				server.unpublishServer();
			}
			case LEAVE -> leave(minecraft, now);
			case JOIN -> join(minecraft, target());
			case NONE, WAIT -> {
			}
		}
	}

	/** McState's coopState: one of Proto's COOP_IDLE to COOP_FAILED. */
	public static int state() {
		return state;
	}

	/** McState's coopPort: the port the own world is open to LAN on while it's COOP_OPEN, else 0. */
	public static int publishedPort() {
		return publishedPort;
	}

	/** False while Half-Life is a guest: Minecraft plays in the host's world, and opens none of its own. */
	public static boolean wantsOwnWorld() {
		return CoopPlan.wantsOwnWorld(wanted);
	}

	/** In the host's world as a guest. */
	public static boolean joined() {
		return state == Proto.COOP_JOINED;
	}

	private static void open(Minecraft minecraft, IntegratedServer server, int port, long nowMs) {
		if (minecraft.getConnection() == null) {
			return; // the world is still loading: publishServer needs the player's connection
		}
		boolean development = FabricLoader.getInstance().isDevelopmentEnvironment();
		if (development) {
			// a LAN world checks every joiner's account, and a development client's offline name has none.
			// a release (production Fabric) keeps vanilla's online mode
			server.setUsesAuthentication(false);
			HalfCraft.LOG.info("HalfCraft: a development client: the LAN world takes offline accounts");
		}
		boolean open;
		try {
			// guests get commands only in a development client
			open = server.publishServer(MinecraftServer.MultiplayerScope.LAN, development, port);
		} catch (RuntimeException e) {
			HalfCraft.LOG.warn("HalfCraft: opening the world to LAN threw", e);
			open = false;
		}
		if (open) {
			HalfCraft.LOG.info("HalfCraft: world open to LAN on port {}", port);
			failedAtMs = 0;
			return;
		}
		// publishServer swallows its IOException: a socket of our own on the port says why
		HalfCraft.LOG.warn("HalfCraft: port {}: {}", port, whyNotOpen(port));
		HalfCraft.LOG.warn("HalfCraft: couldn't open the world to LAN on port {}; trying again in {} s", port, RETRY_S);
		failedAtMs = nowMs;
	}

	private static String whyNotOpen(int port) {
		try (ServerSocket probe = new ServerSocket()) {
			probe.bind(new InetSocketAddress(port));
			return "free now, so not taken: Minecraft's log may say more";
		} catch (IOException | RuntimeException e) {
			return "taken or not allowed (" + e.getMessage() + ")";
		}
	}

	/** Out of the world Minecraft is in, or out of the join under way, onto the title screen. */
	private static void leave(Minecraft minecraft, CoopPlan.Now now) {
		String left = now.remote() != null ? "the world at " + now.remote()
			: now.connecting() ? "the join to " + (joining != null ? joining : "a server")
			: "its own world";
		HalfCraft.LOG.info("HalfCraft: leaving {} ({})", left, "Half-Life is " + mode());
		if (minecraft.gui.screen() instanceof ConnectScreen connect) {
			cancel(connect);
		} else {
			// the pause menu's Disconnect (Save and Quit in the own world)
			minecraft.disconnectFromWorld(ClientLevel.DEFAULT_QUIT_MESSAGE);
		}
		joining = null;
		minecraft.gui.setScreen(new TitleScreen());
		// MirrorWorld opens the own world once and only a disconnect makes it try again: a solo player
		// back from the host's world (or one whose host quit) needs it again
		MirrorWorld.reset();
	}

	/** The connect screen's Cancel button (its only one): it stops the connection and goes back. */
	private static void cancel(ConnectScreen connect) {
		for (GuiEventListener child : connect.children()) {
			if (child instanceof Button button) {
				button.onPress(new KeyEvent(InputConstants.KEY_RETURN, 0, 0));
				return;
			}
		}
		HalfCraft.LOG.warn("HalfCraft: the connect screen has no Cancel button; its connection runs on until it ends");
	}

	private static void join(Minecraft minecraft, String address) {
		HalfCraft.LOG.info("HalfCraft: joining the host's world at {}", address);
		joining = address;
		ServerData data = new ServerData("HalfCraft host", address, ServerData.Type.LAN);
		ConnectScreen.startConnecting(new TitleScreen(), minecraft, ServerAddress.parseString(address), data, false, null);
	}

	/** The address of the server Minecraft is in when it isn't its own world, else null. */
	private static @Nullable String remote(Minecraft minecraft) {
		if (minecraft.isLocalServer()) {
			return null;
		}
		ServerData data = minecraft.getCurrentServer();
		return data == null ? null : data.ip;
	}

	private static String target() {
		return CoopPlan.address(wanted.ip(), wanted.port());
	}

	private static String mode() {
		return switch (wanted.mode()) {
			case Proto.COOP_HOST -> "host, on port " + wanted.port();
			case Proto.COOP_GUEST -> wanted.port() == 0 ? "guest of " + CoopPlan.dotted(wanted.ip()) + ", its world not open yet" : "guest of " + target();
			default -> "solo";
		};
	}
}
