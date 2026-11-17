// The virtual file system: "virtual path -> bytes" for everything a game
// loads (scenes, Lua scripts, .mof models, textures, audio, shaders).
//
// A path is project relative with forward slashes ("scripts/spin.lua").
// Lookups go to the mounted packages (.fepak, core/pak.h) first, then, when
// disk fallback is on (the editor), to plain files under the project folder
// through a reader the platform provides.
//
// Not thread safe: mount before the game starts.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fe::vfs {

// How plain files are read (editor/platform/platform.h's read_file and
// file_exists on both systems). The reader returns malloc'd bytes.
struct Disk {
    void* (*read_file)(const char* path, size_t* size) = nullptr;
    bool (*file_exists)(const char* path) = nullptr;
};
void set_disk(const Disk& disk);
// Folder that relative virtual paths are joined to for the disk fallback
// ("" = the current directory). Paths that already start with it are kept.
void set_root(const char* dir);
const char* root();
// Plain files are used when no package has the path (default: on).
void set_disk_fallback(bool on);

// Mounts a package by file path (opened through the disk reader's
// open hook) or from a random-access source. Later mounts win.
struct PakMountSource;
bool mount_pak_file(const char* file_path, const char** error = nullptr);
bool mount_pak_memory(const void* data, size_t size, const char** error = nullptr);   // data must outlive the mount
void unmount_all();
int mounted_paks();

// Optional: random access reads for mount_pak_file. Without them the whole
// package is read into memory.
struct FileAccess {
    void* (*open)(const char* path, uint64_t* size) = nullptr;
    bool (*read_at)(void* handle, uint64_t offset, void* dst, size_t n) = nullptr;
    void (*close)(void* handle) = nullptr;
};
void set_file_access(const FileAccess& fa);

// Turns a path into a virtual one: backslashes to '/', "./" and "//"
// removed, the project root stripped. Result in `out`.
void normalize(const char* path, char* out, size_t cap);

// Whole file as malloc'd bytes with one zero byte after them; null when
// missing. size may be null.
void* read(const char* path, size_t* size);
bool exists(const char* path);
// Adapter for the engine's read callbacks (renderer, audio): `user` unused.
void* read_cb(const char* path, size_t* size, void* user);

}  // namespace fe::vfs
