package dev.halfcraft.world;

import java.lang.foreign.MemorySegment;
import org.jspecify.annotations.Nullable;

/**
 * Where HostCollision reads Half-Life's collision ring from: the client's own link to its Half-Life (HostLink) in
 * single player, or a server link to a Source dedicated server's server.so (ServerLink) on the HalfCraft server.
 */
public interface CollisionSource {
	/** The mapping, or null while it isn't open. */
	@Nullable
	MemorySegment segment();

	/** How far Half-Life has written the ring. */
	long head();

	/** How far it has been read. */
	long tail();

	/** Marks the ring read up to tail. */
	void setTail(long tail);

	/** The ring lost its place: Half-Life streams its collision afresh (EV_COLLISION_LOST). */
	void lost();
}
