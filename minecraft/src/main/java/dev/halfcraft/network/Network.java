package dev.halfcraft.network;

import com.google.gson.Gson;
import com.google.gson.JsonObject;
import com.google.gson.reflect.TypeToken;
import com.mojang.brigadier.CommandDispatcher;
import dev.halfcraft.HalfCraft;
import io.netty.buffer.ByteBuf;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.commands.CommandSourceStack;
import net.minecraft.commands.Commands;
import net.minecraft.network.chat.Component;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import org.jspecify.annotations.Nullable;

/**
 * The HalfCraft server's network (docs/server/PLAN.md, S3): a hub and every Half-Life 2 map, each a pair of a Source
 * server and a Minecraft server (tools/server/network.py starts them). This Minecraft server is one pair's
 * (HALFCRAFT_PAIR), and HALFCRAFT_NETWORK names the network's description. A player goes to another pair by their
 * Half-Life connecting to its Source server ({@link ConnectPayload}, Half-Life's kRenConnect); their Minecraft follows
 * on its own, to the Minecraft port that server names. The pairs share their players' data, and the homes
 * (/sethome, /home) and the teleports a player takes on to the next pair, in the network's folder.
 */
public final class Network {
	/** One pair: its name, its map, its chapter ("" for the hub) and its ports. */
	public record Pair(String name, String map, String chapter, int srcds, int minecraft) {
	}

	/** A place on a pair's map: where a home is, or where a player goes once they're there. */
	public record Place(String pair, double x, double y, double z, float yaw, float pitch) {
	}

