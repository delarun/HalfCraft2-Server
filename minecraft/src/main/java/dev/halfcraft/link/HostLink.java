package dev.halfcraft.link;

import static dev.halfcraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.halfcraft.HalfCraft;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.VarHandle;

/**
 * The Minecraft end of the shared-memory link. Half-Life owns the mapping; we open it when it
 * appears and treat it as gone when Half-Life's heartbeat stops. Which link is HALFCRAFT_LINK's
 * (LinkNames): the default one without it.
 */
public final class HostLink {
	// Half-Life's loading screens can stall its heartbeat for several seconds.
	private static final long HEARTBEAT_TIMEOUT_MS = 8000;

	private static final VarHandle INT = JAVA_INT.varHandle();
	private static final VarHandle LONG = JAVA_LONG.varHandle();

	// the operating system's side of the link: windows' named objects or linux's /dev/shm and futex
	private static final LinkSystem SYSTEM = LinkSystem.current();
	// the link this Minecraft is on (HALFCRAFT_LINK's); null for an id that isn't one: no Half-Life then, rather than another pair's
	private static final LinkNames LINK = linkFromEnvironment();

	private static LinkNames linkFromEnvironment() {
		String id = System.getenv(LinkNames.VARIABLE);
		LinkNames link = LinkNames.of(id).orElse(null);
		if (link == null) {
			HalfCraft.LOG.error("HalfCraft: {} \"{}\" isn't a link id (1 to {} of A-Z, a-z, 0-9, _ and -): linking to no Half-Life rather than take the default link, another pair's",
				LinkNames.VARIABLE, id, LinkNames.MAX_ID_LENGTH);
		} else if (!link.id().isEmpty()) {
			HalfCraft.LOG.info("HalfCraft: on link {} ({})", link.id(), link.mapping());
		}
		return link;
	}

	/**
	 * Sleeps until Half-Life finishes a frame (it marks each after its HostState: windows' frame event, linux's
	 * futex) or ms pass. False with nothing to wait on (a Half-Life before it): the caller naps and polls instead.
	 */
	public static boolean waitForHostFrame(int ms) {
		return SYSTEM.waitForHostFrame(shm, ms);
	}

	/**
	 * Marks this Minecraft as running on its link for as long as it runs (windows: the named mutex
	 * "&lt;its mapping&gt;_minecraft"; linux: a lock on that name's file in /dev/shm), so a launcher knows not to
	 * start another one on it (even before the two games have linked up).
	 */
	public static void announceRunning() {
		if (LINK != null) {
			SYSTEM.announceRunning(LINK);
		}
	}

	private static volatile MemorySegment shm;
	private static long lastOpenAttempt;
	private static int hostPid;
	private static long hostSession;
	private static volatile int generation;

	private HostLink() {
	}

	/** True when a live Half-Life is on the other end. Cheap; safe from any thread. */
	public static boolean active() {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long beat = (long) LONG.getAcquire(s, OFF_HEADER + H_HOST_HEARTBEAT);
		return tickCount() - beat < HEARTBEAT_TIMEOUT_MS;
	}

	/** Bumps whenever a (new) Half-Life instance is on the other end: everything Half-Life caches must be resent. */
	public static int generation() {
		return generation;
	}

	/** Process id of the Half-Life we're linked to (0 before the first link). */
	public static int hostPid() {
		return hostPid;
	}

	/** The mapping if it has been opened (whether or not Half-Life is still alive). */
	public static MemorySegment segment() {
		return shm;
	}

