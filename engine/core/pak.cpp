#include "core/pak.h"

#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

const char kMagic[8] = { 'F', 'E', 'P', 'A', 'K', 0, 0, 0 };

void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

struct MemSource { const uint8_t* data; size_t size; };

bool mem_read(void* user, uint64_t offset, void* dst, size_t n) {
    MemSource* m = (MemSource*)user;
    if (offset > m->size || n > m->size - offset) return false;
    memcpy(dst, m->data + offset, n);
    return true;
}

void mem_close(void* user) { free(user); }

// Path order: plain byte order, so the reader can binary search.
int path_compare(const char* a, uint32_t al, const char* b, uint32_t bl) {
    uint32_t n = al < bl ? al : bl;
    int c = memcmp(a, b, n);
    if (c) return c;
    return al < bl ? -1 : (al > bl ? 1 : 0);
}

}  // namespace

uint32_t pak_crc32(const void* data, size_t size, uint32_t seed) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = ~seed;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

PakSource pak_memory_source(const void* data, size_t size) {
    static_assert(sizeof(MemSource) > 0, "");
    PakSource s;
    // The MemSource lives inside the source's user pointer; allocated, freed by close().
    MemSource* m = (MemSource*)malloc(sizeof(MemSource));
    if (m) { m->data = (const uint8_t*)data; m->size = size; }
    s.user = m;
    s.read_at = m ? mem_read : nullptr;
    s.size = size;
    s.close = mem_close;
    return s;
}

// ---------------------------------------------------------------- reader

bool PakReader::open(const PakSource& src) {
    close();
    src_ = src;
    auto fail = [&](const char* why) { error_ = why; PakSource s = src_; src_ = PakSource(); free(index_); index_ = nullptr; count_ = 0; if (s.close) s.close(s.user); return false; };
    if (!src.read_at) return fail("no source");
    uint8_t h[kPakHeaderSize];
    if (src.size < kPakHeaderSize || !src.read_at(src.user, 0, h, sizeof h)) return fail("file too short");
    if (memcmp(h, kMagic, 8) != 0) return fail("not a .fepak file");
    uint32_t version = get32(h + 8);
    if (version == 0 || version > kPakVersion) return fail("made by a newer version");
    if (get32(h + 12) < kPakHeaderSize) return fail("bad header size");
    uint64_t index_off = get64(h + 16), index_size = get64(h + 24);
    uint32_t count = get32(h + 32);
    uint64_t file_size = get64(h + 48);
    if (file_size != src.size) return fail("file size differs from the header (truncated?)");
    if (index_off > src.size || index_size > src.size - index_off) return fail("index outside the file");
    if ((uint64_t)count * kPakEntrySize > index_size) return fail("index too small");
    if (index_size > (1u << 28)) return fail("index too large");
    index_ = (uint8_t*)malloc(index_size ? (size_t)index_size : 1);
    if (!index_ || !src.read_at(src.user, index_off, index_, (size_t)index_size)) return fail("cannot read the index");
    if (pak_crc32(index_, (size_t)index_size) != get32(h + 44)) return fail("index checksum mismatch");
    count_ = count;
    strtab_ = (uint64_t)count * kPakEntrySize;
    strtab_size_ = index_size - strtab_;
    for (uint32_t i = 0; i < count; i++) {
        PakEntryInfo e;
        if (!entry_at(i, &e)) return fail("bad entry");
        if (e.offset > src.size || e.size > src.size - e.offset) return fail("entry outside the file");
    }
    error_ = "";
    return true;
}

void PakReader::close() {
    free(index_);
    index_ = nullptr;
    count_ = 0;
    PakSource s = src_;
    src_ = PakSource();
    if (s.close) s.close(s.user);
}

bool PakReader::entry_at(uint32_t i, PakEntryInfo* out) const {
    const uint8_t* p = index_ + (size_t)i * kPakEntrySize;
    uint32_t name_off = get32(p + 24);
    uint32_t name_len = get16(p + 28);
    if ((uint64_t)name_off + name_len > strtab_size_) return false;
    out->path = (const char*)index_ + strtab_ + name_off;
    out->path_len = name_len;
    out->offset = get64(p);
    out->size = get64(p + 8);
    out->raw_size = get64(p + 16);
    out->flags = get16(p + 30);
    out->crc32 = get32(p + 32);
    return true;
}

bool PakReader::entry(uint32_t i, PakEntryInfo* out) const { return i < count_ && entry_at(i, out); }

