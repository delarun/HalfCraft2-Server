package dev.halfcraft.world;

import static dev.halfcraft.link.Proto.*;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import org.jspecify.annotations.Nullable;
import org.junit.jupiter.api.Test;

class DigRulesTest {
	private static final long[] FULL = { -1L, -1L, -1L, -1L, -1L, -1L, -1L, -1L };
	private static final long[] LOWER = { -1L, -1L, -1L, -1L, 0L, 0L, 0L, 0L };
	private static final long[] UPPER = { 0L, 0L, 0L, 0L, -1L, -1L, -1L, -1L };
	private static final long[] EMPTY = new long[8];
	// down, up, north, south, west, east, as DigRules has them
	private static final int[][] SIDES = { { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 } };

	private record Pos(int x, int y, int z) {
	}

	/**
	 * Cells known, loaded, not sky, not dug, air and rockless, except where the sets and maps say otherwise.
	 * One not loaded is dug too, as HostDig has it: out of reach, left alone.
	 */
	private static final class World implements DigRules.Cells {
		final Set<Pos> unknown = new HashSet<>();
		final Set<Pos> unloaded = new HashSet<>();
		final Set<Pos> sky = new HashSet<>();
		final Set<Pos> dug = new HashSet<>();
		final Set<Pos> built = new HashSet<>();
		final Map<Pos, long[]> occupancy = new HashMap<>();
		final Map<Pos, long[]> voids = new HashMap<>();
		final Set<Pos> ground = new HashSet<>();

		@Override
		public boolean known(int x, int y, int z) {
			return !this.unknown.contains(new Pos(x, y, z));
		}

		@Override
		public boolean sky(int x, int y, int z) {
			return this.sky.contains(new Pos(x, y, z));
		}

		@Override
		public boolean loaded(int x, int y, int z) {
			return !this.unloaded.contains(new Pos(x, y, z));
		}

		@Override
		public boolean dug(int x, int y, int z) {
			return this.dug.contains(new Pos(x, y, z)) || !loaded(x, y, z);
		}

		@Override
		public boolean air(int x, int y, int z) {
			return !this.built.contains(new Pos(x, y, z));
		}

		@Override
		public long @Nullable [] occupancy(int x, int y, int z) {
			return this.occupancy.get(new Pos(x, y, z));
		}

		@Override
		public long @Nullable [] voids(int x, int y, int z) {
			return this.voids.get(new Pos(x, y, z));
		}

		@Override
		public boolean ground(int x, int y, int z) {
			return this.ground.contains(new Pos(x, y, z));
		}
	}

	/** The layers with one sub-voxel of the top layer taken out. */
	private static long[] lessOne(long[] layers) {
		long[] out = layers.clone();
		out[7] = 0x7FFFFFFFFFFFFFFFL;
		return out;
	}

	/**
	 * The reveals around the dug cell (x, y, z), as HostDig asks for them: the cell is dug. Each of its
	 * sides' other neighbours is rock (buried: none of them shows a face) unless the test set its
	 * occupancy.
	 */
	private static List<DigRules.Reveal> reveals(World w, int x, int y, int z, int material, boolean brushlessOnly) {
		w.dug.add(new Pos(x, y, z));
		for (int[] a : SIDES) {
			for (int[] b : SIDES) {
				Pos p = new Pos(x + a[0] + b[0], y + a[1] + b[1], z + a[2] + b[2]);
				if (!p.equals(new Pos(x, y, z))) {
					w.occupancy.putIfAbsent(p, FULL);
				}
			}
		}
		return DigRules.reveals(w, x, y, z, material, brushlessOnly);
	}

	/** Rock only where a neighbour on side {@code s} of a cell touches it (bit z * 8 + x of layer y). */
	private static long[] face(int s) {
		long[] out = new long[8];
		for (int y = 0; y < 8; y++) {
			for (int z = 0; z < 8; z++) {
				for (int x = 0; x < 8; x++) {
					int[] at = { x, y, z };
					// down, up, north, south, west, east: the neighbour's top, bottom, z 7, z 0, x 7, x 0
					int axis = s / 2 == 0 ? 1 : s / 2 == 1 ? 2 : 0;
					if (at[axis] == (s % 2 == 0 ? 7 : 0)) {
						out[y] |= 1L << (z * 8 + x);
					}
				}
			}
		}
		return out;
	}

	private static long[] not(long[] layers) {
		long[] out = new long[8];
		for (int y = 0; y < 8; y++) {
			out[y] = ~layers[y];
		}
		return out;
	}

