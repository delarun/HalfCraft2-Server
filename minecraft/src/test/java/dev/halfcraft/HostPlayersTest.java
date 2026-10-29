package dev.halfcraft;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.List;
import java.util.UUID;
import org.junit.jupiter.api.Test;

class HostPlayersTest {
	@Test
	void hostIsFoundWhereverItIsInTheList() {
		UUID host = UUID.randomUUID(), guest = UUID.randomUUID();
		assertEquals(0, HostPlayers.hostIndex(List.of(host, guest), host));
		assertEquals(1, HostPlayers.hostIndex(List.of(guest, host), host));  // the host respawned in Minecraft
	}

	@Test
	void noOwnerMeansNoHost() {
		assertEquals(-1, HostPlayers.hostIndex(List.of(UUID.randomUUID()), null));
	}

	@Test
	void anOwnerNotInYetIsNoHost() {
		assertEquals(-1, HostPlayers.hostIndex(List.of(UUID.randomUUID()), UUID.randomUUID()));
	}

	@Test
	void anEmptyListHasNoHost() {
		assertEquals(-1, HostPlayers.hostIndex(List.of(), UUID.randomUUID()));
	}

	@Test
	void aGuestIsNeverTheHostWhateverItsPlace() {
		UUID host = UUID.randomUUID(), guest = UUID.randomUUID(), other = UUID.randomUUID();
		// the host died and came back last, and a second guest joined after that
		List<UUID> players = List.of(guest, other, host);
		int index = HostPlayers.hostIndex(players, host);
		assertEquals(host, players.get(index));
		assertEquals(-1, HostPlayers.hostIndex(List.of(guest, other), host));
	}
}
