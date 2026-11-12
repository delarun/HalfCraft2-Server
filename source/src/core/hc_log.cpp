#include "hc_log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace halfcraft
{
	namespace
	{
		/// before the host points the log at its console: the debugger on windows, stderr on linux
		void debugger_sink(int /*severity*/, const char* line)
		{
#ifdef _WIN32
			::OutputDebugStringA(line);
			::OutputDebugStringA("\n");
#else
			std::fprintf(stderr, "%s\n", line);
#endif
		}

		std::atomic<LogSink> g_sink{ debugger_sink };

		void write(int severity, const char* format, std::va_list args)
		{
			char      line[1024];
			const int prefix = std::snprintf(line, sizeof(line), "[halfcraft] ");
			std::vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
			g_sink.load(std::memory_order_acquire)(severity, line);
		}
	}

	void set_log_sink(LogSink sink)
	{
		g_sink.store(sink ? sink : debugger_sink, std::memory_order_release);
	}

	void log_info(const char* format, ...)
	{
		std::va_list args;
		va_start(args, format);
		write(LOG_INFO, format, args);
		va_end(args);
	}

	void log_warning(const char* format, ...)
	{
		std::va_list args;
		va_start(args, format);
		write(LOG_WARNING, format, args);
		va_end(args);
	}

	void log_error(const char* format, ...)
	{
		std::va_list args;
		va_start(args, format);
		write(LOG_ERROR, format, args);
		va_end(args);
	}
}
