#pragma once

#include <stddef.h>

namespace fe::gfx {

// Reads a combined .fshader source with `// @vertex` and `// @fragment`
// section markers. The stage outputs are always NUL-terminated on success.
bool parse_shader_file(const char* source, size_t size,
                       char* vertex, size_t vertex_cap,
                       char* fragment, size_t fragment_cap,
                       char* error, size_t error_cap);

}  // namespace fe::gfx
