package dev.halfcraft.link;

import static dev.halfcraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.halfcraft.HalfCraft;
import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;

/**
 * What HostLink asks of the operating system: opening Half-Life's mapping, waiting for its frames, the
 * running-Minecraft mark and the clocks both sides stamp the mapping with. Windows has named kernel objects
 * (Local\...); Linux has POSIX shared memory in /dev/shm, a futex on Header.hostFrame and a lock on a file
 * (Proto.MAPPING_NAME's doc). Half-Life's core/hc_platform.cpp and hc_link.cpp are the other end.
 */
sealed interface LinkSystem permits LinkSystem.Windows, LinkSystem.Linux {
	/** The one for this machine. */
	static LinkSystem current() {
		String os = System.getProperty("os.name", "").toLowerCase(java.util.Locale.ROOT);
		return os.startsWith("windows") ? new Windows() : new Linux();
	}

	/**
	 * Half-Life's whole mapping, or null while it isn't there to open (said in the log once per reason).
	 */
	MemorySegment openMapping(LinkNames link);

	/**
	 * Sleeps until Half-Life finishes a frame or ms pass. False when there is nothing to wait on: the caller naps
	 * and polls instead.
	 */
	boolean waitForHostFrame(MemorySegment shm, int ms);

	/** Marks this Minecraft as running on the link for as long as it runs (a launcher then starts no other). */
	void announceRunning(LinkNames link);

	/** Milliseconds on the clock heartbeats are stamped with. */
	long tickCount();

	/** The high-resolution clock tick times are stamped with. */
	long qpc();

	/** Ticks of {@link #qpc()} per second. */
	long qpcFrequency();

	/** This process's id. */
	default int pid() {
		return (int) ProcessHandle.current().pid();
	}

	/** Windows: named mapping, auto-reset frame event, named mutex, GetTickCount64 and the performance counter. */
	final class Windows implements LinkSystem {
		private static final int FILE_MAP_ALL_ACCESS = 0xF001F;
		private static final int SYNCHRONIZE = 0x00100000;

		private final MethodHandle openFileMapping;
		// OpenFileMappingW's GetLastError, captured right after the call (the JVM may change it later).
		private final StructLayout callState = Linker.Option.captureStateLayout();
		private final VarHandle lastError = this.callState.varHandle(MemoryLayout.PathElement.groupElement("GetLastError"));
		private final MemorySegment openState = Arena.global().allocate(this.callState);
		private int lastOpenError = -1;
		private final MethodHandle mapViewOfFile;
		private final MethodHandle getTickCount64;
		private final MethodHandle queryPerformanceCounter;
		private final MethodHandle queryPerformanceFrequency;
		private final MethodHandle createMutex;
		private final MethodHandle openEvent;
		private final MethodHandle waitForSingleObject;
		private final MemorySegment qpcOut = Arena.global().allocate(JAVA_LONG);
		private MemorySegment runningMutex;
		// Half-Life's frame event; null before it's open, or from a Half-Life without one
		private volatile MemorySegment frameEvent;
		private LinkNames link;
		private long lastEventAttempt;

