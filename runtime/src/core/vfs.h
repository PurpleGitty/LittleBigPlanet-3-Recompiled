// Maps PS4 paths onto host folders.
//
//   /app0/...       read-only, layered: mods/, then the patch dump, then the base dump
//   /savedata0/...  the save folder mounted by libSceSaveData (see Mount)
//   /temp0/...      user/temp0/
//   /data/...       user/data/
//   /download0/...  user/download0/
//
// Relative paths are relative to /app0, like on the console.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace core::vfs {

// `app0Layers` in priority order (first wins).
void Init(std::vector<std::filesystem::path> app0Layers, std::filesystem::path userRoot);

enum class Status { Ok, NotFound, ReadOnly, BadPath };

struct Resolved {
    Status status = Status::BadPath;
    std::filesystem::path host;   // where it is (or would be created)
    std::string guest;            // normalised guest path
    bool writable = false;
};

// For reads of /app0, `host` is the highest-priority layer that has the path.
Resolved Resolve(std::string_view guestPath);

// Serve the startup logo/intro movies as an empty video (--skip-intro).
void SetSkipIntro(bool skip);

// The user/ folder that writable game data lives under.
std::filesystem::path UserRoot();

// Points a mount (e.g. "/savedata0") at a host folder until Unmount. These
// take priority over the fixed mounts above.
void Mount(const std::string& mountPoint, const std::filesystem::path& host, bool writable);
void Unmount(const std::string& mountPoint);
bool IsMounted(const std::string& mountPoint);

// Host folders that make up a guest directory, highest priority first
// (several for /app0 directories, one elsewhere).
std::vector<std::filesystem::path> DirectoryLayers(const Resolved& r);

}  // namespace core::vfs
