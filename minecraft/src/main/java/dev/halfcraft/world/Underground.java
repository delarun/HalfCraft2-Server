package dev.halfcraft.world;

import static dev.halfcraft.link.Proto.*;

/**
 * What lies in the natural rock of the mirror world (beyond Half-Life's map, and under its ground): the block a cell of
 * it becomes when digging reveals it (HostDig), as Minecraft's own underground. Near the ground's surface its dirt or
 * sand (sandstone under the sand), under that stone, deeper deepslate; ores in veins by height, as Minecraft spreads
 * them; caves (long tunnels and wider caverns) of air. All of it from the world's seed, the same for every player and
 * every time. Pure: blocks by name, so tests run without a world.
 *
 * <p>Heights are Minecraft's own shifted down by {@link #SHIFT}: Half-Life's maps lie around y 0-30 of the mirror world,
 * where Minecraft's surface is at its sea level, 63.
 */
public final class Underground {
	/** Minecraft's sea level over the mirror world's ground. */
	static final int SHIFT = 50;
	/** Blocks under the surface that are still the ground's own (dirt, sand), and sandstone under sand down to this. */
	static final double SOIL_DEPTH = 3.0, SANDSTONE_DEPTH = 5.0;
	/** No cave this close under the surface: the ground over it would hang by a thread. */
	static final double CAVE_MIN_DEPTH = 6.0;

	public static final String AIR = "minecraft:air";

	/** One ore: its block (and its deepslate one), its heights (the mirror world's), and how much of the rock it is. */
	private record Ore(String stone, String deep, int minY, int maxY, double threshold, int salt) {
	}

	// thresholds of a vein noise in [-1, 1]: higher is rarer (UndergroundTest prints how much rock each is)
	private static final Ore[] ORES = {
		new Ore("minecraft:coal_ore", "minecraft:deepslate_coal_ore", 0 - SHIFT, 192 - SHIFT, 0.75, 11),
		new Ore("minecraft:iron_ore", "minecraft:deepslate_iron_ore", -64 - SHIFT, 72 - SHIFT, 0.75, 23),
		new Ore("minecraft:copper_ore", "minecraft:deepslate_copper_ore", -16 - SHIFT, 112 - SHIFT, 0.76, 37),
		new Ore("minecraft:gold_ore", "minecraft:deepslate_gold_ore", -64 - SHIFT, 32 - SHIFT, 0.78, 41),
		new Ore("minecraft:redstone_ore", "minecraft:deepslate_redstone_ore", -64 - SHIFT, 16 - SHIFT, 0.75, 53),
		new Ore("minecraft:lapis_ore", "minecraft:deepslate_lapis_ore", -64 - SHIFT, 64 - SHIFT, 0.80, 67),
		new Ore("minecraft:diamond_ore", "minecraft:deepslate_diamond_ore", -64 - SHIFT, 16 - SHIFT, 0.82, 79),
	};

	private Underground() {
	}

	/**
	 * The block a cell of natural rock is.
	 *
	 * @param surface the Proto.DIG_* material of the ground it was dug down from (dirt and sand go on under it)
	 * @param depth blocks from the cell up to the surface over it (Half-Life's first triangle), Double.POSITIVE_INFINITY
	 *        for none near
	 * @return a block id; {@link #AIR} for a cave
	 */
	public static String blockAt(long seed, int x, int y, int z, int surface, double depth) {
		if (depth >= CAVE_MIN_DEPTH && cave(seed, x, y, z)) {
			return AIR;
		}
		if (depth < SOIL_DEPTH && surface == DIG_SAND) {
			return "minecraft:sand";
		}
		if (depth < SOIL_DEPTH && surface == DIG_DIRT) {
			return "minecraft:dirt";
		}
		if (depth < SANDSTONE_DEPTH && surface == DIG_SAND) {
			return "minecraft:sandstone";
		}
		boolean deep = y < -SHIFT;
		if (depth >= 1.0) {
			for (Ore ore : ORES) {
				if (y >= ore.minY() && y <= ore.maxY() && noise(seed + ore.salt(), x * 0.33, y * 0.33, z * 0.33) > ore.threshold()) {
					return deep ? ore.deep() : ore.stone();
				}
			}
		}
		return deep ? "minecraft:deepslate" : "minecraft:stone";
	}

	/** Whether the cell is in a cave: a tunnel where two noises are both near zero, or a cavern where a third is high. */
	static boolean cave(long seed, int x, int y, int z) {
		double a = noise(seed + 101, x / 22.0, y / 14.0, z / 22.0);
		double b = noise(seed + 211, x / 22.0, y / 14.0, z / 22.0);
		if (Math.abs(a) < 0.07 && Math.abs(b) < 0.07) {
			return true;
		}
		return noise(seed + 307, x / 40.0, y / 24.0, z / 40.0) > 0.62;
	}

	/** Smooth 3D value noise in [-1, 1]: a hash at each lattice point, blended with a smoothstep. */
	static double noise(long seed, double x, double y, double z) {
		int x0 = (int) Math.floor(x), y0 = (int) Math.floor(y), z0 = (int) Math.floor(z);
		double fx = smooth(x - x0), fy = smooth(y - y0), fz = smooth(z - z0);
		double c000 = lattice(seed, x0, y0, z0), c100 = lattice(seed, x0 + 1, y0, z0);
		double c010 = lattice(seed, x0, y0 + 1, z0), c110 = lattice(seed, x0 + 1, y0 + 1, z0);
		double c001 = lattice(seed, x0, y0, z0 + 1), c101 = lattice(seed, x0 + 1, y0, z0 + 1);
		double c011 = lattice(seed, x0, y0 + 1, z0 + 1), c111 = lattice(seed, x0 + 1, y0 + 1, z0 + 1);
		double x00 = c000 + (c100 - c000) * fx, x10 = c010 + (c110 - c010) * fx;
		double x01 = c001 + (c101 - c001) * fx, x11 = c011 + (c111 - c011) * fx;
		double y0v = x00 + (x10 - x00) * fy, y1v = x01 + (x11 - x01) * fy;
		return y0v + (y1v - y0v) * fz;
	}

	private static double smooth(double t) {
		return t * t * (3.0 - 2.0 * t);
	}

	/** A lattice point's value in [-1, 1]. */
	private static double lattice(long seed, int x, int y, int z) {
		long h = seed;
		h ^= x * 0x9E3779B97F4A7C15L;
		h = Long.rotateLeft(h, 27) * 0xC2B2AE3D27D4EB4FL;
		h ^= y * 0x165667B19E3779F9L;
		h = Long.rotateLeft(h, 31) * 0x85EBCA77C2B2AE63L;
		h ^= z * 0x27D4EB2F165667C5L;
		h ^= h >>> 33;
		h *= 0xFF51AFD7ED558CCDL;
		h ^= h >>> 33;
		return (h >>> 11) * 0x1.0p-53 * 2.0 - 1.0;
	}
}
