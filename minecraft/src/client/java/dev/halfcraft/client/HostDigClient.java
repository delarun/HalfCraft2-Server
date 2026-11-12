package dev.halfcraft.client;

import static dev.halfcraft.link.Proto.DIG_STONE;

import dev.halfcraft.HalfCraft;
import dev.halfcraft.client.render.WorldExporter;
import dev.halfcraft.mobs.HostNav;
import dev.halfcraft.weapon.HostWeapons;
import dev.halfcraft.world.DigMaterials;
import dev.halfcraft.world.DigProgress;
import dev.halfcraft.world.DugBits;
import dev.halfcraft.world.DugColumn;
import dev.halfcraft.world.HostClip;
import dev.halfcraft.world.HostDig;
import dev.halfcraft.world.HostRay;
import it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap;
import dev.halfcraft.world.HostCollision;
import dev.halfcraft.world.HostTri;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.particle.TerrainParticle;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.client.resources.sounds.SimpleSoundInstance;
import net.minecraft.client.resources.sounds.SoundInstance;
import net.minecraft.client.server.IntegratedServer;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundSource;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.block.SoundType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

/**
 * The mining player's side of {@link HostDig}: attacking a diggable face of Half-Life's walls mines
 * the cell its surface is in, as long as mining a block of its material with what's in hand would take
 * (cracks, hit sounds), and then the integrated server opens it. Also marks for meshing again the
 * sections whose dug cells changed (their hole walls). Render thread only.
 */
public final class HostDigClient {
	private static final DigProgress PROGRESS = new DigProgress();
	private static int ticks;
	// chunk -> its dug cells as last seen, to tell which sections changed
	private static final Long2ObjectOpenHashMap<DugColumn> SEEN = new Long2ObjectOpenHashMap<>();
	private static @Nullable ClientLevel seenLevel;

	private HostDigClient() {
	}

	/** A cell of Half-Life's wall to mine: where the look meets it (x, y, z), the face's normal, and what it digs as. */
	private record Target(BlockPos pos, int material, double x, double y, double z, double nx, double ny, double nz) {
	}

	/**
	 * The cell of Half-Life's wall under the crosshair that attacking would mine, or null: none is hit,
	 * Half-Life lets nothing be dug, the player holds Half-Life's weapon or plays a mode that can't build, the face
	 * isn't diggable, or the cell is dug already or a Minecraft block fills it. The integrated server opens a
	 * cell, or the HalfCraft server (DigPayload).
	 */
	public static @Nullable BlockPos target(Minecraft minecraft) {
		Target target = find(minecraft);
		return target == null ? null : target.pos();
	}

	private static @Nullable Target find(Minecraft minecraft) {
		ClientLevel level = minecraft.level;
		LocalPlayer player = minecraft.player;
		if (level == null || player == null || minecraft.gameMode == null
			|| (minecraft.getSingleplayerServer() == null && !net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.canSend(dev.halfcraft.world.DigPayload.TYPE))) {
			return null;
		}
		if (!HostClient.linked() || HostClient.host().digOff() || !HostNav.inMirror(level)) {
			return null;
		}
		if (HostWeapons.isStandIn(player.getMainHandItem()) || minecraft.gameMode.getPlayerMode().isBlockPlacingRestricted()) {
			return null;
		}
		Target surface = null;
		double surfaceDistance = Double.POSITIVE_INFINITY;
		if (minecraft.hitResult instanceof HostClip.HostHitResult result && result.hit.tri().diggable()) {
			int[] cell = HostRay.surfaceCell(result.hit);
			HostRay.Hit hit = result.hit;
			surface = new Target(new BlockPos(cell[0], cell[1], cell[2]), hit.tri().digMaterial, hit.x(), hit.y(), hit.z(), hit.nx(), hit.ny(), hit.nz());
			surfaceDistance = player.getEyePosition().distanceTo(result.getLocation());
		}
		// a hole's side or bottom where Half-Life has no triangle (under its ground's surface): the look meets the rock there
		Target wall = holeWall(level, player, Math.min(surfaceDistance, player.blockInteractionRange()));
		Target target = wall != null ? wall : surface;
		if (target == null || dug(level, target.pos().getX(), target.pos().getY(), target.pos().getZ())) {
			return null;
		}
		BlockState here = level.getBlockState(target.pos());
		if (!here.isAir() && !here.canBeReplaced()) {
			return null;
		}
		return target;
	}

