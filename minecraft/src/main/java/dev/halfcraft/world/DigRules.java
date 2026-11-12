package dev.halfcraft.world;

import static dev.halfcraft.link.Proto.*;

import java.util.ArrayList;
import java.util.List;
import org.jspecify.annotations.Nullable;

/**
 * Which cells around a dug one become blocks (are revealed): those wholly rock to a tunnel (Half-Life's
 * brushes and its void beyond the map, joined) that nothing dug or built yet, not the map's sky, and with
 * no face Half-Life draws (toward the map's air). Revealed, a cell is dug and holds the block; a cell only
 * partly rock, or with a face Half-Life draws, keeps Half-Life's surface, and the hole's walls show the
 * rest (its void is solid to Minecraft all the same: {@link HostDig#collisionShape}). Rock mostly void
 * (behind the map) is stone; inside a thick wall it is the wall's material. While Half-Life lets nothing
 * more be dug, only void is revealed: a cell with a brush in it would be cut out of Half-Life's walls.
 */
public final class DigRules {
	// sub-voxels of a cell (8 layers of 64)
	private static final int SUB_VOXELS = 8 * 64;
	// down, up, north, south, west, east: Minecraft's Direction order
	private static final int[][] SIDES = { { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 } };
	// the sub-voxels of the neighbour on side s that touch the cell (bit z * 8 + x of each layer): its top,
	// bottom, south (z 7), north (z 0), east (x 7) and west (x 0) face
	private static final long[] FACE_BITS = { -1L, -1L, 0xFF00000000000000L, 0xFFL, 0x8080808080808080L, 0x0101010101010101L };
	// the layers of those: only the top or bottom one for a neighbour below or above, else all of them
	private static final int[] FACE_LAYER = { 7, 0, -1, -1, -1, -1 };

	/** The world around a dug cell, as the rules read it. */
	public interface Cells {
		/** Half-Life has sent the cell's region. */
		boolean known(int x, int y, int z);

		/** All of the cell's Half-Life geometry is the map's sky (its skybox brushes). */
		boolean sky(int x, int y, int z);

		/** The cell's chunk is loaded. */
		boolean loaded(int x, int y, int z);

		/** The cell is dug already (or out of reach, its chunk not loaded: left alone). */
		boolean dug(int x, int y, int z);

		/** Minecraft has nothing in the cell. */
		boolean air(int x, int y, int z);

		/** The cell's Half-Life geometry as 8 layers (bit z * 8 + x of layer y), or null for none. */
		long @Nullable [] occupancy(int x, int y, int z);

		/** The cell is rock under Half-Life's ground (its void is that, not the void beyond the map). */
		default boolean ground(int x, int y, int z) {
			return false;
		}

		/** The cell's void beyond the map as 8 layers, or null for none. */
		long @Nullable [] voids(int x, int y, int z);
	}

	/**
	 * A cell that becomes a block of a Proto.DIG_* material; or, {@code natural} (at least half of it void beyond the
	 * map or rock under the ground), what Underground makes of the rock there, {@code material} the one dug down from.
	 */
	public record Reveal(int x, int y, int z, int material, boolean natural) {
		public Reveal(int x, int y, int z, int material) {
			this(x, y, z, material, false);
		}
	}

	private DigRules() {
	}

	/**
	 * The cells around the dug cell (x, y, z) that are revealed, its {@code material} opened. A neighbour
	 * is revealed when, checked in this order: its region is known; it isn't the map's sky alone (all its
	 * brushes the sky's and none of it void: the sky's brushes are rock to {@link #whollyRock}, and a block
	 * in one would hang against the skybox; one partly void is revealed, so a tunnel goes on through the
	 * void there); with {@code brushlessOnly} (Half-Life lets nothing more be dug), it holds none of
	 * Half-Life's brushes, so marking it dug cuts nothing out of Half-Life's walls; it is wholly rock; it
	 * isn't dug; Minecraft has nothing in it; and it isn't {@link #exposed}: a block put there would cover a face Half-Life draws (a floor under a
	 * room's air, the wall next to the hole).
	 */
	public static List<Reveal> reveals(Cells cells, int x, int y, int z, int material, boolean brushlessOnly) {
		List<Reveal> out = new ArrayList<>();
		for (int[] side : SIDES) {
			int nx = x + side[0], ny = y + side[1], nz = z + side[2];
			if (!cells.known(nx, ny, nz)) {
				continue;
			}
			long[] voids = cells.voids(nx, ny, nz);
			if (voids == null && cells.sky(nx, ny, nz)) {
				continue;
			}
			long[] occupancy = cells.occupancy(nx, ny, nz);
			if (brushlessOnly && occupancy != null) {
				continue;
			}
			boolean natural = voidCount(voids) >= SUB_VOXELS / 2;
			// a cell with a face Half-Life draws stays Half-Life's, rock under the ground too (revealed, it stood out of the
			// ground): a hole's side there is mined where the look meets its collision (HostDigClient.holeWall)
			if (whollyRock(occupancy, voids) && !cells.dug(nx, ny, nz) && cells.air(nx, ny, nz) && !exposed(cells, nx, ny, nz)) {
				out.add(new Reveal(nx, ny, nz, natural ? knownMaterial(material) : revealMaterial(voids, material), natural));
			}
		}
		return out;
	}

