package dev.halfcraft.link;

import java.util.Optional;

/**
 * The names of a link's shared objects (Proto.MAPPING_NAME's doc): its mapping, Half-Life's frame event and the
 * mutex this Minecraft holds while it runs. A link id gives a pair (a Half-Life and its Minecraft) a link of its
 * own, so two pairs can run on one PC:
 *
 * <pre>
 *   no id    Local\HalfCraft_v1      Local\HalfCraft_v1_frame      Local\HalfCraft_v1_minecraft
 *   id b     Local\HalfCraft_v1.b    Local\HalfCraft_v1.b_frame    Local\HalfCraft_v1.b_minecraft
 * </pre>
 *
 * Minecraft takes HALFCRAFT_LINK's id (Half-Life sets it for the Minecraft it starts). An id that isn't one gives
 * no names at all: the default link's would be another pair's. Half-Life's core/hc_link_names.h and
 * tools/halfcraft/link.py build the same names.
 *
 * @param id "" for the default link
 */
public record LinkNames(String id, String mapping, String frameEvent, String minecraftMutex) {
	public static final String VARIABLE = "HALFCRAFT_LINK";
	public static final int MAX_ID_LENGTH = 32;

	/** A link id: 1 to MAX_ID_LENGTH characters, each of A-Z, a-z, 0-9, _ and -. */
	public static boolean validId(String id) {
		if (id == null || id.isEmpty() || id.length() > MAX_ID_LENGTH) {
			return false;
		}
		for (int i = 0; i < id.length(); i++) {
			char c = id.charAt(i);
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
				return false;
			}
		}
		return true;
	}

	/**
	 * A link's names: id's, or the default link's for none (null or "", as an unset or empty HALFCRAFT_LINK is).
	 * Empty when id is given and isn't one.
	 */
	public static Optional<LinkNames> of(String id) {
		if (id == null || id.isEmpty()) {
			return Optional.of(named("", Proto.MAPPING_NAME));
		}
		return validId(id) ? Optional.of(named(id, Proto.MAPPING_NAME + "." + id)) : Optional.empty();
	}

	/**
	 * A link object's name on Linux (POSIX shared memory, /dev/shm): what follows Windows' namespace (Local\),
	 * after a slash. Local\HalfCraft_v1.b is /HalfCraft_v1.b, the file /dev/shm/HalfCraft_v1.b. Half-Life's
	 * core/hc_link_names.h (posix_name) builds the same.
	 */
	public static String posixName(String windowsName) {
		return "/" + windowsName.substring(windowsName.lastIndexOf('\\') + 1);
	}

	private static LinkNames named(String id, String mapping) {
		return new LinkNames(id, mapping, mapping + "_frame", mapping + "_minecraft");
	}
}
