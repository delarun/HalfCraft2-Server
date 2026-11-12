package dev.halfcraft.link;

import static dev.halfcraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.world.CollisionSource;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.VarHandle;
import org.jspecify.annotations.Nullable;

/**
 * The HalfCraft server's link to one Source dedicated server's server.so (docs/server/PLAN.md): server.so makes
 * the mapping on its link id (srcds -hc_link, srv by default) and keeps its heartbeat; this Minecraft server opens
 * it, keeps its own heartbeat in it and reads the server's regions (the collision ring first). One per map's
 * server. HostLink is the client's link to its own Half-Life; this is the same mapping's server half.
 */
public final class ServerLink implements CollisionSource {
	// a map's load can stall srcds's frames for a while, as Half-Life's loading screens stall its heartbeat
	private static final long HEARTBEAT_TIMEOUT_MS = 8000;
	private static final VarHandle LONG = JAVA_LONG.varHandle();
	private static final LinkSystem SYSTEM = LinkSystem.current();

	private final LinkNames names;
	private volatile @Nullable MemorySegment shm;
	private long lastOpenAttempt;
	private long hostSession;
	private boolean saidLayout;

	public ServerLink(LinkNames names) {
		this.names = names;
	}

	/** The link of an id, or null (logged) for an id that isn't one. */
	public static @Nullable ServerLink of(String id) {
		LinkNames names = LinkNames.of(id).orElse(null);
		if (names == null || id.isEmpty()) {
			HalfCraft.LOG.error("HalfCraft: \"{}\" isn't a server link id (1 to {} of A-Z, a-z, 0-9, _ and -)", id, LinkNames.MAX_ID_LENGTH);
			return null;
		}
		return new ServerLink(names);
	}

	public String id() {
		return this.names.id();
	}

	/** Opens the mapping when server.so has made it (at most once a second), and keeps this side's heartbeat. Server thread. */
	public void tick() {
		MemorySegment s = this.shm;
		if (s != null) {
			LONG.setRelease(s, OFF_HEADER + H_MC_HEARTBEAT, SYSTEM.tickCount());
			long session = (long) LONG.getAcquire(s, OFF_HEADER + H_HOST_SESSION);
			if (session != this.hostSession) {
				this.hostSession = session;
				HalfCraft.LOG.info("HalfCraft: server link {}: source server restarted (pid {})", this.id(), s.get(JAVA_INT, OFF_HEADER + H_HOST_PID));
			}
			return;
		}
		long now = System.currentTimeMillis();
		if (now - this.lastOpenAttempt < 1000) {
			return;
		}
		this.lastOpenAttempt = now;
		MemorySegment seg = SYSTEM.openMapping(this.names);
		if (seg == null) {
			return;
		}
		int magic = seg.get(JAVA_INT, OFF_HEADER + H_MAGIC);
		int layout = seg.get(JAVA_INT, OFF_HEADER + H_LAYOUT);
		if (magic != MAGIC || layout != LAYOUT) {
			if (!this.saidLayout) {
				this.saidLayout = true;
				HalfCraft.LOG.error("HalfCraft: server link {} has another layout ({}, magic {}) than this mod's ({}): build both from the same commit", this.id(),
					Integer.toHexString(layout), Integer.toHexString(magic), Integer.toHexString(LAYOUT));
			}
			return;
		}
		seg.set(JAVA_INT, OFF_HEADER + H_MC_PID, SYSTEM.pid());
		LONG.setRelease(seg, OFF_HEADER + H_MC_HEARTBEAT, SYSTEM.tickCount());
		this.hostSession = (long) LONG.getAcquire(seg, OFF_HEADER + H_HOST_SESSION);
		this.shm = seg;
		HalfCraft.LOG.info("HalfCraft: server link {} up: source server pid {}", this.id(), seg.get(JAVA_INT, OFF_HEADER + H_HOST_PID));
	}

	/** True while the source server's heartbeat is fresh. Any thread. */
	public boolean active() {
		MemorySegment s = this.shm;
		return s != null && SYSTEM.tickCount() - (long) LONG.getAcquire(s, OFF_HEADER + H_HOST_HEARTBEAT) < HEARTBEAT_TIMEOUT_MS;
	}