	/**
	 * Whether Half-Life draws a face of the cell (x, y, z): a neighbour that isn't dug has a sub-voxel on
	 * the face they share that isn't rock (map air against it). A dug neighbour's faces are a hole's walls,
	 * so it shows nothing; one whose chunk isn't loaded or whose region is unknown counts as exposure,
	 * until a later try of the reveal knows it ({@link #allKnown}). Rock only behind the face (a thin
	 * wall's back, void behind it) is no exposure.
	 */
	// package-private for DigRulesTest
	static boolean exposed(Cells cells, int x, int y, int z) {
		for (int s = 0; s < SIDES.length; s++) {
			int mx = x + SIDES[s][0], my = y + SIDES[s][1], mz = z + SIDES[s][2];
			if (!cells.loaded(mx, my, mz)) {
				return true;
			}
			if (cells.dug(mx, my, mz)) {
				continue;
			}
			if (!cells.known(mx, my, mz)) {
				return true;
			}
			long[] rock = HostCollision.rock(cells.occupancy(mx, my, mz), cells.voids(mx, my, mz));
			for (int layer = 0; layer < 8; layer++) {
				if ((FACE_LAYER[s] < 0 || FACE_LAYER[s] == layer) && (rock[layer] & FACE_BITS[s]) != FACE_BITS[s]) {
					return true;
				}
			}
		}
		return false;
	}

	/**
	 * Whether every cell {@link #reveals} reads around the dug cell (x, y, z), its six neighbours and theirs,
	 * is in a loaded chunk and a region Half-Life has sent. Until then a reveal may leave a neighbour that a
	 * later try reveals ({@link RevealRetry}).
	 */
	public static boolean allKnown(Cells cells, int x, int y, int z) {
		for (int[] a : SIDES) {
			if (!seen(cells, x + a[0], y + a[1], z + a[2])) {
				return false;
			}
			for (int[] b : SIDES) {
				boolean back = a[0] + b[0] == 0 && a[1] + b[1] == 0 && a[2] + b[2] == 0; // the dug cell itself
				if (!back && !seen(cells, x + a[0] + b[0], y + a[1] + b[1], z + a[2] + b[2])) {
					return false;
				}
			}
		}
		return true;
	}

	private static boolean seen(Cells cells, int x, int y, int z) {
		return cells.loaded(x, y, z) && cells.known(x, y, z);
	}

	/** Every sub-voxel is rock: in Half-Life's geometry or in its void. */
	public static boolean whollyRock(long @Nullable [] occupancy, long @Nullable [] voids) {
		for (long layer : HostCollision.rock(occupancy, voids)) {
			if (layer != -1L) {
				return false;
			}
		}
		return true;
	}

	/**
	 * What a revealed cell is: stone when at least half of it is void (behind the map), else the
	 * material opened next to it; stone for DIG_NONE or a material it doesn't know.
	 */
	public static int revealMaterial(long @Nullable [] voids, int material) {
		return voidCount(voids) >= SUB_VOXELS / 2 ? DIG_STONE : knownMaterial(material);
	}

	/** The material, or stone for DIG_NONE or one it doesn't know. */
	static int knownMaterial(int material) {
		return material < DIG_STONE || material > DIG_SAND ? DIG_STONE : material;
	}

	private static int voidCount(long @Nullable [] voids) {
		int count = 0;
		if (voids != null) {
			for (long layer : voids) {
				count += Long.bitCount(layer);
			}
		}
		return count;
	}
}
