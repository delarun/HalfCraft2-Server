#include "hc_prism.h"

#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

extern char** environ;
#endif

namespace halfcraft::prism
{
	namespace
	{
		constexpr wchar_t INSTANCE[] = L"HalfCraft";

		std::filesystem::path launcher_exe(const std::filesystem::path& bundle)
		{
#ifdef _WIN32
			return bundle / L"Prism" / L"prismlauncher.exe";
#else
			return bundle / L"Prism" / L"prismlauncher";
#endif
		}

#ifdef _WIN32
		std::wstring error_text(DWORD code)
		{
			wchar_t*     text = nullptr;
			const DWORD  length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
				reinterpret_cast<wchar_t*>(&text), 0, nullptr);
			std::wstring result = length ? std::wstring(text, length) : L"error " + std::to_wstring(code);
			::LocalFree(text);
			while (!result.empty() && (result.back() == L'\n' || result.back() == L'\r' || result.back() == L'.')) {
				result.pop_back();
			}
			return result;
		}

#else
		std::wstring error_text(int code)
		{
			const std::string text = std::strerror(code);
			return std::wstring(text.begin(), text.end());  // ascii in the C locale prism's errors come from
		}
#endif

		bool same_file(const std::filesystem::path& a, const std::filesystem::path& b)
		{
			std::error_code ec;
			return std::filesystem::equivalent(a, b, ec);
		}
	}

	std::filesystem::path find_bundle(const std::filesystem::path& game_dir)
	{
		std::error_code ec;
		const auto      bundle = std::filesystem::weakly_canonical(game_dir / L".." / L"minecraft", ec);
		return !ec && std::filesystem::exists(launcher_exe(bundle), ec) ? bundle : std::filesystem::path{};
	}

#ifdef _WIN32
	bool minecraft_running(const LinkNames& link)
	{
		HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, link.minecraft_mutex.c_str());
		if (!mutex) {
			return ::GetLastError() == ERROR_ACCESS_DENIED;  // there, just not ours to open
		}
		::CloseHandle(mutex);
		return true;
	}

	bool launcher_running(const std::filesystem::path& bundle)
	{
		HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return false;
		}
		const auto      exe = launcher_exe(bundle);
		bool            found = false;
		PROCESSENTRY32W entry{ sizeof(entry) };
		for (BOOL more = ::Process32FirstW(snapshot, &entry); more && !found; more = ::Process32NextW(snapshot, &entry)) {
			if (_wcsicmp(entry.szExeFile, L"prismlauncher.exe") != 0) {
				continue;
			}
			// the player may run a prism of their own for other things: only this bundle's counts
			HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
			if (!process) {
				continue;
			}
			wchar_t image[MAX_PATH * 2];
			DWORD   length = static_cast<DWORD>(std::size(image));
			found = ::QueryFullProcessImageNameW(process, 0, image, &length) && same_file(std::filesystem::path(std::wstring(image, length)), exe);
			::CloseHandle(process);
		}
		::CloseHandle(snapshot);
		return found;
	}

#else
	bool minecraft_running(const LinkNames& link)
	{
		// minecraft holds a lock (fcntl's: java's FileLock) on /dev/shm/<the mutex's posix name> while it runs
		const std::string file = "/dev/shm" + posix_name(link.minecraft_mutex);
		const int         fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			return false;  // no minecraft ever ran on this link since the machine started
		}
		struct flock lock{};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		const bool held = ::fcntl(fd, F_GETLK, &lock) == 0 && lock.l_type != F_UNLCK;
		::close(fd);
		return held;
	}

	bool launcher_running(const std::filesystem::path& bundle)
	{
		// the player may run a prism of their own for other things: only this bundle's counts
		const auto      exe = launcher_exe(bundle);
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
			const auto name = entry.path().filename().string();
			if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
				continue;
			}
			std::error_code link_ec;
			const auto      image = std::filesystem::read_symlink(entry.path() / "exe", link_ec);
			if (!link_ec && same_file(image, exe)) {
				return true;
			}
		}
		return false;
	}
#endif

	bool signed_in(const std::filesystem::path& bundle)
	{
		// {"accounts": [ ... ], "formatVersion": 3}: prism writes it once an account was added
		std::ifstream in(bundle / L"Prism" / L"accounts.json", std::ios::binary);
		if (!in) {
			return false;
		}
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const auto        key = text.find("\"accounts\"");
		const auto        open = key == std::string::npos ? std::string::npos : text.find('[', key);
		const auto        first = open == std::string::npos ? std::string::npos : text.find_first_not_of(" \t\r\n", open + 1);
		return first != std::string::npos && text[first] != ']';
	}

	bool start(const std::filesystem::path& bundle, std::wstring& error)
	{
		const auto      exe = launcher_exe(bundle);
		const auto      dir = exe.parent_path();
		std::error_code ec;
		// prism's settings are the player's after its first start: only put the defaults in place once.
		// without them prism still runs, it just asks its first-start questions.
		const auto settings = dir / L"prismlauncher.cfg";
		if (!std::filesystem::exists(settings, ec)) {
			std::filesystem::copy_file(bundle / L"defaults" / L"prismlauncher.cfg", settings, ec);
		}

#ifdef _WIN32
		std::wstring        command = L"\"" + exe.wstring() + L"\" --launch " + INSTANCE;
		STARTUPINFOW        startup{ sizeof(startup) };
		PROCESS_INFORMATION process{};
		// out of whatever job the caller runs in, so half-life closing can't take minecraft down before
		// it has saved its world (it quits by itself a few seconds after half-life)
		BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, dir.c_str(), &startup, &process);
		if (!started && ::GetLastError() == ERROR_ACCESS_DENIED) {
			started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process);
		}
		if (!started) {
			error = exe.wstring() + L": " + error_text(::GetLastError());
			return false;
		}
		::CloseHandle(process.hThread);
		::CloseHandle(process.hProcess);
		return true;
#else
		// in a session of its own, so half-life closing can't take minecraft down before it has saved its world (it
		// quits by itself a few seconds after half-life), and from prism's folder, as on windows
		const std::string          program = exe.string();
		const std::string          instance(std::begin(INSTANCE), std::end(INSTANCE) - 1);
		const std::string          folder = dir.string();
		std::string                launch = "--launch";
		std::string                name = instance;
		char*                      argv[] = { const_cast<char*>(program.c_str()), launch.data(), name.data(), nullptr };
		posix_spawnattr_t          attributes;
		posix_spawn_file_actions_t actions;
		::posix_spawnattr_init(&attributes);
		::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
		::posix_spawn_file_actions_init(&actions);
		::posix_spawn_file_actions_addchdir_np(&actions, folder.c_str());
		pid_t     pid = 0;
		const int failed = ::posix_spawn(&pid, program.c_str(), &actions, &attributes, argv, environ);
		::posix_spawn_file_actions_destroy(&actions);
		::posix_spawnattr_destroy(&attributes);
		if (failed) {
			error = exe.wstring() + L": " + error_text(failed);
			return false;
		}
		return true;
#endif
	}
}