	/**
	 * A render message for server.so (the HalfCraft server's dug cells, kRenDug): on a server link nothing else uses
	 * the render ring, which server.so drains on a dedicated server as client.dll does on a player's machine.
	 */
	public synchronized boolean writeRender(int type, java.nio.ByteBuffer header, java.nio.@Nullable ByteBuffer body) {
		return HostLink.writeRender(this.shm, type, header, body, 1);
	}

	/** server.so's session: a new one each time a source server sets the link up (0 before it's open). */
	public long session() {
		return this.hostSession;
	}

	/** Drains server.so's input ring: hurts and heals for the server's players (kInFor names which). Server thread. */
	public void drainInput(HostLink.InputSink sink) {
		HostLink.drainInput(this.shm, sink);
	}

	/** One player's Minecraft health for server.so (PlayerVitals). */
	public record Vitals(int userId, int flags, float health, float maxHealth, float absorption) {
	}

	/** Seqlock write of the players' Minecraft health, which server.so mirrors onto their Source players. Server thread. */
	public void writePlayerVitals(java.util.List<Vitals> players) {
		MemorySegment s = this.shm;
		if (s == null) {
			return;
		}
		long b = OFF_PLAYER_VITALS;
		int seq = s.get(JAVA_INT, b + PV_SEQ);
		s.set(JAVA_INT, b + PV_SEQ, seq + 1);
		VarHandle.storeStoreFence();
		int count = Math.min(players.size(), MAX_SERVER_PLAYERS);
		s.set(JAVA_INT, b + PV_COUNT, count);
		for (int i = 0; i < count; i++) {
			Vitals v = players.get(i);
			long r = b + PV_RECORDS + (long) i * PV_RECORD_BYTES;
			s.set(JAVA_INT, r + PV_R_USER_ID, v.userId());
			s.set(JAVA_INT, r + PV_R_FLAGS, v.flags());
			s.set(JAVA_FLOAT, r + PV_R_HEALTH, v.health());
			s.set(JAVA_FLOAT, r + PV_R_MAX_HEALTH, v.maxHealth());
			s.set(JAVA_FLOAT, r + PV_R_ABSORPTION, v.absorption());
		}
		VarHandle.releaseFence();
		s.set(JAVA_INT, b + PV_SEQ, seq + 2);
	}

	/** Seqlock read of server.so's actor table: the npcs and breakables near its players. */
	public boolean readActors(java.util.List<ProtoStructs.ActorRecord> out) {
		return HostLink.readActors(this.shm, out);
	}

	@Override
	public @Nullable MemorySegment segment() {
		return this.shm;
	}

	@Override
	public long head() {
		MemorySegment s = this.shm;
		return s == null ? 0 : (long) LONG.getAcquire(s, OFF_COLLISION_RING + CR_HEAD);
	}

	@Override
	public long tail() {
		MemorySegment s = this.shm;
		return s == null ? 0 : s.get(JAVA_LONG, OFF_COLLISION_RING + CR_TAIL);
	}

	@Override
	public void setTail(long tail) {
		MemorySegment s = this.shm;
		if (s != null) {
			LONG.setRelease(s, OFF_COLLISION_RING + CR_TAIL, tail);
		}
	}

	@Override
	public void lost() {
		this.pushEvent(EV_COLLISION_LOST, 0, 0.0F, 0.0F, 0.0F, 0.0F, 0, 0, 0);
	}

	/** Queues an event for server.so, as HostLink.pushEvent does for a client's Half-Life. Any thread. */
	public synchronized void pushEvent(int type, int actorId, float a, float b, float c, float d, int flags, int weapon, int attackerId) {
		MemorySegment s = this.shm;
		if (s == null) {
			return;
		}
		long base = OFF_EVENT_RING;
		long head = s.get(JAVA_LONG, base + ER_HEAD);
		long tail = (long) LONG.getAcquire(s, base + ER_TAIL);
		if (head - tail >= EVENT_RING_ENTRIES) {
			return;
		}
		long e = base + ER_DATA + (head & (EVENT_RING_ENTRIES - 1)) * EVENT_BYTES;
		new ProtoStructs.McEvent(type, actorId, a, b, c, d, flags, weapon, attackerId).write(s, e);
		LONG.setRelease(s, base + ER_HEAD, head + 1);
	}
}
