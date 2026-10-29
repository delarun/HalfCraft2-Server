package dev.halfcraft;

import com.mojang.authlib.GameProfile;
import java.util.List;
import java.util.UUID;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.Entity;
import org.jspecify.annotations.Nullable;

/**
 * Which of the server's players is the host's, the one Half-Life plays: the integrated server's
 * owner, found by UUID. Anyone else on the server is a guest who joined over the LAN.
 *
 * <p>The first player in the list isn't the host for long: vanilla takes a player who died out of
 * the list and puts the new {@link ServerPlayer} at its end, so after the host has died once a
 * guest comes first.
 */
public final class HostPlayers {
	private HostPlayers() {
	}

	/**
	 * The host's player, wherever it is in the player list; null on a dedicated server (nobody owns
	 * it) and before the host is in (or in the middle of a respawn, which has it out of the list).
	 * Server thread.
	 */
	public static @Nullable ServerPlayer host(MinecraftServer server) {
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		int index = hostIndex(players.stream().map(Entity::getUUID).toList(), owner(server));
		return index < 0 ? null : players.get(index);
	}

	/**
	 * Whether this player is the host's. By UUID, not by place in the player list: vanilla teleports a
	 * player to its spot before the list has it (a respawn takes the old player out, teleports the new
	 * one, and only then adds it; a login teleports before adding too), and those teleports are the
	 * host's.
	 */
	public static boolean isHost(ServerPlayer player) {
		UUID owner = owner(player.level().getServer());
		return owner != null && owner.equals(player.getUUID());
	}

	/** The owner's index in the list of the players' ids, or -1 (no owner, or not in yet). */
	static int hostIndex(List<UUID> players, @Nullable UUID owner) {
		return owner == null ? -1 : players.indexOf(owner);
	}

	/** The integrated server's owner (the player whose Minecraft it is); null on a dedicated server. */
	private static @Nullable UUID owner(MinecraftServer server) {
		GameProfile profile = server.getSingleplayerProfile();
		return profile == null ? null : profile.id();
	}
}