	/**
	 * The server tells a player's Minecraft to have its Half-Life connect to another map's server ("ip:port"), and
	 * itself to go to that pair's Minecraft server at once (the same ip, minecraftPort): left to follow Half-Life, it
	 * went solo while Half-Life was between servers, opened a world of its own, and rejoined the old one.
	 */
	public record ConnectPayload(String address, int minecraftPort) implements CustomPacketPayload {
		public static final Type<ConnectPayload> TYPE = new Type<>(Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "connect"));
		public static final StreamCodec<ByteBuf, ConnectPayload> CODEC = StreamCodec.composite(ByteBufCodecs.STRING_UTF8, ConnectPayload::address,
			ByteBufCodecs.VAR_INT, ConnectPayload::minecraftPort, ConnectPayload::new);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** The pair tells a player's Minecraft where the hub is, and whether it's the hub (CoopClient: back there if a map's pair goes down). */
	public record InfoPayload(String hubSource, int hubMinecraftPort, boolean hubHere) implements CustomPacketPayload {
		public static final Type<InfoPayload> TYPE = new Type<>(Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "network"));
		public static final StreamCodec<ByteBuf, InfoPayload> CODEC = StreamCodec.composite(ByteBufCodecs.STRING_UTF8, InfoPayload::hubSource,
			ByteBufCodecs.VAR_INT, InfoPayload::hubMinecraftPort, ByteBufCodecs.BOOL, InfoPayload::hubHere, InfoPayload::new);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	public static final String HUB = "hub";
	private static final Gson GSON = new Gson();
	/** Ticks after a player joins a pair before the place they came for is theirs: Half-Life's spawn teleport first. */
	private static final int ARRIVAL_TICKS = 100;
	/** How often the hub looks for maps' pairs nobody is on, and how long nobody has to be on one before it stops. */
	private static final int IDLE_CHECK_TICKS = 20 * 60, IDLE_MINUTES = 3;

	private static @Nullable Path folder;
	private static String host = "127.0.0.1";
	private static String current = "";
	private static final List<Pair> PAIRS = new ArrayList<>();
	private static final List<String> CHAPTERS = new ArrayList<>();
	/** Players on their way somewhere: no second departure until this one is through. */
	private static final Set<UUID> LEAVING = ConcurrentHashMap.newKeySet();
	/** Players who just came, and the tick their place is theirs. */
	private static final Map<UUID, Integer> ARRIVING = new HashMap<>();
	private static int ticks;

	private Network() {
	}

	/** Both sides: the payload's type. The server (in a network): its commands, the arrivals, and the hub's portals. */
	public static void register() {
		PayloadTypeRegistry.clientboundPlay().register(ConnectPayload.TYPE, ConnectPayload.CODEC);
		PayloadTypeRegistry.clientboundPlay().register(InfoPayload.TYPE, InfoPayload.CODEC);
		load();
		CommandRegistrationCallback.EVENT.register((dispatcher, registry, environment) -> commands(dispatcher));
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> {
			LEAVING.remove(handler.getPlayer().getUUID());
			ARRIVING.put(handler.getPlayer().getUUID(), ticks + ARRIVAL_TICKS);
			Pair hub = pair(HUB);
			if (active() && hub != null && ServerPlayNetworking.canSend(handler.getPlayer(), InfoPayload.TYPE)) {
				ServerPlayNetworking.send(handler.getPlayer(), new InfoPayload(host + ":" + hub.srcds(), hub.minecraft(), HUB.equals(current)));
			}
		});
		net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents.SERVER_STARTED.register(server -> {
			if (active()) {
				// a pair holds no chunks around the world's spawn: memory for the maps' players
				server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "gamerule spawn_chunk_radius 0");
			}
		});
		ServerTickEvents.END_SERVER_TICK.register(Network::tick);
	}

	/** Whether this server is a pair of a network (tools/server/network.py started it). */
	public static boolean active() {
		return folder != null;
	}

	/** This pair's name ("hub" for the hub's). */
	public static String current() {
		return current;
	}

	public static List<Pair> pairs() {
		return PAIRS;
	}

	public static List<String> chapters() {
		return CHAPTERS;
	}

	public static @Nullable Pair pair(String name) {
		for (Pair pair : PAIRS) {
			if (pair.name().equals(name)) {
				return pair;
			}
		}
		return null;
	}

	private static void load() {
		String named = System.getenv("HALFCRAFT_NETWORK");
		String pair = System.getenv("HALFCRAFT_PAIR");
		if (named == null || pair == null) {
			return;
		}
		try {
			JsonObject json = GSON.fromJson(Files.readString(Path.of(named)), JsonObject.class);
			host = json.get("host").getAsString();
			json.getAsJsonArray("chapters").forEach(chapter -> CHAPTERS.add(chapter.getAsString()));
			json.getAsJsonArray("pairs").forEach(each -> {
				JsonObject p = each.getAsJsonObject();
				PAIRS.add(new Pair(p.get("name").getAsString(), p.get("map").getAsString(), p.get("chapter").getAsString(), p.get("srcds").getAsInt(),
					p.get("minecraft").getAsInt()));
			});
			folder = Path.of(named).getParent();
			current = pair;
			HalfCraft.LOG.info("HalfCraft: pair {} of a network of {} ({} on {})", current, PAIRS.size(), named, host);
		} catch (IOException | RuntimeException e) {
			HalfCraft.LOG.error("HalfCraft: can't read the network's description {}", named, e);
		}
	}

	// ---- going to another pair --------------------------------------------------------------------

	/**
	 * Sends a player to another pair: it starts unless it runs (network.py ensure, a while the first time), then their
	 * Half-Life connects to its Source server, and their Minecraft follows. {@code then}, if any, is where they go once
	 * there. Server thread.
	 */
	public static void send(ServerPlayer player, String name, @Nullable Place then) {
		Pair to = pair(name);
		if (to == null || folder == null) {
			player.sendSystemMessage(Component.literal("Нет такой карты: " + name));
			return;
		}
		if (!LEAVING.add(player.getUUID())) {
			return;
		}
		if (then != null) {
			writePlace("pending.json", player.getUUID(), then);
		}
		player.sendSystemMessage(Component.literal("Переход: " + to.map() + (to.chapter().isEmpty() ? "" : " (" + to.chapter() + ")") + "…"));
		MinecraftServer server = player.level().getServer();
		UUID id = player.getUUID();
		Thread.ofVirtual().name("HalfCraft network " + name).start(() -> {
			boolean up = ensure(to);
			server.execute(() -> {
				ServerPlayer still = server.getPlayerList().getPlayer(id);
				if (still == null) {
					LEAVING.remove(id);
					return;
				}
				if (!up) {
					LEAVING.remove(id);
					still.sendSystemMessage(Component.literal("Не получилось запустить " + to.map() + ": попробуй ещё раз"));
					return;
				}
				HalfCraft.LOG.info("HalfCraft: {} goes to {} ({}:{})", still.getName().getString(), to.name(), host, to.srcds());
				ServerPlayNetworking.send(still, new ConnectPayload(host + ":" + to.srcds(), to.minecraft()));
			});
		});
	}

	/** Starts a pair unless it runs, and waits until it's up (tools/server/network.py ensure). Any thread. */
	private static boolean ensure(Pair pair) {
		Path repo = folder.getParent().getParent();
		try {
			Process process = new ProcessBuilder("python3", repo.resolve("tools/server/network.py").toString(), "ensure", pair.name()).directory(repo.toFile())
				.redirectErrorStream(true).redirectOutput(folder.resolve(current).resolve("ensure-" + pair.name() + ".log").toFile()).start();
			return process.waitFor() == 0;
		} catch (IOException e) {
			HalfCraft.LOG.error("HalfCraft: can't start pair {}", pair.name(), e);
			return false;
		} catch (InterruptedException e) {
			Thread.currentThread().interrupt();
			return false;
		}
	}

	private static void tick(MinecraftServer server) {
		ticks++;
		if (!active()) {
			return;
		}
		for (var it = ARRIVING.entrySet().iterator(); it.hasNext();) {
			var arrival = it.next();
			if (ticks < arrival.getValue()) {
				continue;
			}
			it.remove();
			ServerPlayer player = server.getPlayerList().getPlayer(arrival.getKey());
			Place place = takePlace("pending.json", arrival.getKey());
			if (player != null && place != null && place.pair().equals(current)) {
				teleport(player, place);
			}
		}
		if (HUB.equals(current)) {
			HubPortals.tick(server);
			if (ticks % IDLE_CHECK_TICKS == 0) {
				// the maps' pairs nobody has been on for a while stop (network.py idle), off the server thread
				Path repo = folder.getParent().getParent();
				Thread.ofVirtual().name("HalfCraft network idle").start(() -> {
					try {
						new ProcessBuilder("python3", repo.resolve("tools/server/network.py").toString(), "idle", String.valueOf(IDLE_MINUTES)).directory(repo.toFile())
							.redirectErrorStream(true).redirectOutput(ProcessBuilder.Redirect.appendTo(folder.resolve(HUB).resolve("idle.log").toFile())).start().waitFor();
					} catch (IOException e) {
						HalfCraft.LOG.warn("HalfCraft: can't look for idle pairs", e);
					} catch (InterruptedException e) {
						Thread.currentThread().interrupt();
					}
				});
			}
		}
	}

	private static void teleport(ServerPlayer player, Place place) {
		player.teleportTo(player.level(), place.x(), place.y(), place.z(), Set.of(), place.yaw(), place.pitch(), true);
	}

	// ---- homes and places ----------------------------------------------------------------------------

	private static synchronized Map<String, Place> places(String file) {
		try {
			Map<String, Place> read = GSON.fromJson(Files.readString(folder.resolve(file)), new TypeToken<Map<String, Place>>() {
			}.getType());
			return read != null ? read : new HashMap<>();
		} catch (IOException | RuntimeException e) {
			return new HashMap<>();
		}
	}

	private static synchronized void writePlace(String file, UUID player, @Nullable Place place) {
		Map<String, Place> all = places(file);
		if (place == null) {
			all.remove(player.toString());
		} else {
			all.put(player.toString(), place);
		}
		try {
			Path to = folder.resolve(file);
			Path temporary = folder.resolve(file + ".tmp");
			Files.writeString(temporary, GSON.toJson(all), StandardCharsets.UTF_8);
			Files.move(temporary, to, java.nio.file.StandardCopyOption.REPLACE_EXISTING, java.nio.file.StandardCopyOption.ATOMIC_MOVE);
		} catch (IOException e) {
			HalfCraft.LOG.error("HalfCraft: can't write {}", file, e);
		}
	}

	private static @Nullable Place takePlace(String file, UUID player) {
		Place place = places(file).get(player.toString());
		if (place != null) {
			writePlace(file, player, null);
		}
		return place;
	}

	private static Place here(ServerPlayer player) {
		return new Place(current, player.getX(), player.getY(), player.getZ(), player.getYRot(), player.getXRot());
	}

	// ---- commands ----------------------------------------------------------------------------------

	private static void commands(CommandDispatcher<CommandSourceStack> dispatcher) {
		if (!active()) {
			return;
		}
		dispatcher.register(Commands.literal("hub").executes(context -> hub(context.getSource().getPlayerOrException())));
		dispatcher.register(Commands.literal("spawn").executes(context -> hub(context.getSource().getPlayerOrException())));
		dispatcher.register(Commands.literal("hubhere").requires(Commands.hasPermission(Commands.LEVEL_GAMEMASTERS)).executes(context -> {
			ServerPlayer player = context.getSource().getPlayerOrException();
			if (!HUB.equals(current)) {
				player.sendSystemMessage(Component.literal("Это команда хаба"));
				return 0;
			}
			HubPortals.buildHere(player);
			return 1;
		}));
		dispatcher.register(Commands.literal("sethome").executes(context -> {
			ServerPlayer player = context.getSource().getPlayerOrException();
			writePlace("homes.json", player.getUUID(), here(player));
			Pair pair = pair(current);
			player.sendSystemMessage(Component.literal("Дом здесь: " + (pair != null ? pair.map() : current)));
			return 1;
		}));
		dispatcher.register(Commands.literal("home").executes(context -> {
			ServerPlayer player = context.getSource().getPlayerOrException();
			Place home = places("homes.json").get(player.getUUID().toString());
			if (home == null) {
				player.sendSystemMessage(Component.literal("Дома нет: поставь его командой /sethome"));
				return 0;
			}
			if (home.pair().equals(current)) {
				teleport(player, home);
			} else {
				send(player, home.pair(), home);
			}
			return 1;
		}));
	}

	private static int hub(ServerPlayer player) {
		if (HUB.equals(current)) {
			HubPortals.toSpawn(player);
		} else {
			send(player, HUB, null);
		}
		return 1;
	}
}