	/** Try to open the mapping at most once a second. Call regularly from the render thread. */
	public static void poll() {
		if (shm != null) {
			LONG.setRelease(shm, OFF_HEADER + H_MC_HEARTBEAT, tickCount());
			long session = (long) LONG.getAcquire(shm, OFF_HEADER + H_HOST_SESSION);
			if (session != hostSession) {
				// Half-Life restarted and reset the shared state; start our side over too. Its session,
				// not its pid: a restarted Half-Life can get the pid of the one before it.
				hostSession = session;
				hostPid = shm.get(JAVA_INT, OFF_HEADER + H_HOST_PID);
				overlayBack = 1;
				HostStrings.reset();
				generation++;
				HalfCraft.LOG.info("HalfCraft: Half-Life instance changed (pid {})", hostPid);
			}
			return;
		}
		long now = System.currentTimeMillis();
		if (now - lastOpenAttempt < 1000 || LINK == null) {
			return;
		}
		lastOpenAttempt = now;
		try {
			MemorySegment seg = SYSTEM.openMapping(LINK);
			if (seg == null) {
				return;
			}
			int magic = seg.get(JAVA_INT, OFF_HEADER + H_MAGIC);
			int layout = seg.get(JAVA_INT, OFF_HEADER + H_LAYOUT);
			if (magic != MAGIC || layout != LAYOUT) {
				HalfCraft.LOG.error("HalfCraft: Half-Life's shared memory has another layout ({}, magic {}) than this mod's ({}): build both from the same commit",
					Integer.toHexString(layout), Integer.toHexString(magic), Integer.toHexString(LAYOUT));
				return;
			}
			seg.set(JAVA_INT, OFF_HEADER + H_MC_PID, SYSTEM.pid());
			LONG.setRelease(seg, OFF_HEADER + H_MC_HEARTBEAT, tickCount());
			hostPid = seg.get(JAVA_INT, OFF_HEADER + H_HOST_PID);
			hostSession = (long) LONG.getAcquire(seg, OFF_HEADER + H_HOST_SESSION);
			generation++;
			shm = seg;
			HalfCraft.LOG.info("HalfCraft: linked to Half-Life (pid {}){}", seg.get(JAVA_INT, OFF_HEADER + H_HOST_PID),
				LINK.id().isEmpty() ? "" : " on link " + LINK.id() + " (" + LINK.mapping() + ")");
		} catch (Throwable t) {
			HalfCraft.LOG.error("HalfCraft: failed to open shared memory", t);
		}
	}

	/** The client's own Half-Life's collision ring, for HostCollision. */
	public static final dev.halfcraft.world.CollisionSource COLLISION = new dev.halfcraft.world.CollisionSource() {
		@Override
		public MemorySegment segment() {
			return shm;
		}

		@Override
		public long head() {
			return collisionHead();
		}

		@Override
		public long tail() {
			return collisionTail();
		}

		@Override
		public void setTail(long tail) {
			setCollisionTail(tail);
		}

		@Override
		public void lost() {
			pushEvent(EV_COLLISION_LOST, 0, 0.0F, 0.0F, 0.0F, 0.0F, 0);
		}
	};

	/** The high-resolution clock Half-Life reads too (QueryPerformanceCounter, linux: CLOCK_MONOTONIC ns), so tick timestamps line up across processes. */
	public static long qpc() {
		return SYSTEM.qpc();
	}

	/** Ticks per second of {@link #qpc()}. */
	public static long qpcFrequency() {
		return SYSTEM.qpcFrequency();
	}

	/** Milliseconds on the clock both sides stamp their heartbeats with (GetTickCount64, linux: CLOCK_MONOTONIC). */
	public static long tickCount() {
		return SYSTEM.tickCount();
	}

	// ---- HostState (read) -------------------------------------------------------------------

	/** Plain snapshot of HostState, and the sequence number it was read at. */
	public static final class HostState extends ProtoStructs.HostState {
		public int seq;

		public boolean inGame() {
			return (this.flags & HOST_IN_GAME) != 0;
		}

		public boolean menuOpen() {
			return (this.flags & HOST_MENU_OPEN) != 0;
		}

		public boolean loading() {
			return (this.flags & HOST_LOADING) != 0;
		}

		/** Half-Life moves its player itself (a ladder, a lift, a vehicle): Minecraft's player goes where it is. */
		public boolean takeover() {
			return (this.flags & HOST_TAKEOVER) != 0;
		}

