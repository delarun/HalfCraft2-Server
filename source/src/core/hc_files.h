#pragma once

// what windows says about files that the engine's file system doesn't: whether one could be deleted right now, and the
// room left on a disk. paths as the engine gives them (RelativePathToFullPath): utf-8, or the ansi code page's, so each
// is tried, through windows' wide functions, as whichever names something that's there. on linux paths are utf-8, and a
// file open elsewhere can still be deleted. engine-agnostic.

namespace halfcraft
{
	enum class Deletable
	{
		kYes,      // nothing has it open
		kNo,       // something has it open
		kUnknown,  // windows finds nothing under the path, read either way
	};

	/// whether the file could be deleted now.
	Deletable file_deletable(const char* path);

	/// the bytes this user may still write to the disk a folder is on; -1 when windows won't say (or can't find it).
	long long free_bytes(const char* folder);
}
