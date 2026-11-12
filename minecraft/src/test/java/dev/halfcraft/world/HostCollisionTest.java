package dev.halfcraft.world;

import static dev.halfcraft.link.Proto.COL_BLOCK_BITS;
import static dev.halfcraft.link.Proto.COL_BLOCK_BYTES;
import static dev.halfcraft.link.Proto.COL_BLOCK_X;
import static dev.halfcraft.link.Proto.COL_BLOCK_Y;
import static dev.halfcraft.link.Proto.COL_BLOCK_Z;
import static dev.halfcraft.link.Proto.COL_REGION_COUNT;
import static dev.halfcraft.link.Proto.COL_REGION_EPOCH;
import static dev.halfcraft.link.Proto.COL_REGION_HEADER_BYTES;
import static dev.halfcraft.link.Proto.COL_REGION_MAX_X;
import static dev.halfcraft.link.Proto.COL_REGION_MAX_Y;
import static dev.halfcraft.link.Proto.COL_REGION_MAX_Z;
import static dev.halfcraft.link.Proto.COL_REGION_MIN_X;
import static dev.halfcraft.link.Proto.COL_REGION_MIN_Y;
import static dev.halfcraft.link.Proto.COL_REGION_MIN_Z;
import static dev.halfcraft.link.Proto.VOID_ALL;
import static dev.halfcraft.link.Proto.VOID_GROUND_ALL;
import static dev.halfcraft.link.Proto.VOID_GROUND_SOME;
import static dev.halfcraft.link.Proto.VOID_SOME;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.Arrays;
import java.util.Set;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.shapes.BooleanOp;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

class HostCollisionTest {
	private static final long[] FULL = { -1L, -1L, -1L, -1L, -1L, -1L, -1L, -1L };
	private static final long B = COL_REGION_HEADER_BYTES;
	/** The first block of region (1, 0, 2), which {@link #header} describes. */
	private static final int PX = 8, PY = 0, PZ = 16;

	/** What the other test classes in this JVM find: no region known, no epoch joined. */
	@AfterEach
	void forgetTheCollision() {
		HostCollision.clear(-1);
	}

	/** The occupancy or void layers of a block: {@code head}, then zeros. */
	private static long[] layers(long... head) {
		return Arrays.copyOf(head, 8);
	}

	/** A ColRegion for region (1, 0, 2) in epoch 7, followed by {@code extra} zeroed bytes. */
	private static MemorySegment header(Arena arena, int count, long extra) {
		MemorySegment s = arena.allocate(B + extra, 8);
		s.set(JAVA_INT, COL_REGION_MIN_X, 8);
		s.set(JAVA_INT, COL_REGION_MIN_Z, 16);
		s.set(JAVA_INT, COL_REGION_MAX_X, 15);
		s.set(JAVA_INT, COL_REGION_MAX_Y, 7);
		s.set(JAVA_INT, COL_REGION_MAX_Z, 23);
		s.set(JAVA_INT, COL_REGION_EPOCH, 7);
		s.set(JAVA_INT, COL_REGION_COUNT, count);
		return s;
	}

	private static int index(int bx, int by, int bz) {
		return bx + 8 * bz + 64 * by;
	}

	/** Block {@code i} of a ColRegion's: at (x, y, z), its occupancy {@code bits}. */
	private static void block(MemorySegment s, int i, int x, int y, int z, long[] bits) {
		long e = B + (long) i * COL_BLOCK_BYTES;
		s.set(JAVA_INT, e + COL_BLOCK_X, x);
		s.set(JAVA_INT, e + COL_BLOCK_Y, y);
		s.set(JAVA_INT, e + COL_BLOCK_Z, z);
		for (int k = 0; k < 8; k++) {
			s.set(JAVA_LONG, e + COL_BLOCK_BITS + k * 8L, bits[k]);
		}
	}

	/** Cell (x, PY, PZ)'s collision shape, with its geometry or not, the cells in {@code dug} dug. */
	private static @Nullable VoxelShape shape(int x, boolean geometry, Set<Long> dug) {
		return HostCollision.collisionShapeAt(x, PY, PZ, geometry, dug, (cells, cx, cy, cz) -> cells.contains(BlockPos.asLong(cx, cy, cz)));
	}