		/** In a Half-Life vehicle's seat, which faces {@link #seatYaw}. */
		public boolean seated() {
			return (this.flags & HOST_SEATED) != 0;
		}

		/**
		 * Half-Life lets no new cell of its map be dug (hc_dig 0, or no HalfCraft shader DLL): what was dug
		 * stays dug.
		 */
		public boolean digOff() {
			return (this.flags & HOST_DIG_OFF) != 0;
		}

		/**
		 * How much smaller than Half-Life's viewport the overlay is drawn (hc_overlay_scale), its gui scale
		 * alike; Half-Life scales it back up. 1 from a Half-Life before it.
		 */
		public int divisor() {
			return Math.max(1, this.overlayDivisor);
		}
	}

	/** Half-Life's water surface around the player (see WaterGrid in the protocol). */
	public static final class WaterGrid extends ProtoStructs.WaterGrid {
		public final int size = WATER_GRID_SIZE;
	}

	/** A consistent copy of the water grid, or null (no link, or Half-Life mid-write). */
	public static WaterGrid readWaterGrid() {
		MemorySegment s = shm;
		if (s == null) {
			return null;
		}
		long b = OFF_WATER_GRID;
		WaterGrid out = new WaterGrid();
		for (int attempt = 0; attempt < 100; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + WG_SEQ);
			if ((seq1 & 1) != 0 || seq1 == 0) {
				Thread.onSpinWait();
				if (seq1 == 0) {
					return null; // Half-Life hasn't written one yet
				}
				continue;
			}
			out.read(s, b);
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + WG_SEQ) == seq1) {
				return out;
			}
		}
		return null;
	}

	public static boolean readHostState(HostState out) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long b = OFF_HOST_STATE;
		for (int attempt = 0; attempt < 1000; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + HS_SEQ);
			if ((seq1 & 1) != 0) {
				if (attempt > 100) {
					Thread.yield();
				} else {
					Thread.onSpinWait();
				}
				continue;
			}
			out.read(s, b);
			VarHandle.loadLoadFence();
			int seq2 = (int) INT.getAcquire(s, b + HS_SEQ);
			if (seq1 == seq2) {
				out.seq = seq1;
				return true;
			}
		}
		return false;
	}

	/** Raw HostState sequence number; changes once per Half-Life frame. */
	public static int hostStateSeq() {
		MemorySegment s = shm;
		return s == null ? 0 : (int) INT.getAcquire(s, OFF_HOST_STATE + HS_SEQ);
	}

	// ---- McState (write) -------------------------------------------------------------------

	public static final class McState extends ProtoStructs.McState {
		public McState() {
			this.tickMs = 50.0F;
		}
	}

	public static void writeMcState(McState st) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long b = OFF_MC_STATE;
		int seq = s.get(JAVA_INT, b + MS_SEQ);
		INT.setRelease(s, b + MS_SEQ, seq + 1);
		VarHandle.storeStoreFence();
		st.write(s, b);
		INT.setRelease(s, b + MS_SEQ, seq + 2);
	}

	// ---- input ring (consume) --------------------------------------------------------------

	public interface InputSink {
		void accept(int type, int code, int a, int b, int c);
	}

	/** Drains every pending input event. Render thread only. */
	public static void drainInput(InputSink sink) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long base = OFF_INPUT_RING;
		long head = (long) LONG.getAcquire(s, base + IR_HEAD);
		long tail = s.get(JAVA_LONG, base + IR_TAIL);
		if (tail > head) {
			HalfCraft.LOG.warn("HalfCraft: input ring out of step (written up to {}, read up to {}): resyncing", head, tail);
			tail = head; // read past what was written: wait at Half-Life's head from now on
		} else if (head - tail > INPUT_RING_ENTRIES) {
			tail = head - INPUT_RING_ENTRIES; // producer lapped us; drop the oldest
		}
		while (tail < head) {
			ProtoStructs.InputEvent event = ProtoStructs.InputEvent.read(s, base + IR_DATA + (tail & (INPUT_RING_ENTRIES - 1)) * INPUT_EVENT_BYTES);
			tail++;
			sink.accept(event.type(), event.code(), event.a(), event.b(), event.c());
		}
		LONG.setRelease(s, base + IR_TAIL, tail);
	}

	// ---- actor table (read) ----------------------------------------------------------------

	/** What one nearby Half-Life actor's flags say (ProtoStructs.ActorRecord, the actor table's records, has it). */
	public interface Actor {
		int flags();

		default boolean dead() {
			return (this.flags() & ACTOR_DEAD) != 0;
		}

		default boolean hostile() {
			return (this.flags() & ACTOR_HOSTILE) != 0;
		}
	}

	/** Seqlock read of the actor table. Returns false (leaving {@code out} empty) on a torn read. */
	public static boolean readActors(java.util.List<ProtoStructs.ActorRecord> out) {
		return readActors(shm, out);
	}

	/** Seqlock read of a mapping's actor table (this link's or a server link's). */
	static boolean readActors(MemorySegment s, java.util.List<ProtoStructs.ActorRecord> out) {
		out.clear();
		if (s == null) {
			return false;
		}
		long b = OFF_ACTOR_TABLE;
		for (int attempt = 0; attempt < 16; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + AT_SEQ);
			if ((seq1 & 1) != 0) {
				Thread.onSpinWait();
				continue;
			}
			int count = Math.min(s.get(JAVA_INT, b + AT_COUNT), MAX_ACTORS);
			for (int i = 0; i < count; i++) {
				out.add(ProtoStructs.ActorRecord.read(s, b + AT_RECORDS + i * ACTOR_RECORD_BYTES));
			}
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + AT_SEQ) == seq1) {
				return true;
			}
			out.clear();
		}
		return false;
	}

	// ---- event ring (produce) --------------------------------------------------------------

	/** Queues an event for Half-Life. Safe from any thread. Drops the event if Half-Life is a full ring behind. */
	public static void pushEvent(int type, int actorId, float a, float b, float c, float d, int flags) {
		pushEvent(type, actorId, a, b, c, d, flags, 0);
	}

	public static void pushEvent(int type, int actorId, float a, float b, float c, float d, int flags, int weapon) {
		pushEvent(type, actorId, a, b, c, d, flags, weapon, 0);
	}

	public static synchronized void pushEvent(int type, int actorId, float a, float b, float c, float d, int flags, int weapon, int attackerId) {
		MemorySegment s = shm;
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

	// ---- world entities (write) ------------------------------------------------------------

	/**
	 * Seqlock write of the world-entity table and block selection. Render thread only. An entity's
	 * {@code uv} holds up to three atlas rects {u0, v0, u1, v1}: sprite/side, top, bottom.
	 */
	public static void writeWorldEntities(java.util.List<ProtoStructs.WorldEntity> entities, float[] selection) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long b = OFF_WORLD_ENTITIES;
		int seq = s.get(JAVA_INT, b + WE_SEQ);
		INT.setRelease(s, b + WE_SEQ, seq + 1);
		VarHandle.storeStoreFence();
		int count = Math.min(entities.size(), MAX_WORLD_ENTITIES);
		s.set(JAVA_INT, b + WE_COUNT, count);
		s.set(JAVA_INT, b + WE_HAS_SELECTION, selection != null ? 1 : 0);
		if (selection != null) {
			for (int i = 0; i < 6; i++) {
				s.set(JAVA_FLOAT, b + WE_SEL_MIN + i * 4L, selection[i]);
			}
		}
		for (int i = 0; i < count; i++) {
			entities.get(i).write(s, b + WE_RECORDS + i * WORLD_ENTITY_BYTES);
		}
		INT.setRelease(s, b + WE_SEQ, seq + 2);
	}

	// ---- render ring (produce) -------------------------------------------------------------

	/**
	 * Writes one render message ({@code header} bytes then {@code body} bytes) into the render ring,
	 * waiting briefly for space. Single producer. Returns false if it never fit.
	 */
	public static boolean writeRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body) {
		return writeRender(type, header, body, 500);
	}

	/** Like writeRender, but gives up at once if the ring is full (per-frame data that the next frame replaces). */
	public static boolean tryWriteRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body) {
		return writeRender(type, header, body, 1);
	}

	private static synchronized boolean writeRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body, int attempts) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		int payload = header.remaining() + (body != null ? body.remaining() : 0);
		long msgBytes = (8 + payload + 7) & ~7L;
		if (msgBytes > RR_DATA_BYTES / 2) {
			HalfCraft.LOG.warn("HalfCraft: render message too large ({} bytes)", msgBytes);
			return false;
		}
		long base = OFF_RENDER_RING;
		for (int attempt = 0; attempt < attempts; attempt++) {
			long head = s.get(JAVA_LONG, base + RR_HEAD);
			long tail = (long) LONG.getAcquire(s, base + RR_TAIL);
			long pos = head % RR_DATA_BYTES;
			long pad = pos + msgBytes > RR_DATA_BYTES ? RR_DATA_BYTES - pos : 0;
			if (RR_DATA_BYTES - (head - tail) < msgBytes + pad) {
				if (attempt + 1 >= attempts) {
					break;
				}
				try {
					Thread.sleep(2);
				} catch (InterruptedException e) {
					return false;
				}
				continue;
			}
			if (pad > 0) {
				s.set(JAVA_INT, base + RR_DATA + pos, REN_PAD);
				s.set(JAVA_INT, base + RR_DATA + pos + 4, 0);
				head += pad;
				pos = 0;
			}
			long at = base + RR_DATA + pos;
			s.set(JAVA_INT, at, type);
			s.set(JAVA_INT, at + 4, payload);
			MemorySegment.copy(MemorySegment.ofBuffer(header), 0, s, at + 8, header.remaining());
			if (body != null && body.remaining() > 0) {
				MemorySegment.copy(MemorySegment.ofBuffer(body), 0, s, at + 8 + header.remaining(), body.remaining());
			}
			LONG.setRelease(s, base + RR_HEAD, head + msgBytes);
			return true;
		}
		return false;
	}

	// ---- overlay (publish) -----------------------------------------------------------------

	private static int overlayBack = 1; // writer's private slot; Half-Life's front starts at 2, middle at 0

	/** Returns the byte offset where the next overlay frame should be written. */
	public static long overlayBackSlotOffset() {
		return OFF_OVERLAY_PIXELS + overlayBack * OVERLAY_SLOT_BYTES;
	}

	/** Publishes the frame just written into the back slot. */
	public static void publishOverlay(int width, int height, boolean bottomUp, long frameId) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		new ProtoStructs.OverlaySlotHdr(width, height, bottomUp ? 1 : 0, frameId).write(s, OFF_OVERLAY_SLOT_HDR + overlayBack * SLOT_HDR_SIZE);
		int old = (int) INT.getAndSet(s, OFF_OVERLAY_CTL + OC_STATE, overlayBack | OVERLAY_DIRTY);
		overlayBack = old & 3;
		LONG.getAndAdd(s, OFF_OVERLAY_CTL + OC_FRAMES_PUBLISHED, 1L);
	}

	// ---- collision ring (consume) ----------------------------------------------------------

	public static long collisionHead() {
		MemorySegment s = shm;
		return s == null ? 0 : (long) LONG.getAcquire(s, OFF_COLLISION_RING + CR_HEAD);
	}

	public static long collisionTail() {
		MemorySegment s = shm;
		return s == null ? 0 : s.get(JAVA_LONG, OFF_COLLISION_RING + CR_TAIL);
	}

	public static void setCollisionTail(long tail) {
		MemorySegment s = shm;
		if (s != null) {
			LONG.setRelease(s, OFF_COLLISION_RING + CR_TAIL, tail);
		}
	}
}