	/**
	 * The first cell along the look, nearer than {@code reach}, that is rock Half-Life's collision holds (HostDig.collisionShape)
	 * next to a dug cell: a hole's side or bottom. Its material is its diggable triangles' (the ground's surface in it), else
	 * stone. Null for none, or a Minecraft block in the way first.
	 */
	private static @Nullable Target holeWall(ClientLevel level, LocalPlayer player, double reach) {
		Vec3 from = player.getEyePosition();
		Vec3 to = from.add(player.getViewVector(1.0F).scale(reach));
		BlockPos[] found = new BlockPos[1];
		BlockHitResult[] at = new BlockHitResult[1];
		BlockGetter.traverseBlocks(from, to, null, (ignored, pos) -> {
			BlockState state = level.getBlockState(pos);
			if (!state.getShape(level, pos).isEmpty()) {
				return Boolean.FALSE; // Minecraft's block: vanilla's to mine
			}
			if (dug(level, pos.getX(), pos.getY(), pos.getZ()) || !nextToDug(level, pos)) {
				return null;
			}
			VoxelShape rock = HostDig.collisionShape(level, pos, true);
			BlockHitResult hit = rock == null ? null : rock.clip(from, to, pos);
			if (hit == null) {
				return null;
			}
			found[0] = pos.immutable();
			at[0] = hit;
			return Boolean.TRUE;
		}, ignored -> Boolean.FALSE);
		if (found[0] == null) {
			return null;
		}
		BlockPos pos = found[0];
		int material = DIG_STONE;
		List<HostTri> tris = new ArrayList<>();
		HostCollision.trianglesNear(new AABB(pos), tris);
		for (HostTri tri : tris) {
			if (tri.diggable()) {
				material = tri.digMaterial;
				break;
			}
		}
		Vec3 hit = at[0].getLocation();
		var normal = at[0].getDirection().getUnitVec3();
		return new Target(pos, material, hit.x, hit.y, hit.z, normal.x, normal.y, normal.z);
	}

	private static boolean nextToDug(ClientLevel level, BlockPos pos) {
		for (Direction side : Direction.values()) {
			if (dug(level, pos.getX() + side.getStepX(), pos.getY() + side.getStepY(), pos.getZ() + side.getStepZ())) {
				return true;
			}
		}
		return false;
	}

	/**
	 * The player attacked (clicked or held): mines the {@link #target} cell, if any, one hit a tick.
	 * Returns whether it did, and Minecraft's own attack is to be left out.
	 */
	public static boolean attack(Minecraft minecraft) {
		Target target = find(minecraft);
		if (target == null) {
			return false;
		}
		var delay = (dev.halfcraft.client.mixin.GameModeDelayAccessor) minecraft.gameMode;
		if (delay.halfcraft$getDestroyDelay() > 0) {
			// Minecraft broke a block of its own a moment ago: the wall behind it waits, as Minecraft's next block would
			delay.halfcraft$setDestroyDelay(delay.halfcraft$getDestroyDelay() - 1);
			return true;
		}
		BlockPos pos = target.pos();
		LocalPlayer player = minecraft.player;
		ClientLevel level = minecraft.level;
		int material = target.material();
		BlockState state = HostDig.stateFor(material);
		float delta = player.getAbilities().instabuild ? 1.0F : state.getDestroyProgress(player, level, pos);
		DigProgress.Step step = PROGRESS.hit(pos.asLong(), ticks, delta);
		if (step == DigProgress.Step.KNOCK) {
			knock(minecraft, level, target, state);
		} else if (step == DigProgress.Step.OPEN) {
			open(minecraft, player, pos, material);
			// and a block of Minecraft's behind it waits too
			delay.halfcraft$setDestroyDelay(DigProgress.DELAY_TICKS);
		}
		return true;
	}

	// the block's hit sound and a chip off the surface
	private static void knock(Minecraft minecraft, ClientLevel level, Target hit, BlockState state) {
		BlockPos pos = hit.pos();
		SoundType sound = state.getSoundType();
		minecraft.getSoundManager().play(new SimpleSoundInstance(
			sound.getHitSound(), SoundSource.BLOCKS, (sound.getVolume() + 1.0F) / 8.0F, sound.getPitch() * 0.5F, SoundInstance.createUnseededRandom(), pos
		));
		minecraft.particleEngine.add(new TerrainParticle(level, hit.x() + hit.nx() * 0.05, hit.y() + hit.ny() * 0.05, hit.z() + hit.nz() * 0.05, 0.0, 0.0, 0.0, state, pos)
			.setPower(0.2F).scale(0.6F));
	}

