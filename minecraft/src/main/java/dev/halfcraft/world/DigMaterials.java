package dev.halfcraft.world;

import static dev.halfcraft.link.Proto.*;

/** The Minecraft block each Proto.DIG_* material of Half-Life's walls digs as, and back. */
public final class DigMaterials {
	private static final String STONE = "minecraft:stone";
	private static final String STONE_BRICKS = "minecraft:stone_bricks";
	private static final String IRON = "minecraft:iron_block";
	private static final String PLANKS = "minecraft:oak_planks";
	private static final String DIRT = "minecraft:dirt";
	private static final String GLASS = "minecraft:glass";
	private static final String SAND = "minecraft:sand";

	private DigMaterials() {
	}

	/** The block's id; stone for DIG_STONE, DIG_NONE and a material it doesn't know. */
	public static String blockId(int material) {
		return switch (material) {
			case DIG_STONE_BRICKS -> STONE_BRICKS;
			case DIG_IRON -> IRON;
			case DIG_PLANKS -> PLANKS;
			case DIG_DIRT -> DIRT;
			case DIG_GLASS -> GLASS;
			case DIG_SAND -> SAND;
			default -> STONE;
		};
	}

	/** The material a block is of: DIG_STONE for any block but those of the other materials. */
	public static int material(String blockId) {
		return switch (blockId) {
			case STONE_BRICKS -> DIG_STONE_BRICKS;
			case IRON -> DIG_IRON;
			case PLANKS -> DIG_PLANKS;
			case DIRT -> DIG_DIRT;
			case GLASS -> DIG_GLASS;
			case SAND -> DIG_SAND;
			default -> DIG_STONE;
		};
	}
}