	private static void assertSameShape(VoxelShape expected, @Nullable VoxelShape actual) {
		assertNotNull(actual);
		assertFalse(Shapes.joinIsNotEmpty(expected, actual, BooleanOp.NOT_SAME), () -> expected + " but " + actual);
	}

	@Test
	void shouldJoinOccupancyAndVoidLayerByLayer() {
		assertArrayEquals(layers(0xFFFF, -1), HostCollision.rock(layers(0xFF), layers(0xFF00, -1)));
	}

	@Test
	void shouldTakeEitherAloneAndGiveZerosForNeither() {
		long[] h = { 0, 0, 0, 0, -1L, -1L, -1L, -1L };
		assertArrayEquals(h, HostCollision.rock(null, h));
		assertArrayEquals(h, HostCollision.rock(h, null));
		assertArrayEquals(new long[8], HostCollision.rock(null, null));
	}

	@Test
	void shouldNeverChangeOrHandBackItsInputs() {
		long[] o = layers(1);
		long[] v = layers(2);
		assertNotSame(o, HostCollision.rock(o, v));
		assertNotSame(o, HostCollision.rock(o, null));
		assertNotSame(v, HostCollision.rock(null, v));
		assertArrayEquals(layers(1), o);
		assertArrayEquals(layers(2), v);
	}