	/**
	 * A room over a floor on the line y = 1 around (0, 0, 0): air from y 1 up, the floor's cells at y 0
	 * (a thin brush in their top layer, the void beyond the map under it, as d1_trainstation_04's
	 * interrogation room has it), void below them.
	 */
	private static World floorOverVoid() {
		World w = new World();
		long[] brush = { 0L, 0L, 0L, 0L, 0L, 0L, 0L, -1L };
		for (int x = -3; x <= 3; x++) {
			for (int y = -3; y <= 3; y++) {
				for (int z = -3; z <= 3; z++) {
					Pos p = new Pos(x, y, z);
					if (y >= 1) {
						w.occupancy.put(p, EMPTY);
					} else if (y == 0) {
						w.occupancy.put(p, brush);
						w.voids.put(p, not(brush));
					} else {
						w.voids.put(p, FULL);
					}
				}
			}
		}
		return w;
	}

	@Test
	void shouldRevealAWhollyRockNeighbourWithTheMiningMaterial() {
		World w = new World();
		w.occupancy.put(new Pos(1, 0, 0), FULL);
		assertEquals(List.of(new DigRules.Reveal(1, 0, 0, DIG_IRON)), reveals(w, 0, 0, 0, DIG_IRON, false));
	}

	@Test
	void shouldRevealAMostlyVoidNeighbourAsStone() {
		World w = new World();
		w.occupancy.put(new Pos(0, -1, 0), LOWER);
		w.voids.put(new Pos(0, -1, 0), UPPER);
		assertEquals(List.of(new DigRules.Reveal(0, -1, 0, DIG_PLANKS, true)), reveals(w, 0, 0, 0, DIG_PLANKS, false));
	}

	@Test
	void shouldLeaveAPartlyRockNeighbour() {
		World w = new World();
		w.occupancy.put(new Pos(0, 0, 1), lessOne(FULL));
		assertEquals(List.of(), reveals(w, 0, 0, 0, DIG_STONE, false));
	}

	@Test
	void shouldLeaveDugBuiltAndUnknownNeighbours() {
		World w = new World();
		for (Pos p : List.of(new Pos(-1, 0, 0), new Pos(0, 0, -1), new Pos(0, 1, 0))) {
			w.occupancy.put(p, FULL);
		}
		w.dug.add(new Pos(-1, 0, 0));
		w.built.add(new Pos(0, 0, -1));
		w.unknown.add(new Pos(0, 1, 0));
		assertEquals(List.of(), reveals(w, 0, 0, 0, DIG_STONE, false));
	}

	@Test
	void shouldLeaveAWhollyRockNeighbourInTheMapsSkyButRevealItsTwinOutOfIt() {
		World w = new World();
		w.occupancy.put(new Pos(0, 1, 0), FULL); // inside the skybox's brush: its voxels are rock too
		w.sky.add(new Pos(0, 1, 0));
		w.occupancy.put(new Pos(0, -1, 0), FULL);
		assertEquals(List.of(new DigRules.Reveal(0, -1, 0, DIG_IRON)), reveals(w, 0, 0, 0, DIG_IRON, false));
	}

	@Test
	void shouldRevealASkyNeighbourThatIsPartlyVoid() {
		// the outside of the skybox's brush: its voxels the sky's, the rest of it beyond the map
		World w = new World();
		w.occupancy.put(new Pos(0, 1, 0), LOWER);
		w.voids.put(new Pos(0, 1, 0), UPPER);
		w.sky.add(new Pos(0, 1, 0));
		assertEquals(List.of(new DigRules.Reveal(0, 1, 0, DIG_IRON, true)), reveals(w, 0, 0, 0, DIG_IRON, false));
	}

	@Test
	void shouldRevealOnlyNeighboursWithoutBrushesWhileNothingMoreMayBeDug() {
		World w = new World();
		w.voids.put(new Pos(-1, 0, 0), FULL); // void only, beyond the map
		w.occupancy.put(new Pos(1, 0, 0), FULL); // inside a thick wall
		w.occupancy.put(new Pos(0, 0, 1), LOWER); // a wall's back, void behind it
		w.voids.put(new Pos(0, 0, 1), UPPER);
		assertEquals(List.of(new DigRules.Reveal(-1, 0, 0, DIG_IRON, true)), reveals(w, 0, 0, 0, DIG_IRON, true));
		assertEquals(List.of(new DigRules.Reveal(0, 0, 1, DIG_IRON, true), new DigRules.Reveal(-1, 0, 0, DIG_IRON, true), new DigRules.Reveal(1, 0, 0, DIG_IRON)),
			reveals(w, 0, 0, 0, DIG_IRON, false));
	}

