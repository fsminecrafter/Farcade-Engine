// .mpkg writer: the installable package of Minimal-OS (Deliver, the `pkg`
// terminal command, mos_pkg_unzip). Layout from Minimal-OS's
// src/intf/x86_64/pkgformat.h: a 16 byte header ("MPKG0001", version 1,
// entry count), 148 byte entries (name[128], flags, method, uncompressed size,
// compressed size, data offset) and the payloads. Files are stored raw
// (method 0), which every reader accepts.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fe {

class MpkgWriter {
public:
    ~MpkgWriter();
    MpkgWriter() = default;
    MpkgWriter(const MpkgWriter&) = delete;
    MpkgWriter& operator=(const MpkgWriter&) = delete;
    // Relative paths with '/', at most 127 bytes ("programs/Game.run"). False when invalid.
    bool add_dir(const char* path);
    bool add_file(const char* path, const void* data, size_t size);
    int count() const { return count_; }
    uint8_t* build(size_t* size) const;     // malloc'd

private:
    struct Item { char name[128]; bool dir; uint8_t* data; uint32_t size; };
    Item* items_ = nullptr;
    int count_ = 0, cap_ = 0;
    Item* push();
};

}  // namespace fe