	// mined through: the integrated server opens the cell
	private static void open(Minecraft minecraft, LocalPlayer player, BlockPos pos, int material) {
		HalfCraft.LOG.info("HalfCraft: dug Half-Life's wall at {} ({})", pos.toShortString(), DigMaterials.blockId(material));
		IntegratedServer server = minecraft.getSingleplayerServer();
		if (server == null) {
			// the halfcraft server's
			net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.send(new dev.halfcraft.world.DigPayload(pos, material));
			return;
		}
		UUID uuid = player.getUUID();
		server.execute(() -> {
			ServerPlayer sp = server.getPlayerList().getPlayer(uuid);
			if (sp != null && HostNav.inMirror(sp.level())) {
				HostDig.open(sp.level(), sp, pos, material);
			}
		});
	}

	/** The crack over the cell being mined, {x, y, z, stage 0-9}, or null. */
	public static int @Nullable [] crack() {
		if (!PROGRESS.mining() || PROGRESS.stage() < 0) {
			return null;
		}
		long cell = PROGRESS.cell();
		return new int[] { BlockPos.getX(cell), BlockPos.getY(cell), BlockPos.getZ(cell), PROGRESS.stage() };
	}

	/** Each client tick (END_CLIENT_TICK). */
	public static void tick(Minecraft minecraft) {
		ticks++;
		ClientLevel level = minecraft.level;
		if (level == null || minecraft.player == null || !HostClient.linked()) {
			PROGRESS.clear();
			SEEN.clear();
			seenLevel = null;
			return;
		}
		PROGRESS.tick(ticks);
		pollDug(minecraft, level);
	}

	// chunks whose dug cells changed: the sections that changed are meshed again, with those next to them
	// (a dug cell can open one of their walls)
	private static void pollDug(Minecraft minecraft, ClientLevel level) {
		if (level != seenLevel) {
			SEEN.clear();
			seenLevel = level;
		}
		int radius = minecraft.options.getEffectiveRenderDistance() + 1;
		int pcx = minecraft.player.getBlockX() >> 4, pcz = minecraft.player.getBlockZ() >> 4;
		for (int cx = pcx - radius; cx <= pcx + radius; cx++) {
			for (int cz = pcz - radius; cz <= pcz + radius; cz++) {
				long key = ChunkPos.pack(cx, cz);
				LevelChunk chunk = level.getChunkSource().getChunk(cx, cz, ChunkStatus.FULL, false);
				if (chunk == null) {
					SEEN.remove(key);
					continue;
				}
				DugColumn now = chunk.getAttached(HostDig.DUG);
				if (now == null) {
					now = DugColumn.EMPTY;
				}
				DugColumn old = SEEN.put(key, now);
				if (old == now) {
					continue;
				}
				if (old == null) {
					old = DugColumn.EMPTY;
				}
				for (DugColumn column : new DugColumn[] { now, old }) {
					for (DugColumn.DugSection s : column.sections()) {
						int sy = s.sectionY();
						if (!Arrays.equals(old.bits(sy), now.bits(sy))) {
							markChanged(cx, sy, cz);
						}
					}
				}
			}
		}
		if (SEEN.size() > 8192) {
			SEEN.clear();
		}
	}

	private static void markChanged(int sx, int sy, int sz) {
		WorldExporter.markDirtyNow(sx, sy, sz);
		WorldExporter.markDirty(sx - 1, sy, sz);
		WorldExporter.markDirty(sx + 1, sy, sz);
		WorldExporter.markDirty(sx, sy - 1, sz);
		WorldExporter.markDirty(sx, sy + 1, sz);
		WorldExporter.markDirty(sx, sy, sz - 1);
		WorldExporter.markDirty(sx, sy, sz + 1);
	}

	/** The dug cells of section (sx, sy, sz) of a loaded chunk (see {@link DugBits}), or null for none. Never change them. */
	public static long @Nullable [] dugBits(ClientLevel level, int sx, int sy, int sz) {
		LevelChunk chunk = level.getChunkSource().getChunk(sx, sz, ChunkStatus.FULL, false);
		return chunk == null ? null : HostDig.sectionBits(chunk, sy);
	}

	/** Whether the cell (x, y, z) of a loaded chunk is dug. */
	public static boolean dug(ClientLevel level, int x, int y, int z) {
		long[] bits = dugBits(level, x >> 4, y >> 4, z >> 4);
		return bits != null && DugBits.get(bits, DugBits.index(x, y, z));
	}
}