	@Test
	void shouldRevealEverySideDownUpNorthSouthWestEast() {
		World w = new World();
		List<Pos> sides = List.of(new Pos(5, 5, 7), new Pos(5, 7, 7), new Pos(5, 6, 6), new Pos(5, 6, 8), new Pos(4, 6, 7), new Pos(6, 6, 7));
		for (Pos p : sides) {
			w.occupancy.put(p, FULL);
		}
		List<DigRules.Reveal> expected = sides.stream().map(p -> new DigRules.Reveal(p.x(), p.y(), p.z(), DIG_DIRT)).toList();
		assertEquals(expected, reveals(w, 5, 6, 7, DIG_DIRT, false));
	}

	@Test
	void shouldKeepTheFloorAroundAHoleWithTheRoomsAirOverIt() {
		// the floor's cells by the hole keep Half-Life's face: only the void under the hole becomes a block
		assertEquals(List.of(new DigRules.Reveal(0, -1, 0, DIG_STONE_BRICKS, true)), reveals(floorOverVoid(), 0, 0, 0, DIG_STONE_BRICKS, false));
	}

	@Test
	void shouldRevealTheSameFloorCellBuriedUnderRock() {
		World w = floorOverVoid();
		w.occupancy.put(new Pos(1, 1, 0), FULL); // a wall standing on it: no face of it shows
		assertEquals(List.of(new DigRules.Reveal(0, -1, 0, DIG_STONE_BRICKS, true), new DigRules.Reveal(1, 0, 0, DIG_STONE_BRICKS, true)), reveals(w, 0, 0, 0, DIG_STONE_BRICKS, false));
	}

	@Test
	void shouldHoldANeighbourBackWhileACellNextToItIsUnknown() {
		World w = new World();
		w.occupancy.put(new Pos(1, 0, 0), FULL);
		w.unknown.add(new Pos(2, 0, 0));
		assertEquals(List.of(), reveals(w, 0, 0, 0, DIG_IRON, false));
		assertFalse(DigRules.allKnown(w, 0, 0, 0));
		w.unknown.clear();
		assertTrue(DigRules.allKnown(w, 0, 0, 0));
		assertEquals(List.of(new DigRules.Reveal(1, 0, 0, DIG_IRON)), reveals(w, 0, 0, 0, DIG_IRON, false));
	}

	@Test
	void shouldHoldANeighbourBackWhileACellNextToItIsNotLoaded() {
		// not loaded reads as dug ("left alone"), which shows no face: it mustn't let a reveal cover one there
		World w = new World();
		w.occupancy.put(new Pos(1, 0, 0), FULL);
		w.unloaded.add(new Pos(1, 1, 0));
		assertEquals(List.of(), reveals(w, 0, 0, 0, DIG_IRON, false));
		assertFalse(DigRules.allKnown(w, 0, 0, 0));
		w.unloaded.clear();
		assertTrue(DigRules.allKnown(w, 0, 0, 0));
		assertEquals(List.of(new DigRules.Reveal(1, 0, 0, DIG_IRON)), reveals(w, 0, 0, 0, DIG_IRON, false));
		// the neighbour itself not loaded: left alone
		w.unloaded.add(new Pos(1, 0, 0));
		assertEquals(List.of(), reveals(w, 0, 0, 0, DIG_IRON, false));
	}

	@Test
	void shouldNotCountAFaceTowardADugCellAsShowing() {
		World w = new World();
		w.occupancy.put(new Pos(1, 0, 0), FULL);
		w.occupancy.put(new Pos(0, 0, 0), EMPTY); // the hole itself: air once cut
		w.occupancy.put(new Pos(1, 1, 0), EMPTY); // a hole dug before over it
		w.dug.add(new Pos(1, 1, 0));
		assertEquals(List.of(new DigRules.Reveal(1, 0, 0, DIG_IRON)), reveals(w, 0, 0, 0, DIG_IRON, false));
		w.dug.remove(new Pos(1, 1, 0)); // the map's air over it: its top shows
		assertEquals(List.of(), reveals(w, 0, 0, 0, DIG_IRON, false));
	}