		Windows() {
			Linker linker = Linker.nativeLinker();
			SymbolLookup k32 = SymbolLookup.libraryLookup("kernel32", Arena.global());
			this.openFileMapping = linker.downcallHandle(
				k32.find("OpenFileMappingW").orElseThrow(), FunctionDescriptor.of(ADDRESS, JAVA_INT, JAVA_INT, ADDRESS), Linker.Option.captureCallState("GetLastError")
			);
			this.mapViewOfFile = linker.downcallHandle(
				k32.find("MapViewOfFile").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG)
			);
			this.getTickCount64 = linker.downcallHandle(k32.find("GetTickCount64").orElseThrow(), FunctionDescriptor.of(JAVA_LONG));
			this.queryPerformanceCounter = linker.downcallHandle(k32.find("QueryPerformanceCounter").orElseThrow(), FunctionDescriptor.of(JAVA_INT, ADDRESS));
			this.queryPerformanceFrequency = linker.downcallHandle(k32.find("QueryPerformanceFrequency").orElseThrow(), FunctionDescriptor.of(JAVA_INT, ADDRESS));
			this.createMutex = linker.downcallHandle(k32.find("CreateMutexW").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, ADDRESS));
			this.openEvent = linker.downcallHandle(k32.find("OpenEventW").orElseThrow(), FunctionDescriptor.of(ADDRESS, JAVA_INT, JAVA_INT, ADDRESS));
			this.waitForSingleObject = linker.downcallHandle(k32.find("WaitForSingleObject").orElseThrow(), FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT));
		}

		@Override
		public MemorySegment openMapping(LinkNames link) {
			this.link = link;
			try (Arena arena = Arena.ofConfined()) {
				MemorySegment name = arena.allocateFrom(link.mapping(), StandardCharsets.UTF_16LE);
				MemorySegment handle = (MemorySegment) this.openFileMapping.invokeExact(this.openState, FILE_MAP_ALL_ACCESS, 0, name);
				if (handle.address() == 0) {
					// Say why, once per reason: 2 is "Half-Life hasn't made it yet" (normal while it loads),
					// 5 is "not allowed" (a Half-Life run as administrator, before 0.1.1).
					int error = (int) this.lastError.get(this.openState, 0L);
					if (error != this.lastOpenError) {
						this.lastOpenError = error;
						HalfCraft.LOG.info("HalfCraft: can't open Half-Life's shared memory yet (Windows error {}{})", error,
							error == 2 ? ": Half-Life hasn't created it yet" : error == 5 ? ": access denied; is Half-Life running as administrator?" : "");
					}
					return null;
				}
				MemorySegment view = (MemorySegment) this.mapViewOfFile.invokeExact(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0L);
				if (view.address() == 0) {
					HalfCraft.LOG.error("HalfCraft: MapViewOfFile failed");
					return null;
				}
				return view.reinterpret(MAPPING_BYTES);
			} catch (Throwable t) {
				HalfCraft.LOG.error("HalfCraft: failed to open shared memory", t);
				return null;
			}
		}

		@Override
		public boolean waitForHostFrame(MemorySegment shm, int ms) {
			MemorySegment event = this.frameEvent;
			if (event == null) {
				this.openFrameEvent();
				return false;
			}
			try {
				int result = (int) this.waitForSingleObject.invokeExact(event, ms);
				return result != -1; // WAIT_FAILED
			} catch (Throwable t) {
				return false;
			}
		}

		// the frame event, once the mapping is open (at most once a second): it outlives a Half-Life restart while
		// this holds it, as the mapping does
		private void openFrameEvent() {
			long now = System.currentTimeMillis();
			if (this.link == null || now - this.lastEventAttempt < 1000) {
				return;
			}
			this.lastEventAttempt = now;
			try (Arena arena = Arena.ofConfined()) {
				MemorySegment name = arena.allocateFrom(this.link.frameEvent(), StandardCharsets.UTF_16LE);
				MemorySegment handle = (MemorySegment) this.openEvent.invokeExact(SYNCHRONIZE, 0, name);
				if (handle.address() != 0) {
					this.frameEvent = handle;
					HalfCraft.LOG.info("HalfCraft: waiting on Half-Life's frame event between frames");
				}
			} catch (Throwable t) {
				HalfCraft.LOG.warn("HalfCraft: couldn't open Half-Life's frame event", t);
			}
		}

		@Override
		public synchronized void announceRunning(LinkNames link) {
			if (this.runningMutex != null) {
				return;
			}
			try (Arena arena = Arena.ofConfined()) {
				MemorySegment name = arena.allocateFrom(link.minecraftMutex(), StandardCharsets.UTF_16LE);
				this.runningMutex = (MemorySegment) this.createMutex.invokeExact(MemorySegment.NULL, 0, name);
			} catch (Throwable t) {
				HalfCraft.LOG.warn("HalfCraft: couldn't create the running-Minecraft mutex", t);
			}
		}

		@Override
		public long tickCount() {
			try {
				return (long) this.getTickCount64.invokeExact();
			} catch (Throwable t) {
				throw new RuntimeException(t);
			}
		}

		@Override
		public synchronized long qpc() {
			try {
				int ok = (int) this.queryPerformanceCounter.invokeExact(this.qpcOut);
				return this.qpcOut.get(JAVA_LONG, 0);
			} catch (Throwable t) {
				throw new RuntimeException(t);
			}
		}

		@Override
		public synchronized long qpcFrequency() {
			try {
				int ok = (int) this.queryPerformanceFrequency.invokeExact(this.qpcOut);
				return this.qpcOut.get(JAVA_LONG, 0);
			} catch (Throwable t) {
				throw new RuntimeException(t);
			}
		}
	}

	/**
	 * Linux: the mapping is /dev/shm/&lt;LinkNames.posixName(mapping)&gt;, Half-Life's frames are a futex on
	 * Header.hostFrame, the running mark is a lock (fcntl's, as FileLock takes it) on the file of the mutex's posix
	 * name, and both clocks are CLOCK_MONOTONIC.
	 */
	final class Linux implements LinkSystem {
		private static final Path SHM = Path.of("/dev/shm");
		private static final long SYS_FUTEX = 202; // x86_64
		private static final int FUTEX_WAIT = 0;
		private static final int CLOCK_MONOTONIC = 1;
		private static final int EAGAIN = 11;
		private static final int EINTR = 4;
		private static final int ETIMEDOUT = 110;
		private static final VarHandle INT = JAVA_INT.varHandle();

		private final MethodHandle futex;
		private final MethodHandle clockGettime;
		private final StructLayout callState = Linker.Option.captureStateLayout();
		private final VarHandle errno = this.callState.varHandle(MemoryLayout.PathElement.groupElement("errno"));
		private String lastOpenProblem;
		private FileChannel runningFile;
		private FileLock runningLock;
		// the frame count this Minecraft last waited past: a frame since then ends the next wait at once, as an
		// auto-reset event Half-Life set while nobody waited does
		private int seenFrame;

		Linux() {
			Linker linker = Linker.nativeLinker();
			SymbolLookup libc = linker.defaultLookup();
			// long syscall(long number, ...): futex(uaddr, op, val, timeout, uaddr2, val3)
			this.futex = linker.downcallHandle(
				libc.find("syscall").orElseThrow(), FunctionDescriptor.of(JAVA_LONG, JAVA_LONG, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT),
				Linker.Option.firstVariadicArg(1), Linker.Option.captureCallState("errno")
			);
			this.clockGettime = linker.downcallHandle(libc.find("clock_gettime").orElseThrow(), FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS));
		}

		@Override
		public MemorySegment openMapping(LinkNames link) {
			Path file = SHM.resolve(LinkNames.posixName(link.mapping()).substring(1));
			try (FileChannel channel = FileChannel.open(file, StandardOpenOption.READ, StandardOpenOption.WRITE)) {
				long size = channel.size();
				if (size < MAPPING_BYTES) {
					// touching past its end would kill the JVM (SIGBUS): Half-Life is still making it, or is a
					// build of another layout
					this.sayOnce("HalfCraft: Half-Life's shared memory " + file + " is " + size + " bytes, not " + MAPPING_BYTES + " yet");
					return null;
				}
				// the mapping outlives the channel, as Half-Life's does its descriptor
				return channel.map(FileChannel.MapMode.READ_WRITE, 0, MAPPING_BYTES, Arena.global());
			} catch (NoSuchFileException e) {
				this.sayOnce("HalfCraft: can't open Half-Life's shared memory yet (" + file + " isn't there: Half-Life hasn't created it yet)");
				return null;
			} catch (IOException e) {
				this.sayOnce("HalfCraft: can't open Half-Life's shared memory " + file + " yet (" + e + ")");
				return null;
			}
		}

		private void sayOnce(String problem) {
			if (!problem.equals(this.lastOpenProblem)) {
				this.lastOpenProblem = problem;
				HalfCraft.LOG.info(problem);
			}
		}

		@Override
		public boolean waitForHostFrame(MemorySegment shm, int ms) {
			if (shm == null) {
				return false;
			}
			MemorySegment word = shm.asSlice(OFF_HEADER + H_HOST_FRAME, 4);
			int frame = (int) INT.getAcquire(word, 0L);
			if (frame != this.seenFrame) {
				this.seenFrame = frame;
				return true;
			}
			try (Arena arena = Arena.ofConfined()) {
				MemorySegment timeout = arena.allocate(16);
				timeout.set(JAVA_LONG, 0, ms / 1000);
				timeout.set(JAVA_LONG, 8, (ms % 1000) * 1_000_000L);
				MemorySegment state = arena.allocate(this.callState);
				long result = (long) this.futex.invokeExact(state, SYS_FUTEX, word, FUTEX_WAIT, frame, timeout, MemorySegment.NULL, 0);
				if (result != 0) {
					int error = (int) this.errno.get(state, 0L);
					if (error != EAGAIN && error != EINTR && error != ETIMEDOUT) {
						return false; // no futex here: nap and poll
					}
				}
				this.seenFrame = (int) INT.getAcquire(word, 0L);
				return true;
			} catch (Throwable t) {
				return false;
			}
		}

		@Override
		public synchronized void announceRunning(LinkNames link) {
			if (this.runningLock != null) {
				return;
			}
			Path file = SHM.resolve(LinkNames.posixName(link.minecraftMutex()).substring(1));
			try {
				this.runningFile = FileChannel.open(file, StandardOpenOption.CREATE, StandardOpenOption.READ, StandardOpenOption.WRITE);
				this.runningLock = this.runningFile.tryLock();
				if (this.runningLock == null) {
					HalfCraft.LOG.warn("HalfCraft: another Minecraft already runs on link {} ({} is locked)", link.id().isEmpty() ? "(default)" : link.id(), file);
				}
			} catch (IOException e) {
				HalfCraft.LOG.warn("HalfCraft: couldn't lock {} to mark this Minecraft as running", file, e);
			}
		}

		private long monotonicNanos() {
			try (Arena arena = Arena.ofConfined()) {
				MemorySegment now = arena.allocate(16);
				int ok = (int) this.clockGettime.invokeExact(CLOCK_MONOTONIC, now);
				return now.get(JAVA_LONG, 0) * 1_000_000_000L + now.get(JAVA_LONG, 8);
			} catch (Throwable t) {
				throw new RuntimeException(t);
			}
		}

		@Override
		public long tickCount() {
			return this.monotonicNanos() / 1_000_000L;
		}

		@Override
		public long qpc() {
			return this.monotonicNanos();
		}

		@Override
		public long qpcFrequency() {
			return 1_000_000_000L;
		}
	}
}
