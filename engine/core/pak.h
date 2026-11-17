// .fepak v1: the game package (docs/pak-format.md).
//
// PakWriter collects files (virtual path + bytes) and produces the whole
// package in memory. PakReader opens a package through a random-access
// callback (a file, or a block of memory) and reads single entries.
//
// Built for the host and for Minimal-OS: <stdint.h>, <stddef.h> and a few
// libc calls (malloc, memcpy, strcmp) only.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fe {

constexpr uint32_t kPakVersion = 1;
constexpr uint32_t kPakHeaderSize = 64;
constexpr uint32_t kPakEntrySize = 40;
constexpr uint32_t kPakChunkAlign = 64 * 1024;      // data of files at least this big starts on one
constexpr uint32_t kPakSmallAlign = 16;

// Entry flags (low 4 bits: how the data is stored).
enum : uint16_t {
    kPakStoreRaw = 0,
    kPakStoreZstd = 1,          // reserved: readers of v1 reject it
    kPakStoreMask = 0xF,
};

struct PakEntryInfo {
    const char* path;           // points into the reader (not NUL terminated: use path_len)
    uint32_t path_len;
    uint64_t offset;
    uint64_t size;              // bytes stored in the package
    uint64_t raw_size;          // bytes after decoding
    uint16_t flags;
    uint32_t crc32;             // of the raw bytes
};

uint32_t pak_crc32(const void* data, size_t size, uint32_t seed = 0);

// Random access to the package's bytes.
struct PakSource {
    void* user = nullptr;
    // Reads exactly n bytes at `offset`; false when that is not possible.
    bool (*read_at)(void* user, uint64_t offset, void* dst, size_t n) = nullptr;
    uint64_t size = 0;
    // Called once by PakReader::close(); may be null.
    void (*close)(void* user) = nullptr;
};

// A PakSource over a block of memory (not copied; the block must outlive the reader).
PakSource pak_memory_source(const void* data, size_t size);

class PakReader {
public:
    PakReader() = default;
    ~PakReader() { close(); }
    PakReader(const PakReader&) = delete;
    PakReader& operator=(const PakReader&) = delete;

    // Reads and checks the header and the index. False on any problem; error() says which.
    bool open(const PakSource& src);
    void close();
    bool is_open() const { return src_.read_at != nullptr; }
    const char* error() const { return error_; }

    uint32_t count() const { return count_; }
    bool entry(uint32_t index, PakEntryInfo* out) const;
    // Binary search; the path is a virtual path ("scripts/a.lua").
    bool find(const char* path, PakEntryInfo* out) const;
    // Reads the raw bytes of an entry into malloc'd memory (one extra byte,
    // zero, after the data so text can be used as a C string). Verifies the
    // checksum. Null when the entry is missing or damaged.
    void* read(const PakEntryInfo& e, size_t* size) const;
    void* read(const char* path, size_t* size) const;
    // Reads part of an entry (for streaming).
    bool read_range(const PakEntryInfo& e, uint64_t offset, void* dst, size_t n) const;

private:
    PakSource src_;
    uint8_t* index_ = nullptr;          // entries, then the string table
    uint32_t count_ = 0;
    uint64_t strtab_ = 0;               // offset of the string table in index_
    uint64_t strtab_size_ = 0;
    const char* error_ = "";
    bool entry_at(uint32_t i, PakEntryInfo* out) const;
};

class PakWriter {
public:
    PakWriter() = default;
    ~PakWriter();
    PakWriter(const PakWriter&) = delete;
    PakWriter& operator=(const PakWriter&) = delete;

    // Adds (or replaces) a file. The data is copied. Paths use '/', have no
    // leading '/', and are at most 255 bytes.
    bool add(const char* path, const void* data, size_t size);
    int count() const { return count_; }
    bool has(const char* path) const;
    // Builds the package; malloc'd, its size in *size. Null on failure.
    uint8_t* build(size_t* size) const;

private:
    struct Item { char* path; uint32_t path_len; uint8_t* data; uint64_t size; };
    Item* items_ = nullptr;
    int count_ = 0, cap_ = 0;
};

}  // namespace fe