	@Test
	void shouldRevealTheVoidBehindAThinWallButNotVoidAgainstTheMapsAirWhetherOrNotMoreMayBeDug() {
		// W1 of d1_trainstation_02: the wall's surface cell holds its brush on the face it shares with the
		// void cell beside the tunnel, the room's air beyond: the void cell shows no face
		World w = new World();
		w.voids.put(new Pos(-1, 0, 0), FULL);
		w.occupancy.put(new Pos(-1, 0, 1), face(3));
		assertEquals(List.of(new DigRules.Reveal(-1, 0, 0, DIG_IRON, true)), reveals(w, 0, 0, 0, DIG_IRON, false));
		assertEquals(List.of(new DigRules.Reveal(-1, 0, 0, DIG_IRON, true)), reveals(w, 0, 0, 0, DIG_IRON, true));
		w.occupancy.put(new Pos(-1, 0, 1), face(2)); // the brush on its far side: the void meets the air
		assertEquals(List.of(), reveals(w, 0, 0, 0, DIG_IRON, false));
		assertEquals(List.of(), reveals(w, 0, 0, 0, DIG_IRON, true));
	}

	@Test
	void shouldLookAtTheSubVoxelsOfEachNeighbourThatTouchTheCell() {
		for (int s = 0; s < SIDES.length; s++) {
			World w = new World();
			for (int[] side : SIDES) {
				w.occupancy.put(new Pos(side[0], side[1], side[2]), FULL);
			}
			Pos m = new Pos(SIDES[s][0], SIDES[s][1], SIDES[s][2]);
			w.occupancy.put(m, face(s));
			assertFalse(DigRules.exposed(w, 0, 0, 0), "rock on the face of side " + s);
			w.occupancy.put(m, not(face(s)));
			assertTrue(DigRules.exposed(w, 0, 0, 0), "air on the face of side " + s);
			// one sub-voxel of the face is air: in the face's one layer for a neighbour below or above, in its
			// bottom and its top layer for one beside
			for (int y : s == 0 ? new int[] { 7 } : s == 1 ? new int[] { 0 } : new int[] { 0, 7 }) {
				long[] holed = face(s);
				holed[y] &= holed[y] - 1;
				w.occupancy.put(m, holed);
				assertTrue(DigRules.exposed(w, 0, 0, 0), "one sub-voxel of air in layer " + y + " of the face of side " + s);
			}
			w.occupancy.put(m, EMPTY);
			w.voids.put(m, face(s));
			assertFalse(DigRules.exposed(w, 0, 0, 0), "void on the face of side " + s);
		}
	}

	@Test
	void shouldNeedEveryCellTwoStepsAwayKnownButNotTheDugCellOrFartherOnes() {
		World w = new World();
		assertTrue(DigRules.allKnown(w, 0, 0, 0));
		w.unknown.add(new Pos(0, 0, 0)); // the dug cell itself isn't read
		w.unknown.add(new Pos(3, 0, 0));
		w.unknown.add(new Pos(1, 1, 1));
		assertTrue(DigRules.allKnown(w, 0, 0, 0));
		for (Pos p : List.of(new Pos(0, 0, -1), new Pos(2, 0, 0), new Pos(1, 1, 0), new Pos(0, -1, 1))) {
			w.unknown.add(p);
			assertFalse(DigRules.allKnown(w, 0, 0, 0), p.toString());
			w.unknown.remove(p);
		}
	}

	@Test
	void shouldCountOccupancyAndVoidTogetherAsRock() {
		assertTrue(DigRules.whollyRock(FULL, null));
		assertTrue(DigRules.whollyRock(null, FULL));
		assertTrue(DigRules.whollyRock(LOWER, UPPER));
		assertFalse(DigRules.whollyRock(LOWER, null));
		assertFalse(DigRules.whollyRock(null, null));
	}

	@Test
	void shouldTakeStoneFromHalfVoidUpAndTheOpeningsMaterialBelow() {
		assertEquals(DIG_STONE, DigRules.revealMaterial(UPPER, DIG_GLASS));
		assertEquals(DIG_GLASS, DigRules.revealMaterial(lessOne(UPPER), DIG_GLASS));
		assertEquals(DIG_IRON, DigRules.revealMaterial(null, DIG_IRON));
		assertEquals(DIG_STONE, DigRules.revealMaterial(null, DIG_NONE));
		assertEquals(DIG_STONE, DigRules.revealMaterial(null, 99));
	}
}
