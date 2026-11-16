#include "gfx/shader_file.h"

#include <stdio.h>
#include <string.h>

namespace fe::gfx {
namespace {

enum class Stage { None, Vertex, Fragment };

bool marker(const char* line, size_t size, const char* name) {
    while (size && (*line == ' ' || *line == '\t')) { line++; size--; }
    const char* tag = "// @";
    size_t tag_size = 4;
    if (size < tag_size || memcmp(line, tag, tag_size) != 0) return false;
    line += tag_size;
    size -= tag_size;
    size_t name_size = strlen(name);
    if (size < name_size || memcmp(line, name, name_size) != 0) return false;
    line += name_size;
    size -= name_size;
    while (size && (*line == ' ' || *line == '\t' || *line == '\r')) { line++; size--; }
    return size == 0;
}

bool append(char* out, size_t cap, size_t* used, const char* text, size_t count) {
    if (*used >= cap || count >= cap - *used) return false;
    memcpy(out + *used, text, count);
    *used += count;
    out[*used] = 0;
    return true;
}

void fail(char* error, size_t cap, const char* message) {
    if (error && cap) snprintf(error, cap, "%s", message);
}

}  // namespace

bool parse_shader_file(const char* source, size_t size,
                       char* vertex, size_t vertex_cap,
                       char* fragment, size_t fragment_cap,
                       char* error, size_t error_cap) {
    if (error && error_cap) error[0] = 0;
    if (vertex && vertex_cap) vertex[0] = 0;
    if (fragment && fragment_cap) fragment[0] = 0;
    if (!source || !vertex || !vertex_cap || !fragment || !fragment_cap) {
        fail(error, error_cap, "invalid shader output buffers");
        return false;
    }

    Stage stage = Stage::None;
    bool saw_vertex = false, saw_fragment = false;
    size_t vertex_size = 0, fragment_size = 0;
    for (size_t pos = 0; pos < size;) {
        size_t end = pos;
        while (end < size && source[end] != '\n') end++;
        size_t line_size = end - pos;
        if (line_size && source[pos + line_size - 1] == '\r') line_size--;
        if (marker(source + pos, line_size, "vertex")) {
            if (saw_vertex) { fail(error, error_cap, "duplicate vertex section"); return false; }
            saw_vertex = true;
            stage = Stage::Vertex;
        } else if (marker(source + pos, line_size, "fragment")) {
            if (saw_fragment) { fail(error, error_cap, "duplicate fragment section"); return false; }
            saw_fragment = true;
            stage = Stage::Fragment;
        } else if (stage == Stage::Vertex) {
            if (!append(vertex, vertex_cap, &vertex_size, source + pos, end - pos + (end < size ? 1 : 0))) {
                fail(error, error_cap, "vertex section is too large");
                return false;
            }
        } else if (stage == Stage::Fragment) {
            if (!append(fragment, fragment_cap, &fragment_size, source + pos, end - pos + (end < size ? 1 : 0))) {
                fail(error, error_cap, "fragment section is too large");
                return false;
            }
        } else if (line_size) {
            fail(error, error_cap, "shader source must start with // @vertex");
            return false;
        }
        pos = end < size ? end + 1 : size;
    }
    if (!saw_vertex || !vertex_size) { fail(error, error_cap, "missing or empty vertex section"); return false; }
    if (!saw_fragment || !fragment_size) { fail(error, error_cap, "missing or empty fragment section"); return false; }
    return true;
}

}  // namespace fe::gfx
