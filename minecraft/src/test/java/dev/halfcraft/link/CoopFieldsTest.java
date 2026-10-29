package dev.halfcraft.link;

import static dev.halfcraft.link.Proto.*;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

/** The co-op fields go through the link's structs whole: written, then read back by another copy of the struct. */
class CoopFieldsTest {
	private Arena arena;

	@BeforeEach
	void map() {
		arena = Arena.ofConfined();
	}

	@AfterEach
	void unmap() {
		arena.close();
	}

	@Test
	void hostStateCarriesTheCoopFields() {
		MemorySegment shm = arena.allocate(0x400);
		HostLink.HostState sent = new HostLink.HostState();
		// the fields on either side of the new ones, so a field written over its neighbour shows
		sent.overlayDivisor = 3;
		sent.coopMode = COOP_GUEST;
		sent.coopIp = 0xC0A8021F;
		sent.coopPort = 25565;
		sent.write(shm, 0x100);

		HostLink.HostState got = new HostLink.HostState();
		got.read(shm, 0x100);
		assertEquals(COOP_GUEST, got.coopMode);
		assertEquals(0xC0A8021F, got.coopIp);
		assertEquals(25565, got.coopPort);
		assertEquals(3, got.overlayDivisor);
	}

	@Test
	void mcStateCarriesTheCoopFields() {
		MemorySegment shm = arena.allocate(0x400);
		HostLink.McState sent = new HostLink.McState();
		sent.teleportCount = 7;
		sent.coopState = COOP_JOINED;
		sent.coopPort = 25565;
		sent.write(shm, 0x200);

		HostLink.McState got = new HostLink.McState();
		got.read(shm, 0x200);
		assertEquals(COOP_JOINED, got.coopState);
		assertEquals(25565, got.coopPort);
		assertEquals(7, got.teleportCount);
	}
}
