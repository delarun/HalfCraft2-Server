package dev.halfcraft.world;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.link.HostLink;
import dev.halfcraft.mobs.HostNav;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;
import net.fabricmc.fabric.api.attachment.v1.AttachmentRegistry;
import net.fabricmc.fabric.api.attachment.v1.AttachmentSyncPredicate;
import net.fabricmc.fabric.api.attachment.v1.AttachmentType;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.CollisionGetter;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.LevelEvent;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

/**
 * Digging into Half-Life's walls. Half-Life's walls, floors and ceilings are geometry, not blocks; mining
 * one opens the cell its surface is in: the cell is dug (Half-Life cuts it out of what it draws and what
 * Minecraft collides with), with the drops of its material's block. Around a dug cell, the cells wholly
 * rock to a tunnel, with no face Half-Life draws, are revealed ({@link DigRules}): dug too, with their
 * material's block put in them, which the rollback leaves alone. The same happens when a block in a dug
 * cell goes, however it goes (a player, TNT, a piston); while Half-Life lets nothing more be dug, only the
 * void beyond the map is revealed. Outside dug cells, that void is solid to Minecraft
 * ({@link #collisionShape}). Which cells are dug is kept per chunk ({@link #DUG}), saved with the world
 * and synced to every player.
 */
public final class HostDig {
	public static final AttachmentType<DugColumn> DUG = AttachmentRegistry.<DugColumn>create(Identifier.fromNamespaceAndPath(HalfCraft.MOD_ID, "dug"),
		builder -> builder.persistent(DugColumn.CODEC).syncWith(DugColumn.STREAM_CODEC, AttachmentSyncPredicate.all()));

	// how far from the player's eyes the middle of a cell they open may be
	private static final double REACH = 8.0;
	// server thread only
	private static final HostLink.HostState HOST = new HostLink.HostState();
	// the reveals still to do, one a cell, in the order they were first queued; server thread only
	private static final Map<Cell, Pending> PENDING = new LinkedHashMap<>();

	/** A dug cell of a level. */
	private record Cell(ServerLevel level, BlockPos pos) {
	}

	/** A dug cell whose neighbours are to be revealed, opened as {@code material}; {@code age}: see {@link RevealRetry}. */
	private record Pending(Cell cell, int material, int age) {
		Pending older() {
			return new Pending(this.cell, this.material, RevealRetry.older(this.age));
		}
	}

	private HostDig() {
	}

