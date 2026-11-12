#pragma once

#include <cstdint>
#include <optional>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// what halfcraft asks of the operating system outside the link itself: clocks, the process id and its
// environment. windows and linux each implement it in hc_platform.cpp; minecraft's HostLink reads the same
// clocks (on linux CLOCK_MONOTONIC, on windows GetTickCount64 and QueryPerformanceCounter), so stamps one
// side writes into the mapping mean the same to the other.

namespace halfcraft
{
	/// milliseconds on the clock heartbeats are stamped with (GetTickCount64 on windows, CLOCK_MONOTONIC on
	/// linux): it only grows, and is the same clock in every process of this machine.
	std::uint64_t tick_ms();

	/// the high-resolution clock minecraft stamps its ticks with (QueryPerformanceCounter on windows,
	/// CLOCK_MONOTONIC in nanoseconds on linux).
	std::int64_t qpc_now();

	/// ticks of qpc_now() per second.
	std::int64_t qpc_frequency();

	/// this process's id.
	std::uint32_t process_id();

	/// one of this process's environment variables in utf-8; nullopt when it isn't set or is empty.
	std::optional<std::string> environment_variable(const char* name);

	/// sets one of this process's environment variables, or takes it out (value nullptr), for what the
	/// process starts from then on.
	/// @return false when the system refused
	bool set_environment_variable(const char* name, const char* value);

	/// the system's code for why the last call failed (GetLastError, errno).
	unsigned long last_error();

	/// a hint to the cpu that this thread spins waiting for another (_mm_pause on x86).
	inline void cpu_pause()
	{
#if defined(_MSC_VER)
		_mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
		__builtin_ia32_pause();
#endif
	}
}
