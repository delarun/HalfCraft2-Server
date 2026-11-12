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
	public InteractionResult use(Level level, Player player, InteractionHand hand) {
		if (!HostSide.active()) {
			return InteractionResult.PASS; // no Half-Life to put it in
		}
		if (player instanceof ServerPlayer server) {
			HostSide.pushEvent(Proto.EV_PLACE_PROP, 0, 0.0F, 0.0F, 0.0F, 0.0F, 0, this.kind, 0, ServerPlayers.userId(server));
			ItemStack stack = player.getItemInHand(hand);
			if (!player.getAbilities().instabuild) {
				stack.shrink(1);
			}
			player.getCooldowns().addCooldown(stack, HostProps.PLACE_COOLDOWN_TICKS);
		}
		return InteractionResult.SUCCESS;
	}
}
