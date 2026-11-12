package dev.halfcraft.link;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import org.junit.jupiter.api.Test;

class LinkNamesTest {
	@Test
	void linuxNamesAreWhatFollowsTheNamespace() {
		assertEquals("/HalfCraft_v1", LinkNames.posixName(LinkNames.of("").orElseThrow().mapping()));
		LinkNames b = LinkNames.of("b").orElseThrow();
		assertEquals("/HalfCraft_v1.b", LinkNames.posixName(b.mapping()));
		assertEquals("/HalfCraft_v1.b_minecraft", LinkNames.posixName(b.minecraftMutex()));
		assertEquals("/HalfCraft_v1", LinkNames.posixName("HalfCraft_v1"));
	}

	@Test
	void noIdKeepsTheNamesHalfCraftAlwaysHad() {
		for (String none : new String[] { null, "" }) {
			LinkNames names = LinkNames.of(none).orElseThrow();
			assertEquals("", names.id());
			assertEquals(Proto.MAPPING_NAME, names.mapping());
			assertEquals("Local\\HalfCraft_v1", names.mapping());
			assertEquals("Local\\HalfCraft_v1_frame", names.frameEvent());
			assertEquals("Local\\HalfCraft_v1_minecraft", names.minecraftMutex());
		}
	}

	@Test
	void anIdGetsALinkOfItsOwn() {
		LinkNames names = LinkNames.of("b").orElseThrow();
		assertEquals("b", names.id());
		assertEquals("Local\\HalfCraft_v1.b", names.mapping());
		assertEquals("Local\\HalfCraft_v1.b_frame", names.frameEvent());
		assertEquals("Local\\HalfCraft_v1.b_minecraft", names.minecraftMutex());
		String longest = "Pair_2-of-3_ABCDEFGHIJKLMNOPQRST";
		assertEquals(LinkNames.MAX_ID_LENGTH, longest.length());
		assertEquals("Local\\HalfCraft_v1." + longest, LinkNames.of(longest).orElseThrow().mapping());
	}

	@Test
	void validIdsAreShortAndPlain() {
		assertTrue(LinkNames.validId("b"));
		assertTrue(LinkNames.validId("AZaz09_-"));
		assertTrue(LinkNames.validId("x".repeat(LinkNames.MAX_ID_LENGTH)));
		assertFalse(LinkNames.validId(null));
		assertFalse(LinkNames.validId(""));
		assertFalse(LinkNames.validId("x".repeat(LinkNames.MAX_ID_LENGTH + 1)));
		// a mapping name's own characters, a space, a line break, other letters and a nul aren't an id's
		for (String bad : List.of("b c", "a.b", "a/b", "a\\b", "a:b", "*", "b\n", "é", "Ｂ", "б", "a\0b")) {
			assertFalse(LinkNames.validId(bad), bad);
		}
	}

	@Test
	void anIdThatIsntOneNamesNothing() {
		// never the default link's names: that link would be another pair's
		assertTrue(LinkNames.of("b c").isEmpty());
		assertTrue(LinkNames.of("x".repeat(LinkNames.MAX_ID_LENGTH + 1)).isEmpty());
		assertTrue(LinkNames.of("Ｂ").isEmpty());
	}
}