	@Test
	void shouldReadAColVoidPayloadByBlockIndex() {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment s = header(arena, 1, 512 + 64);
			s.set(JAVA_BYTE, B + index(0, 0, 0), (byte) VOID_ALL);
			s.set(JAVA_BYTE, B + index(1, 3, 2), (byte) VOID_SOME);
			for (int y = 0; y < 4; y++) {
				s.set(JAVA_LONG, B + 512 + y * 8L, -1L);
			}
			HostCollision.RegionVoid r = HostCollision.parseVoid(s, 0);
			assertArrayEquals(FULL, r.at(0, 0, 0));
			assertArrayEquals(layers(-1, -1, -1, -1), r.at(1, 3, 2));
			assertNull(r.at(2, 3, 1));
			assertNull(r.at(7, 7, 7));
		}
	}

	@Test
	void shouldHandOutOneSharedArrayForAWhollyVoidBlock() {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment s = header(arena, 0, 512);
			s.set(JAVA_BYTE, B + index(0, 0, 0), (byte) VOID_ALL);
			s.set(JAVA_BYTE, B + index(5, 6, 7), (byte) VOID_ALL);
			HostCollision.RegionVoid r = HostCollision.parseVoid(s, 0);
			assertSame(r.at(0, 0, 0), r.at(5, 6, 7));
			assertSame(r.at(0, 0, 0), HostCollision.parseVoid(s, 0).at(5, 6, 7));
		}
	}

	@Test
	void shouldLeaveTheVoidSomeBlocksBeyondTheCountWithoutLayers() {
		try (Arena arena = Arena.ofConfined()) {
			// two blocks are partly void, the message holds the layers of one: the one with the greater index has none
			MemorySegment s = header(arena, 1, 512 + 64);
			s.set(JAVA_BYTE, B + index(5, 0, 0), (byte) VOID_SOME);
			s.set(JAVA_BYTE, B + index(1, 0, 1), (byte) VOID_SOME);
			s.set(JAVA_LONG, B + 512, 3L);
			HostCollision.RegionVoid r = HostCollision.parseVoid(s, 0);
			assertArrayEquals(layers(3), r.at(5, 0, 0));
			assertNull(r.at(1, 0, 1));
		}
	}

	@Test
	void shouldShowARegionsVoidOnceItsRegionArrivesAndForgetItWithTheRegion() {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment voids = header(arena, 0, 512);
			voids.set(JAVA_BYTE, B, (byte) VOID_ALL);
			MemorySegment region = header(arena, 0, 0);

			HostCollision.clear(7);
			HostCollision.readVoid(voids, 0);
			assertNull(HostCollision.voidLayersAt(PX, PY, PZ));
			assertNull(HostCollision.rockLayersAt(PX, PY, PZ));

			HostCollision.readRegion(region, 0);
			assertArrayEquals(FULL, HostCollision.voidLayersAt(PX, PY, PZ));
			assertArrayEquals(FULL, HostCollision.rockLayersAt(PX, PY, PZ));
			assertNull(HostCollision.voidLayersAt(PX + 1, PY, PZ));
			assertArrayEquals(new long[8], HostCollision.rockLayersAt(PX + 1, PY, PZ));

			// sent again without a kColVoid before it: no void left in it
			HostCollision.readRegion(region, 0);
			assertNull(HostCollision.voidLayersAt(PX, PY, PZ));
			assertArrayEquals(new long[8], HostCollision.rockLayersAt(PX, PY, PZ));

			HostCollision.readVoid(voids, 0);
			HostCollision.readRegion(region, 0);
			assertArrayEquals(FULL, HostCollision.voidLayersAt(PX, PY, PZ));
			HostCollision.clear(8);
			assertNull(HostCollision.voidLayersAt(PX, PY, PZ));
			assertNull(HostCollision.rockLayersAt(PX, PY, PZ));
		}
	}

	@Test
	void shouldCollideWithTheVoidAloneOrJoinedWithTheGeometryOnceItsRegionComes() {
		try (Arena arena = Arena.ofConfined()) {
			// block 0 of region (1, 0, 2) wholly void; block 1 void in its lower half, a brush in its top layer;
			// block 2 a brush without void
			MemorySegment voids = header(arena, 1, 512 + 64);
			voids.set(JAVA_BYTE, B + index(0, 0, 0), (byte) VOID_ALL);
			voids.set(JAVA_BYTE, B + index(1, 0, 0), (byte) VOID_SOME);
			for (int y = 0; y < 4; y++) {
				voids.set(JAVA_LONG, B + 512 + y * 8L, -1L);
			}
			MemorySegment region = header(arena, 2, 2L * COL_BLOCK_BYTES);
			block(region, 0, PX + 1, PY, PZ, layers(0, 0, 0, 0, 0, 0, 0, -1));
			block(region, 1, PX + 2, PY, PZ, FULL);
			VoxelShape lower = Shapes.box(0, 0, 0, 1, 0.5, 1);
			VoxelShape top = Shapes.box(0, 0.875, 0, 1, 1, 1);

			Set<Long> none = Set.of();
			HostCollision.clear(7);
			HostCollision.readVoid(voids, 0);
			assertNull(shape(PX, false, none));
			HostCollision.readRegion(region, 0);
			assertSame(Shapes.block(), shape(PX, false, none));
			assertSame(Shapes.block(), shape(PX, true, none));
			assertSameShape(lower, shape(PX + 1, false, none));
			assertSameShape(Shapes.or(lower, top), shape(PX + 1, true, none));
			assertNull(shape(PX + 2, false, none));
			assertSame(HostCollision.shapeAt(new BlockPos(PX + 2, PY, PZ)), shape(PX + 2, true, none));
			assertNull(shape(PX + 3, true, none));

			// dug: the void left out, the geometry kept where it's asked for
			Set<Long> dug = Set.of(BlockPos.asLong(PX, PY, PZ), BlockPos.asLong(PX + 1, PY, PZ));
			assertNull(shape(PX, false, dug));
			assertNull(shape(PX, true, dug));
			assertNull(shape(PX + 1, false, dug));
			assertSameShape(top, shape(PX + 1, true, dug));
			// whether a cell is dug is asked only about one with void
			assertSame(HostCollision.shapeAt(new BlockPos(PX + 2, PY, PZ)), HostCollision.<Object>collisionShapeAt(PX + 2, PY, PZ, true, null, (c, cx, cy, cz) -> {
				throw new AssertionError("asked whether a cell without void is dug");
			}));

			// sent again without a kColVoid: the geometry alone
			HostCollision.readRegion(region, 0);
			assertNull(shape(PX, false, none));
			assertNull(shape(PX + 1, false, none));
			assertSameShape(top, shape(PX + 1, true, none));
		}
	}

	@Test
	void shouldCollideWithTheRockUnderTheGroundOnlyNextToADugCell() {
		try (Arena arena = Arena.ofConfined()) {
			// blocks 0 and 1 of region (1, 0, 2) rock under the ground: 0 wholly, 1 in its lower half
			MemorySegment voids = header(arena, 1, 512 + 64);
			voids.set(JAVA_BYTE, B + index(0, 0, 0), (byte) VOID_GROUND_ALL);
			voids.set(JAVA_BYTE, B + index(1, 0, 0), (byte) VOID_GROUND_SOME);
			for (int y = 0; y < 4; y++) {
				voids.set(JAVA_LONG, B + 512 + y * 8L, -1L);
			}
			HostCollision.clear(7);
			HostCollision.readVoid(voids, 0);
			HostCollision.readRegion(header(arena, 0, 0), 0);

			// nothing dug: the ground's triangles alone hold what walks on it
			Set<Long> none = Set.of();
			assertNull(shape(PX, false, none));
			assertNull(shape(PX + 1, false, none));
			// a cell dug over them: the hole's bottom is solid, the dug cell itself isn't
			Set<Long> above = Set.of(BlockPos.asLong(PX, PY + 1, PZ), BlockPos.asLong(PX + 1, PY + 1, PZ));
			assertSame(Shapes.block(), shape(PX, false, above));
			assertSameShape(Shapes.box(0, 0, 0, 1, 0.5, 1), shape(PX + 1, true, above));
			assertNull(shape(PX, false, Set.of(BlockPos.asLong(PX, PY, PZ), BlockPos.asLong(PX, PY + 1, PZ))));
			// a side dug: a hole's wall
			assertSame(Shapes.block(), shape(PX, false, Set.of(BlockPos.asLong(PX - 1, PY, PZ))));
		}
	}

	@Test
	void shouldFindTheVoidOfNegativeBlockPositions() {
		try (Arena arena = Arena.ofConfined()) {
			// region (-1, -1, -1): blocks -8..-1 along every axis, the last of them is block 511
			MemorySegment voids = header(arena, 0, 512);
			MemorySegment region = header(arena, 0, 0);
			for (MemorySegment s : new MemorySegment[] { voids, region }) {
				s.set(JAVA_INT, COL_REGION_MIN_X, -8);
				s.set(JAVA_INT, COL_REGION_MIN_Y, -8);
				s.set(JAVA_INT, COL_REGION_MIN_Z, -8);
				s.set(JAVA_INT, COL_REGION_MAX_X, -1);
				s.set(JAVA_INT, COL_REGION_MAX_Y, -1);
				s.set(JAVA_INT, COL_REGION_MAX_Z, -1);
			}
			voids.set(JAVA_BYTE, B + 511, (byte) VOID_ALL);

			HostCollision.clear(7);
			HostCollision.readVoid(voids, 0);
			HostCollision.readRegion(region, 0);
			assertArrayEquals(FULL, HostCollision.voidLayersAt(-1, -1, -1));
			assertNull(HostCollision.voidLayersAt(-2, -1, -1));
			assertNull(HostCollision.voidLayersAt(-1, -1, 0));
		}
	}

	@Test
	void shouldForgetAVoidStillWaitingForItsRegionWhenCleared() {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment voids = header(arena, 0, 512);
			voids.set(JAVA_BYTE, B, (byte) VOID_ALL);
			MemorySegment region = header(arena, 0, 0);

			HostCollision.clear(7);
			HostCollision.readVoid(voids, 0);
			HostCollision.clear(7);
			HostCollision.readRegion(region, 0);
			assertNull(HostCollision.voidLayersAt(PX, PY, PZ));
			assertArrayEquals(new long[8], HostCollision.rockLayersAt(PX, PY, PZ));
		}
	}

	@Test
	void shouldIgnoreAVoidFromAnotherEpoch() {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment voids = header(arena, 0, 512);
			voids.set(JAVA_BYTE, B, (byte) VOID_ALL);
			MemorySegment region = header(arena, 0, 0);
			region.set(JAVA_INT, COL_REGION_EPOCH, 8);

			HostCollision.clear(8);
			HostCollision.readVoid(voids, 0);
			HostCollision.readRegion(region, 0);
			assertNull(HostCollision.voidLayersAt(PX, PY, PZ));
			assertArrayEquals(new long[8], HostCollision.rockLayersAt(PX, PY, PZ));
		}
	}
}
