package dev.halfcraft.props;

import dev.halfcraft.link.HostSide;
import dev.halfcraft.link.Proto;
import dev.halfcraft.link.ServerPlayers;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.InteractionResult;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.Level;

/**
 * A Half-Life prop as a Minecraft item (HostProps): using it puts the prop into Half-Life's world where the player
 * looks (Proto.EV_PLACE_PROP; server.dll finds the room), as a physics prop the gravity gun throws about. Minecraft's
 * hits break such a prop down into its item again.
 */
public final class PropItem extends Item {
	private final int kind;

	public PropItem(Properties properties, int kind) {
		super(properties);
		this.kind = kind;
	}

	/** Its Proto.PROP_* kind. */
	public int kind() {
		return this.kind;
	}

	@Override
	public net.minecraft.network.chat.Component getName(ItemStack stack) {
		PropData data = stack.get(HostProps.PROP_DATA);
		return data != null ? net.minecraft.network.chat.Component.translatable("item.halfcraft.hl_prop.named", data.shortName()) : super.getName(stack);
	}

	@Override
	public InteractionResult use(Level level, Player player, InteractionHand hand) {
		if (!HostSide.active()) {
			return InteractionResult.PASS; // no Half-Life to put it in
		}
		ItemStack held = player.getItemInHand(hand);
		PropData data = held.get(HostProps.PROP_DATA);
		if (this.kind == Proto.PROP_NONE && data == null) {
			return InteractionResult.FAIL; // a model's item without its model
		}
		if (player instanceof ServerPlayer server) {
			int userId = ServerPlayers.userId(server);
			int propClass = data != null ? data.propClass() : Proto.PROP_CLASS_PHYSICS;
			if (this.kind == Proto.PROP_NONE) {
				sendModel(data.model(), userId);
			}
			HostSide.pushEvent(Proto.EV_PLACE_PROP, 0, 0.0F, 0.0F, 0.0F, 0.0F, 0, this.kind, propClass, userId);
			ItemStack stack = held;
			if (!player.getAbilities().instabuild) {
				stack.shrink(1);
			}
			player.getCooldowns().addCooldown(stack, HostProps.PLACE_COOLDOWN_TICKS);
		}
		return InteractionResult.SUCCESS;
	}

	/** The model's path to Half-Life in Proto.EV_PROP_MODEL pieces of 24 bytes, before the place that uses it. */
	private static void sendModel(String model, int userId) {
		byte[] bytes = model.getBytes(java.nio.charset.StandardCharsets.UTF_8);
		for (int piece = 0; piece * 24 < bytes.length; piece++) {
			int[] words = new int[6];
			for (int i = 0; i < 24 && piece * 24 + i < bytes.length; i++) {
				words[i / 4] |= (bytes[piece * 24 + i] & 0xFF) << (8 * (i % 4));
			}
			// the text's bytes ride in the event's floats as they are: a path's ascii is never a float's NaN
			HostSide.pushEvent(Proto.EV_PROP_MODEL, piece, Float.intBitsToFloat(words[0]), Float.intBitsToFloat(words[1]), Float.intBitsToFloat(words[2]),
				Float.intBitsToFloat(words[3]), words[4], words[5], bytes.length, userId);
		}
	}
}
