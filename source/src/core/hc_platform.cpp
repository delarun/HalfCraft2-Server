#include "hc_platform.h"

#ifdef _WIN32
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#endif

namespace halfcraft
{
#ifdef _WIN32
	std::uint64_t tick_ms()
	{
		return ::GetTickCount64();
	}

	std::int64_t qpc_now()
	{
		LARGE_INTEGER now;
		::QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	std::int64_t qpc_frequency()
	{
		static const std::int64_t frequency = [] {
			LARGE_INTEGER f;
			::QueryPerformanceFrequency(&f);
			return f.QuadPart;
		}();
		return frequency;
	}

	std::uint32_t process_id()
	{
		return ::GetCurrentProcessId();
	}

	namespace
	{
		std::wstring widen(const char* ascii)
		{
			std::wstring wide;
			for (; *ascii; ++ascii) {
				wide += static_cast<wchar_t>(static_cast<unsigned char>(*ascii));
			}
			return wide;
		}

		std::wstring utf8_to_wide(const char* utf8)
		{
			const int    chars = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
			std::wstring wide(static_cast<std::size_t>(std::max(chars, 1)), L'\0');
			if (chars > 0) {
				::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), chars);
			}
			wide.resize(wide.size() - 1);  // its terminator
			return wide;
		}
	}

	std::optional<std::string> environment_variable(const char* name)
	{
		const std::wstring wide_name = widen(name);
		const DWORD        size = ::GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
		if (size <= 1) {
			return std::nullopt;
		}
		std::wstring wide(size, L'\0');
		const DWORD  length = ::GetEnvironmentVariableW(wide_name.c_str(), wide.data(), size);
		if (length == 0 || length >= size) {
			return std::nullopt;
		}
		const int   bytes = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
		std::string utf8(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
		if (bytes > 0) {
			::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(length), utf8.data(), bytes, nullptr, nullptr);
		}
		return utf8;
	}

	bool set_environment_variable(const char* name, const char* value)
	{
		const std::wstring wide = value ? utf8_to_wide(value) : std::wstring();
		return ::SetEnvironmentVariableW(widen(name).c_str(), value ? wide.c_str() : nullptr) != FALSE;
	}

	unsigned long last_error()
	{
		return ::GetLastError();
	}
#else
	namespace
	{
		std::int64_t monotonic_ns()
		{
			timespec now{};
			::clock_gettime(CLOCK_MONOTONIC, &now);
			return static_cast<std::int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
		}
	}

	std::uint64_t tick_ms()
	{
		return static_cast<std::uint64_t>(monotonic_ns() / 1000000);
	}

	std::int64_t qpc_now()
	{
		return monotonic_ns();
	}

	std::int64_t qpc_frequency()
	{
		return 1000000000LL;
	}

	std::uint32_t process_id()
	{
		return static_cast<std::uint32_t>(::getpid());
	}

	std::optional<std::string> environment_variable(const char* name)
	{
		const char* value = std::getenv(name);
		if (!value || !*value) {
			return std::nullopt;
		}
		return std::string(value);
	}

	bool set_environment_variable(const char* name, const char* value)
	{
		return (value ? ::setenv(name, value, 1) : ::unsetenv(name)) == 0;
	}

	unsigned long last_error()
	{
		return static_cast<unsigned long>(errno);
	}
#endif
}