bool PakReader::find(const char* path, PakEntryInfo* out) const {
    uint32_t len = (uint32_t)strlen(path);
    uint32_t lo = 0, hi = count_;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        PakEntryInfo e;
        if (!entry_at(mid, &e)) return false;
        int c = path_compare(e.path, e.path_len, path, len);
        if (c == 0) { if (out) *out = e; return true; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return false;
}

bool PakReader::read_range(const PakEntryInfo& e, uint64_t offset, void* dst, size_t n) const {
    if ((e.flags & kPakStoreMask) != kPakStoreRaw) return false;
    if (offset > e.size || n > e.size - offset) return false;
    return src_.read_at && src_.read_at(src_.user, e.offset + offset, dst, n);
}

void* PakReader::read(const PakEntryInfo& e, size_t* size) const {
    if ((e.flags & kPakStoreMask) != kPakStoreRaw) return nullptr;     // zstd comes later
    uint8_t* data = (uint8_t*)malloc((size_t)e.size + 1);
    if (!data) return nullptr;
    if (e.size && !read_range(e, 0, data, (size_t)e.size)) { free(data); return nullptr; }
    data[e.size] = 0;
    if (pak_crc32(data, (size_t)e.size) != e.crc32) { free(data); return nullptr; }
    if (size) *size = (size_t)e.size;
    return data;
}

void* PakReader::read(const char* path, size_t* size) const {
    PakEntryInfo e;
    return find(path, &e) ? read(e, size) : nullptr;
}

// ---------------------------------------------------------------- writer

PakWriter::~PakWriter() {
    for (int i = 0; i < count_; i++) { free(items_[i].path); free(items_[i].data); }
    free(items_);
}

bool PakWriter::has(const char* path) const {
    for (int i = 0; i < count_; i++)
        if (strcmp(items_[i].path, path) == 0) return true;
    return false;
}

bool PakWriter::add(const char* path, const void* data, size_t size) {
    size_t len = strlen(path);
    if (len == 0 || len > 255 || path[0] == '/') return false;
    int at = -1;
    for (int i = 0; i < count_; i++)
        if (strcmp(items_[i].path, path) == 0) { at = i; break; }
    uint8_t* copy = (uint8_t*)malloc(size ? size : 1);
    if (!copy) return false;
    if (size) memcpy(copy, data, size);
    if (at >= 0) {
        free(items_[at].data);
        items_[at].data = copy;
        items_[at].size = size;
        return true;
    }
    if (count_ == cap_) {
        int nc = cap_ ? cap_ * 2 : 32;
        Item* n = (Item*)realloc(items_, sizeof(Item) * (size_t)nc);
        if (!n) { free(copy); return false; }
        items_ = n;
        cap_ = nc;
    }
    char* p = (char*)malloc(len + 1);
    if (!p) { free(copy); return false; }
    memcpy(p, path, len + 1);
    items_[count_++] = Item{ p, (uint32_t)len, copy, (uint64_t)size };
    return true;
}

uint8_t* PakWriter::build(size_t* size) const {
    // Sort by path (an index array: the items stay where they are).
    int* order = (int*)malloc(sizeof(int) * (size_t)(count_ ? count_ : 1));
    if (!order) return nullptr;
    for (int i = 0; i < count_; i++) order[i] = i;
    for (int i = 1; i < count_; i++) {         // insertion sort: a game has hundreds of files, not millions
        int v = order[i], j = i - 1;
        while (j >= 0 && path_compare(items_[order[j]].path, items_[order[j]].path_len, items_[v].path, items_[v].path_len) > 0) { order[j + 1] = order[j]; j--; }
        order[j + 1] = v;
    }
    // Layout: header, data (aligned), index.
    uint64_t pos = kPakHeaderSize;
    uint64_t* offs = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)(count_ ? count_ : 1));
    uint64_t strsize = 0;
    if (!offs) { free(order); return nullptr; }
    for (int k = 0; k < count_; k++) {
        const Item& it = items_[order[k]];
        uint64_t a = it.size >= kPakChunkAlign ? kPakChunkAlign : kPakSmallAlign;
        pos = (pos + a - 1) / a * a;
        offs[k] = pos;
        pos += it.size;
        strsize += it.path_len;
    }
    uint64_t index_off = (pos + 15) / 16 * 16;
    uint64_t index_size = (uint64_t)count_ * kPakEntrySize + strsize;
    uint64_t total = index_off + index_size;
    uint8_t* out = (uint8_t*)calloc(1, (size_t)total ? (size_t)total : 1);
    if (!out) { free(order); free(offs); return nullptr; }
    uint8_t* idx = out + index_off;
    uint32_t name_off = 0;
    for (int k = 0; k < count_; k++) {
        const Item& it = items_[order[k]];
        if (it.size) memcpy(out + offs[k], it.data, (size_t)it.size);
        uint8_t* e = idx + (size_t)k * kPakEntrySize;
        put64(e, offs[k]);
        put64(e + 8, it.size);
        put64(e + 16, it.size);
        put32(e + 24, name_off);
        put16(e + 28, (uint16_t)it.path_len);
        put16(e + 30, kPakStoreRaw);
        put32(e + 32, pak_crc32(it.data, (size_t)it.size));
        memcpy(idx + (uint64_t)count_ * kPakEntrySize + name_off, it.path, it.path_len);
        name_off += it.path_len;
    }
    uint8_t* h = out;
    memcpy(h, kMagic, 8);
    put32(h + 8, kPakVersion);
    put32(h + 12, kPakHeaderSize);
    put64(h + 16, index_off);
    put64(h + 24, index_size);
    put32(h + 32, (uint32_t)count_);
    put32(h + 36, 0);
    put32(h + 40, kPakChunkAlign);
    put32(h + 44, pak_crc32(idx, (size_t)index_size));
    put64(h + 48, total);
    free(order);
    free(offs);
    if (size) *size = (size_t)total;
    return out;
}

}  // namespace fe
