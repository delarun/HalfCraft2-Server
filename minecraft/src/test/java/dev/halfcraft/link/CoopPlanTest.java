package dev.halfcraft.link;

import static dev.halfcraft.link.CoopPlan.Action.*;
import static dev.halfcraft.link.CoopPlan.next;
import static dev.halfcraft.link.Proto.*;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import dev.halfcraft.link.CoopPlan.Now;
import dev.halfcraft.link.CoopPlan.Wanted;
import org.junit.jupiter.api.Test;

class CoopPlanTest {
	private static final Wanted G = new Wanted(COOP_GUEST, 0xC0A8021F, 25565);
	private static final Wanted H = new Wanted(COOP_HOST, 0, 25565);
	private static final Wanted S = new Wanted(COOP_SOLO, 0, 0);

	@Test
	void addressIsDottedWithItsPort() {
		assertEquals("192.168.2.31:25565", CoopPlan.address(0xC0A8021F, 25565));
	}

	@Test
	void soloLeavesTheHostsWorld() {
		assertEquals(LEAVE, next(S, new Now(false, false, 0, "192.168.2.31:25565", false, 0), 0));
	}

	@Test
	void soloClosesItsLanWorld() {
		assertEquals(CLOSE, next(S, new Now(true, true, 25565, null, false, 0), 0));
	}

	@Test
	void hostOpensItsWorld() {
		assertEquals(OPEN, next(H, new Now(true, false, 0, null, false, 0), 0));
	}

	@Test
	void hostWaitsForItsOwnWorld() {
		assertEquals(NONE, next(H, new Now(false, false, 0, null, false, 0), 0));
	}

	@Test
	void hostTriesAgainOnlyAfterTheRetryTime() {
		Now failed = new Now(true, false, 0, null, false, 1000);
		assertEquals(NONE, next(H, failed, 1000 + COOP_RETRY_MS - 1));
		assertEquals(OPEN, next(H, failed, 1000 + COOP_RETRY_MS));
	}

	@Test
	void hostReopensOnAnotherPort() {
		assertEquals(CLOSE, next(H, new Now(true, true, 25566, null, false, 0), 0));
	}

	@Test
	void guestWaitsForTheHostsPort() {
		assertEquals(WAIT, next(new Wanted(COOP_GUEST, 0xC0A8021F, 0), new Now(true, false, 0, null, false, 0), 0));
	}

	@Test
	void guestLeavesItsOwnWorldFirst() {
		assertEquals(LEAVE, next(G, new Now(true, false, 0, null, false, 0), 0));
	}

	@Test
	void guestJoins() {
		assertEquals(JOIN, next(G, new Now(false, false, 0, null, false, 0), 0));
	}

	@Test
	void guestStaysJoined() {
		assertEquals(NONE, next(G, new Now(false, false, 0, "192.168.2.31:25565", false, 0), 0));
	}

	@Test
	void guestMovesToAnotherHost() {
		assertEquals(LEAVE, next(G, new Now(false, false, 0, "10.0.0.5:25565", false, 0), 0));
	}

	@Test
	void guestRetriesOnlyAfterTheRetryTime() {
		Now failed = new Now(false, false, 0, null, false, 1000);
		assertEquals(WAIT, next(G, failed, 1000 + COOP_RETRY_MS - 1));
		assertEquals(JOIN, next(G, failed, 1000 + COOP_RETRY_MS));
	}

	@Test
	void statesAreWhatHalfLifeIsTold() {
		assertEquals(COOP_OPEN, CoopPlan.state(H, new Now(true, true, 25565, null, false, 0)));
		assertEquals(COOP_FAILED, CoopPlan.state(H, new Now(true, false, 0, null, false, 1000)));
		assertEquals(COOP_JOINING, CoopPlan.state(G, new Now(false, false, 0, null, true, 0)));
		assertEquals(COOP_JOINED, CoopPlan.state(G, new Now(false, false, 0, "192.168.2.31:25565", false, 0)));
		assertEquals(COOP_IDLE, CoopPlan.state(S, new Now(true, false, 0, null, false, 0)));
	}

	// beyond the brief: the rest of each mode's rules

	@Test
	void soloInItsOwnWorldDoesNothing() {
		assertEquals(NONE, next(S, new Now(true, false, 0, null, false, 0), 0));
		assertEquals(NONE, next(S, new Now(false, false, 0, null, false, 0), 0));
	}

	@Test
	void soloStopsAJoinUnderWay() {
		assertEquals(LEAVE, next(S, new Now(false, false, 0, null, true, 0), 0));
	}

	@Test
	void hostLeavesAnotherHostsWorldFirst() {
		assertEquals(LEAVE, next(H, new Now(false, false, 0, "10.0.0.5:25565", false, 0), 0));
		assertEquals(LEAVE, next(H, new Now(false, false, 0, null, true, 0), 0));
	}

	@Test
	void hostStaysOpenOnItsPort() {
		assertEquals(NONE, next(H, new Now(true, true, 25565, null, false, 0), 0));
	}

	@Test
	void hostWithoutAPortDoesntOpen() {
		assertEquals(NONE, next(new Wanted(COOP_HOST, 0, 0), new Now(true, false, 0, null, false, 0), 0));
	}

	@Test
	void guestWaitsForTheHostsAddress() {
		assertEquals(WAIT, next(new Wanted(COOP_GUEST, 0, 25565), new Now(false, false, 0, null, false, 0), 0));
	}

	@Test
	void guestWaitsWhileItConnects() {
		assertEquals(WAIT, next(G, new Now(false, false, 0, null, true, 0), 0));
	}

	@Test
	void anUnknownModeIsSolo() {
		Wanted odd = new Wanted(7, 0xC0A8021F, 25565);
		assertEquals(LEAVE, next(odd, new Now(false, false, 0, "192.168.2.31:25565", false, 0), 0));
		assertEquals(COOP_IDLE, CoopPlan.state(odd, new Now(true, true, 25565, null, false, 0)));
		assertTrue(CoopPlan.wantsOwnWorld(odd));
	}

	@Test
	void onlyAGuestGivesUpItsOwnWorld() {
		assertTrue(CoopPlan.wantsOwnWorld(S));
		assertTrue(CoopPlan.wantsOwnWorld(H));
		assertFalse(CoopPlan.wantsOwnWorld(G));
	}

	@Test
	void moreStatesAreWhatHalfLifeIsTold() {
		assertEquals(COOP_IDLE, CoopPlan.state(H, new Now(true, false, 0, null, false, 0)));
		assertEquals(COOP_IDLE, CoopPlan.state(S, new Now(true, true, 25565, null, false, 1000)));
		assertEquals(COOP_FAILED, CoopPlan.state(G, new Now(false, false, 0, null, false, 1000)));
		assertEquals(COOP_IDLE, CoopPlan.state(G, new Now(true, false, 0, null, false, 0)));
		// in another host's world, on the way out: not joined to this one
		assertEquals(COOP_IDLE, CoopPlan.state(G, new Now(false, false, 0, "10.0.0.5:25565", false, 0)));
	}

	@Test
	void addressTakesEveryByteUnsigned() {
		assertEquals("255.0.128.1:65535", CoopPlan.address(0xFF008001, 65535));
		assertEquals("10.0.0.5:25565", CoopPlan.address(0x0A000005, 25565));
	}
}
