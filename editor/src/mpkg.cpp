#include "mpkg.h"

#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
bool valid_name(const char* n) {
    size_t len = strlen(n);
    return len > 0 && len < 128 && n[0] != '/' && !strstr(n, "..") && !strchr(n, '\\');
}
}  // namespace

MpkgWriter::~MpkgWriter() {
    for (int i = 0; i < count_; i++) free(items_[i].data);
    free(items_);
}

MpkgWriter::Item* MpkgWriter::push() {
    if (count_ == cap_) {
        int nc = cap_ ? cap_ * 2 : 16;
        Item* n = (Item*)realloc(items_, sizeof(Item) * (size_t)nc);
        if (!n) return nullptr;
        items_ = n;
        cap_ = nc;
    }
    Item* it = &items_[count_++];
    memset(it, 0, sizeof *it);
    return it;
}

bool MpkgWriter::add_dir(const char* path) {
    if (!valid_name(path)) return false;
    Item* it = push();
    if (!it) return false;
    strcpy(it->name, path);
    it->dir = true;
    return true;
}

bool MpkgWriter::add_file(const char* path, const void* data, size_t size) {
    if (!valid_name(path) || size > 0xFFFFFFFFu) return false;
    uint8_t* copy = (uint8_t*)malloc(size ? size : 1);
    if (!copy) return false;
    if (size) memcpy(copy, data, size);
    Item* it = push();
    if (!it) { free(copy); return false; }
    strcpy(it->name, path);
    it->data = copy;
    it->size = (uint32_t)size;
    return true;
}

uint8_t* MpkgWriter::build(size_t* size) const {
    size_t table = 16 + (size_t)count_ * 148, total = table;
    for (int i = 0; i < count_; i++) total += items_[i].size;
    if (total > 0xFFFFFFFFu) return nullptr;
    uint8_t* out = (uint8_t*)calloc(1, total);
    if (!out) return nullptr;
    memcpy(out, "MPKG0001", 8);
    put32(out + 8, 1);
    put32(out + 12, (uint32_t)count_);
    size_t at = table;
    for (int i = 0; i < count_; i++) {
        uint8_t* e = out + 16 + (size_t)i * 148;
        memcpy(e, items_[i].name, strlen(items_[i].name));
        put32(e + 128, items_[i].dir ? 1u : 0u);
        put32(e + 132, 0);                              // method: store
        put32(e + 136, items_[i].size);
        put32(e + 140, items_[i].size);
        put32(e + 144, items_[i].dir ? 0u : (uint32_t)at);
        if (items_[i].size) memcpy(out + at, items_[i].data, items_[i].size);
        at += items_[i].size;
    }
    if (size) *size = total;
    return out;
}

}  // namespace fe
