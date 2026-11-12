package dev.halfcraft.world;

import static dev.halfcraft.link.Proto.*;
import static org.junit.jupiter.api.Assertions.*;

import java.util.Map;
import java.util.TreeMap;
import org.junit.jupiter.api.Test;

class UndergroundTest {
	private static final long SEED = 12345L;
	private static final double DEEP = 100.0;

	@Test
	void theSameCellIsTheSameBlockEveryTime() {
		for (int i = 0; i < 1000; i++) {
			assertEquals(Underground.blockAt(SEED, i * 7, -i, i * 3, DIG_STONE, DEEP), Underground.blockAt(SEED, i * 7, -i, i * 3, DIG_STONE, DEEP));
		}
	}

	@Test
	void theGroundGoesOnUnderItsSurface() {
		assertEquals("minecraft:sand", Underground.blockAt(SEED, 0, 10, 0, DIG_SAND, 1.5));
		assertEquals("minecraft:sandstone", Underground.blockAt(SEED, 0, 10, 0, DIG_SAND, 4.0));
		assertEquals("minecraft:dirt", Underground.blockAt(SEED, 0, 10, 0, DIG_DIRT, 0.5));
	}

	@Test
	void noCaveRightUnderTheSurface() {
		for (int x = 0; x < 64; x++) {
			for (int z = 0; z < 64; z++) {
				assertNotEquals(Underground.AIR, Underground.blockAt(SEED, x, 0, z, DIG_DIRT, 2.0));
			}
		}
	}

	@Test
	void deepslateAndItsOresDeepDown() {
		assertTrue(Underground.blockAt(SEED, 5, -200, 5, DIG_STONE, DEEP).contains("deepslate") || Underground.blockAt(SEED, 5, -200, 5, DIG_STONE, DEEP).equals(Underground.AIR));
	}

	/** How much of the rock each block is, over a slab of heights: the ores rare, the caves a share, stone the rest. */
	@Test
	void oresAndCavesAreAShareOfTheRock() {
		Map<String, Integer> counts = new TreeMap<>();
		int total = 0;
		for (int y = -120; y < 30; y += 3) {
			for (int x = 0; x < 96; x++) {
				for (int z = 0; z < 96; z++) {
					counts.merge(Underground.blockAt(SEED, x, y, z, DIG_STONE, DEEP), 1, Integer::sum);
					total++;
				}
			}
		}
		int finalTotal = total;
		counts.forEach((block, n) -> System.out.printf("%-40s %6.3f%%%n", block, 100.0 * n / finalTotal));
		double air = counts.getOrDefault(Underground.AIR, 0) / (double) total;
		assertTrue(air > 0.01 && air < 0.2, "caves: " + air);
		double coal = (counts.getOrDefault("minecraft:coal_ore", 0) + counts.getOrDefault("minecraft:deepslate_coal_ore", 0)) / (double) total;
		assertTrue(coal > 0.002 && coal < 0.03, "coal: " + coal);
		double diamond = (counts.getOrDefault("minecraft:diamond_ore", 0) + counts.getOrDefault("minecraft:deepslate_diamond_ore", 0)) / (double) total;
		assertTrue(diamond > 0.0 && diamond < 0.004, "diamond: " + diamond);
	}
}
