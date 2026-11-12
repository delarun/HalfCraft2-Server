package dev.halfcraft.props;

import com.mojang.serialization.Codec;
import com.mojang.serialization.codecs.RecordCodecBuilder;
import io.netty.buffer.ByteBuf;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;

/**
 * What a Half-Life object's item ({@code halfcraft:hl_prop}) puts back: its model ("models/....mdl") and its
 * Proto.PROP_CLASS_* (a physics prop, a door, a dynamic prop). An item component, kept with the stack.
 */
public record PropData(String model, int propClass) {
	public static final Codec<PropData> CODEC = RecordCodecBuilder.create(i -> i.group(Codec.STRING.fieldOf("model").forGetter(PropData::model),
		Codec.INT.fieldOf("class").forGetter(PropData::propClass)).apply(i, PropData::new));
	public static final StreamCodec<ByteBuf, PropData> STREAM_CODEC = StreamCodec.composite(ByteBufCodecs.STRING_UTF8, PropData::model, ByteBufCodecs.VAR_INT,
		PropData::propClass, PropData::new);

	/** The model's file name without its folders and extension ("oildrum001"), for the item's name. */
	public String shortName() {
		String name = this.model.substring(this.model.lastIndexOf('/') + 1);
		return name.endsWith(".mdl") ? name.substring(0, name.length() - 4) : name;
	}
}