	/** Registers the attachment (by loading the class) and the reveals' tick, at mod start. */
	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(HostDig::serverTick);
		// the levels go with the server
		ServerLifecycleEvents.SERVER_STOPPING.register(server -> PENDING.clear());
		HalfCraft.LOG.info("HalfCraft: digging into Half-Life's walls registered ({})", DUG.identifier());
	}

	/** The dug cells of a chunk's section (see {@link DugBits}), or null for none. Never change them. */
	public static long @Nullable [] sectionBits(LevelChunk chunk, int sectionY) {
		return column(chunk).bits(sectionY);
	}

	/** The block a Proto.DIG_* material digs as (stone for none, or one it doesn't know). */
	public static BlockState stateFor(int material) {
		return BuiltInRegistries.BLOCK.getValue(Identifier.parse(DigMaterials.blockId(material))).defaultBlockState();
	}

	private static int materialOf(BlockState state) {
		return DigMaterials.material(BuiltInRegistries.BLOCK.getKey(state.getBlock()).toString());
	}

	private static DugColumn column(LevelChunk chunk) {
		DugColumn column = chunk.getAttached(DUG);
		return column != null ? column : DugColumn.EMPTY;
	}

	/** Whether a loaded cell is dug. */
	public static boolean isDugAt(Level level, BlockPos pos) {
		return isDug(level, pos);
	}

	/**
	 * Fire burnt Half-Life's wood out of this cell (HostFire): dug, with nothing dropped, and the wood around it revealed
	 * as planks, which burn on. False when it can't be: Half-Life lets nothing more be dug, the cell is dug already or a
	 * Minecraft block fills it. Server thread only.
	 */
	public static boolean burn(ServerLevel level, BlockPos pos) {
		if (!HostNav.inMirror(level) || !level.isLoaded(pos) || digOff() || isDug(level, pos) || !level.getBlockState(pos).canBeReplaced()) {
			return false;
		}
		LevelChunk chunk = level.getChunkAt(pos);
		chunk.setAttached(DUG, column(chunk).with(pos.getX(), pos.getY(), pos.getZ()));
		ServerDug.changed(chunk, pos);
		queue(level, pos, dev.halfcraft.link.Proto.DIG_PLANKS);
		if (++burnt % 20 == 1) {
			HalfCraft.LOG.info("HalfCraft: fire burnt Half-Life's wood out of {} ({} cells so far)", pos.toShortString(), burnt);
		}
		return true;
	}

	private static int burnt;

	// loads the cell's chunk: callers check it's loaded first
	private static boolean isDug(Level level, BlockPos pos) {
		return column(level.getChunkAt(pos)).has(pos.getX(), pos.getY(), pos.getZ());
	}

	/**
	 * Half-Life's world in a cell as Minecraft's block collisions take it (BlockCollisionsMixin): with
	 * {@code geometry}, its brushes' voxels (the players colliding with its exact triangles go without),
	 * and, unless the cell is dug, the void beyond the map, solid like the rock it is (see
	 * {@link HostCollision#collisionShapeAt}): a cell kept next to a hole for the face Half-Life draws on it
	 * ({@link DigRules}) leaves no way into the void. On the maps checked (d1_trainstation_02 and 04) no void
	 * meets the map's open space where a player stands, so nothing outside them is reached without
	 * digging. Null for nothing. Any thread a collision runs on: it reads the chunk it's given.
	 *
	 * @param chunk - the cell's chunk (a getter that isn't a chunk or a level has no dug cells)
	 */
	public static @Nullable VoxelShape collisionShape(BlockGetter chunk, BlockPos pos, boolean geometry) {
		return HostCollision.collisionShapeAt(pos.getX(), pos.getY(), pos.getZ(), geometry, chunk, HostDig::dugIn);
	}

	/**
	 * The void beyond the map in a cell as 8 layers (see {@link HostCollision#voidLayersAt}), or null for
	 * none or a dug cell: rock to the water and lava that flow among Half-Life's walls (FlowingFluidMixin).
	 */
	public static long @Nullable [] voidLayers(BlockGetter level, BlockPos pos) {
		int x = pos.getX(), y = pos.getY(), z = pos.getZ();
		long[] voids = HostCollision.voidLayersAt(x, y, z);
		return voids == null || dugIn(level, x, y, z) ? null : voids;
	}

	/** Whether a getter's cell is dug: the chunk's own, or the level's (or a region's) chunk there, if it has one. */
	private static boolean dugIn(BlockGetter getter, int x, int y, int z) {
		BlockGetter chunk = getter instanceof CollisionGetter level ? level.getChunkForCollisions(x >> 4, z >> 4) : getter;
		if (chunk instanceof LevelChunk c && (c.getPos().x() != x >> 4 || c.getPos().z() != z >> 4)) {
			chunk = c.getLevel().getChunkForCollisions(x >> 4, z >> 4); // a neighbour's cell (HostCollision.collisionShapeAt)
		}
		return chunk instanceof LevelChunk c && column(c).has(x, y, z);
	}

	// ---- opening ------------------------------------------------------------------------------

	/**
	 * A player mined Half-Life's wall out of this cell (the one its surface is in), made of
	 * {@code material}: the cell is dug, with the drops and the sound of mining its block, and the rock
	 * around it is revealed. Nothing when the player can't build here or reach it, the cell isn't loaded,
	 * Half-Life lets nothing more be dug, the cell is dug already or a Minecraft block fills it. Server
	 * thread only.
	 */
	public static void open(ServerLevel level, ServerPlayer player, BlockPos pos, int material) {
		if (!HostNav.inMirror(level) || player.isSpectator() || !player.mayBuild() || !level.isLoaded(pos) || !inReach(player, pos) || digOff()
			|| isDug(level, pos) || !level.getBlockState(pos).canBeReplaced()) {
			return;
		}
		LevelChunk chunk = level.getChunkAt(pos);
		chunk.setAttached(DUG, column(chunk).with(pos.getX(), pos.getY(), pos.getZ()));
		ServerDug.changed(chunk, pos);
		BlockState state = stateFor(material);
		if (!player.isCreative()) {
			ItemStack tool = player.getMainHandItem();
			ItemStack used = tool.copy();
			// before mining: the tool may break in it
			boolean harvest = player.hasCorrectToolForDrops(state);
			tool.mineBlock(level, state, pos, player);
			if (harvest) {
				state.getBlock().playerDestroy(level, player, pos, state, null, used);
			}
		}
		level.levelEvent(LevelEvent.PARTICLES_AND_SOUND_DESTROY_BLOCK, pos, Block.getId(state));
		queue(level, pos, material);
		HalfCraft.LOG.info("HalfCraft: dug {} out of Half-Life's walls as {}", pos.toShortString(), DigMaterials.blockId(material));
	}

	/**
	 * A cell of Half-Life's ground becomes Minecraft's own (PlantGround): dug out of Half-Life's collision, and the block
	 * put there (a grass block for a sapling, farmland under a hoe). Server thread.
	 */
	public static void fill(ServerLevel level, BlockPos pos, BlockState state) {
		LevelChunk chunk = level.getChunkAt(pos);
		if (!isDug(level, pos)) {
			chunk.setAttached(DUG, column(chunk).with(pos.getX(), pos.getY(), pos.getZ()));
			ServerDug.changed(chunk, pos);
		}
		level.setBlockAndUpdate(pos, state);
	}

	private static boolean inReach(ServerPlayer player, BlockPos pos) {
		return player.getEyePosition().distanceToSqr(pos.getX() + 0.5, pos.getY() + 0.5, pos.getZ() + 0.5) <= REACH * REACH;
	}

	private static boolean digOff() {
		return HostLink.active() && HostLink.readHostState(HOST) && HOST.digOff();
	}

	// ---- revealing ----------------------------------------------------------------------------

	/**
	 * LevelChunk.setBlockState, after: a block that went from a dug cell (mined, blown up, pushed away)
	 * reveals the rock around it, opened as what it was, on the next tick; while Half-Life lets nothing
	 * more be dug (hc_dig 0, or no shader DLL), only the void beyond the map around it, so the tunnel
	 * can be mined on (see {@link #reveal}). Leaves the world alone.
	 */
	public static void afterChange(LevelChunk chunk, BlockPos pos, @Nullable BlockState previous, BlockState now) {
		if (previous == null || previous.canBeReplaced() || !now.canBeReplaced()) {
			return;
		}
		if (!(chunk.getLevel() instanceof ServerLevel level) || !level.getServer().isSameThread() || !HostNav.inMirror(level)) {
			return;
		}
		if (column(chunk).has(pos.getX(), pos.getY(), pos.getZ())) {
			queue(level, pos, materialOf(previous));
		}
	}

	/** Queues a reveal around a dug cell, opened as {@code material}: it replaces one queued there already, starting over. */
	private static void queue(ServerLevel level, BlockPos pos, int material) {
		Cell cell = new Cell(level, pos.immutable());
		PENDING.put(cell, new Pending(cell, material, 0));
	}

	private static void serverTick(MinecraftServer server) {
		if (!FRONTIER.isEmpty() && server.getTickCount() % FRONTIER_EVERY == 0) {
			// the edges of the caves that stopped short of the players: the ones a player came near open on
			FRONTIER.entrySet().removeIf(e -> {
				Cell cell = e.getKey();
				if (!cell.level().isLoaded(cell.pos())) {
					return true;
				}
				if (!nearPlayer(cell.level(), cell.pos(), CAVE_REACH - 1.0)) {
					return false;
				}
				queue(cell.level(), cell.pos(), e.getValue());
				return true;
			});
		}
		if (PENDING.isEmpty()) {
			return;
		}
		// what revealing queues (a block it puts changes others) waits for the next tick; a cell queued again
		// meanwhile starts over, so that one stays (putIfAbsent) and this one goes. The cell and its sides are
		// looked at only on the ticks a reveal tries
		List<Pending> due = new ArrayList<>(PENDING.values());
		PENDING.clear();
		int budget = REVEALS_PER_TICK;
		for (Pending p : due) {
			if (budget <= 0) {
				PENDING.putIfAbsent(p.cell(), p); // a cave opening wide: the rest on the next ticks
				continue;
			}
			budget--;
			if (!RevealRetry.tries(p.age())) {
				PENDING.putIfAbsent(p.cell(), p.older());
				continue;
			}
			ServerLevel level = p.cell().level();
			BlockPos pos = p.cell().pos();
			boolean loaded = level.isLoaded(pos);
			boolean dug = loaded && isDug(level, pos);
			switch (RevealRetry.step(p.age(), loaded, dug, dug && DigRules.allKnown(cells(level), pos.getX(), pos.getY(), pos.getZ()))) {
				case FAST_TRY, SLOW_WAIT -> PENDING.putIfAbsent(p.cell(), p.older());
				case REVEAL_KEEP -> {
					reveal(level, pos, p.material());
					PENDING.putIfAbsent(p.cell(), p.older());
				}
				case REVEAL_DONE -> reveal(level, pos, p.material());
				case DROP -> {
					// its chunk went, or it isn't dug: nothing to reveal around
				}
			}
		}
	}

	/**
	 * Reveals the rock around a dug cell. While Half-Life lets nothing more be dug, only the cells without
	 * its brushes (void beyond the map): marking one with a brush dug would cut it out of Half-Life's walls,
	 * while a void one becomes a block mined like any other (void has no triangle to mine), so the tunnel
	 * goes on.
	 */
	private static void reveal(ServerLevel level, BlockPos pos, int material) {
		boolean voidOnly = digOff();
		List<DigRules.Reveal> reveals = DigRules.reveals(cells(level), pos.getX(), pos.getY(), pos.getZ(), material, voidOnly);
		int caves = 0;
		boolean waiting = false;
		for (DigRules.Reveal r : reveals) {
			BlockPos cell = new BlockPos(r.x(), r.y(), r.z());
			// natural rock is Minecraft's own underground: the ground's soil near its surface, under it Minecraft's world
			BlockState state = r.natural() ? naturalState(level, r) : stateFor(r.material());
			if (state == null) {
				waiting = true; // its chunk of Minecraft's underground is being generated: revealed on a later try
				continue;
			}
			if (state.isAir() && (!nearPlayer(level, cell, CAVE_REACH) || !DigRules.allKnown(cells(level), r.x(), r.y(), r.z()))) {
				// a cave this far from everyone stays rock for now, and one where Half-Life hasn't described the rock
				// around (opened, its sides would be nothing, neither rock nor block): it opens on as a player comes near
				// (FRONTIER), and the collision streamed around them makes it known
				if (FRONTIER.size() < FRONTIER_MAX) {
					FRONTIER.put(new Cell(level, pos.immutable()), material);
				}
				continue;
			}
			LevelChunk chunk = level.getChunkAt(cell);
			chunk.setAttached(DUG, column(chunk).with(r.x(), r.y(), r.z()));
			ServerDug.changed(chunk, cell);
			if (state.isAir()) {
				// a cave's cell: open, and the rock around it goes on being revealed, through the cave
				caves++;
				queue(level, cell, r.material());
				continue;
			}
			Rollback.untracked(() -> {
				level.setBlock(cell, state, Block.UPDATE_ALL);
				Rollback.forget(cell);
			});
		}
		if (waiting) {
			queue(level, pos, material);
		}
		if (caves > 0 && ++cavesLogged % 50 == 1) {
			HalfCraft.LOG.info("HalfCraft: a cave opens around {} ({} of its cells now, {} so far)", pos.toShortString(), caves, cavesLogged);
		}
		if (!reveals.isEmpty() && caves == 0) {
			HalfCraft.LOG.info("HalfCraft: {} cells around {} revealed: {}{}", reveals.size(), pos.toShortString(),
				reveals.stream().map(r -> r.x() + ", " + r.y() + ", " + r.z()).collect(Collectors.joining("; ")),
				voidOnly ? " (void only: Half-Life lets nothing more be dug)" : "");
			Rollback.writeForgotten(level.getServer());
		}
	}

	/**
	 * What a cell of natural rock is: the ground's soil near its surface (Underground.soilAt), under it Minecraft's own
	 * underground (VanillaUnderground), or HalfCraft's (Underground) without that level. Null while Minecraft's is being
	 * generated there.
	 */
	private static @Nullable BlockState naturalState(ServerLevel level, DigRules.Reveal r) {
		double depth = depthUnder(r.x(), r.y(), r.z());
		String soil = Underground.soilAt(r.material(), depth);
		if (soil != null) {
			return BuiltInRegistries.BLOCK.getValue(Identifier.parse(soil)).defaultBlockState();
		}
		if (VanillaUnderground.available(level.getServer())) {
			BlockState state = VanillaUnderground.at(level.getServer(), r.x(), r.y(), r.z());
			// no cave right under the ground's surface: the ground over it would hang by a thread
			return state != null && state.isAir() && depth < Underground.CAVE_MIN_DEPTH ? net.minecraft.world.level.block.Blocks.STONE.defaultBlockState() : state;
		}
		return BuiltInRegistries.BLOCK.getValue(Identifier.parse(Underground.blockAt(level.getSeed(), r.x(), r.y(), r.z(), r.material(), depth))).defaultBlockState();
	}

	/** Blocks a cave opens up to from the nearest player; past it, it waits for a pick. */
	private static final double CAVE_REACH = 48.0;
	/** The server stopped: no cave's edge waits for a player any more. */
	public static void clearFrontier() {
		FRONTIER.clear();
	}

	/** The dug cells of caves whose neighbours were too far from every player to open, with what they opened as. */
	private static final java.util.Map<Cell, Integer> FRONTIER = new java.util.HashMap<>();
	private static final int FRONTIER_MAX = 20000, FRONTIER_EVERY = 10;
	/** Dug cells whose rock around is looked at in one tick: a wide cave opens over a few ticks, not in one long one. */
	private static final int REVEALS_PER_TICK = 256;
	/** How far up the surface over natural rock is looked for (Underground's depth): deeper is just deep. */
	private static final double DEPTH_LOOK = 16.0;
	private static int cavesLogged;

	private static boolean nearPlayer(ServerLevel level, BlockPos cell, double reach) {
		return level.getNearestPlayer(cell.getX() + 0.5, cell.getY() + 0.5, cell.getZ() + 0.5, reach, false) != null;
	}

	/** Blocks from the middle of cell (x, y, z) up to Half-Life's first triangle over it; infinity for none in DEPTH_LOOK. */
	private static double depthUnder(int x, int y, int z) {
		double fx = x + 0.5, fy = y + 0.5, fz = z + 0.5;
		List<HostTri> tris = new java.util.ArrayList<>();
		HostCollision.trianglesNear(new net.minecraft.world.phys.AABB(fx - 0.01, fy, fz - 0.01, fx + 0.01, fy + DEPTH_LOOK, fz + 0.01), tris);
		HostRay.Hit hit = HostRay.cast(tris, fx, fy, fz, fx, fy + DEPTH_LOOK, fz);
		return hit == null ? Double.POSITIVE_INFINITY : hit.y() - fy;
	}

	/** The world around a dug cell as DigRules reads it. Loads no chunk: a cell in one not loaded is left alone (as if dug). */
	private static DigRules.Cells cells(ServerLevel level) {
		return new DigRules.Cells() {
			@Override
			public boolean known(int x, int y, int z) {
				return HostCollision.isKnown(x, y, z);
			}

			@Override
			public boolean sky(int x, int y, int z) {
				return HostCollision.isSkyAt(x, y, z);
			}

			@Override
			public boolean loaded(int x, int y, int z) {
				return level.isLoaded(new BlockPos(x, y, z));
			}

			@Override
			public boolean dug(int x, int y, int z) {
				BlockPos pos = new BlockPos(x, y, z);
				return !level.isLoaded(pos) || isDug(level, pos);
			}

			@Override
			public boolean air(int x, int y, int z) {
				BlockPos pos = new BlockPos(x, y, z);
				return level.isLoaded(pos) && level.getBlockState(pos).isAir();
			}

			@Override
			public long @Nullable [] occupancy(int x, int y, int z) {
				return HostCollision.layersAt(x, y, z);
			}

			@Override
			public long @Nullable [] voids(int x, int y, int z) {
				return HostCollision.voidLayersAt(x, y, z);
			}

			@Override
			public boolean ground(int x, int y, int z) {
				return HostCollision.isGroundAt(x, y, z);
			}
		};
	}
}
