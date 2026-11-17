#include "project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

// One line: key, then the rest; a value may be "quoted" (\" and \\ escapes).
bool split_line(char* line, char** key, char** value) {
    while (*line == ' ' || *line == '\t' || *line == '\r') line++;
    if (!*line || *line == '#') return false;
    *key = line;
    while (*line && *line != ' ' && *line != '\t') line++;
    if (*line) *line++ = 0;
    while (*line == ' ' || *line == '\t') line++;
    *value = line;
    char* end = line + strlen(line);
    while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    if (*line == '"') {
        char* w = line;
        char* r = line + 1;
        while (*r && *r != '"') {
            if (*r == '\\' && r[1]) r++;
            *w++ = *r++;
        }
        *w = 0;
    }
    return true;
}

void copy(char* dst, size_t cap, const char* src) { snprintf(dst, cap, "%s", src); }

void put_str(char* out, size_t cap, size_t* n, const char* key, const char* value) {
    char esc[600];
    size_t e = 0;
    for (const char* p = value; *p && e + 2 < sizeof esc; p++) {
        if (*p == '"' || *p == '\\') esc[e++] = '\\';
        esc[e++] = *p;
    }
    esc[e] = 0;
    *n += (size_t)snprintf(out ? out + *n : nullptr, out ? cap - *n : 0, "%s \"%s\"\n", key, esc);
}

}  // namespace

bool project_parse(const char* text, size_t size, ProjectSettings* out) {
    char* buf = (char*)malloc(size + 1);
    if (!buf) return false;
    memcpy(buf, text, size);
    buf[size] = 0;
    bool header = false;
    ProjectSettings p;
    char* s = buf;
    while (s && *s) {
        char* nl = strchr(s, '\n');
        if (nl) *nl = 0;
        char* line = s;
        s = nl ? nl + 1 : nullptr;
        char *key, *value;
        if (!split_line(line, &key, &value)) continue;
        if (!header) {
            if (strcmp(key, "farcade-project") != 0) { free(buf); return false; }
            header = true;
            continue;
        }
        if (strcmp(key, "name") == 0) copy(p.name, sizeof p.name, value);
        else if (strcmp(key, "version") == 0) copy(p.version, sizeof p.version, value);
        else if (strcmp(key, "entry_scene") == 0) copy(p.entry_scene, sizeof p.entry_scene, value);
        else if (strcmp(key, "icon") == 0) copy(p.icon, sizeof p.icon, value);
        else if (strcmp(key, "window_width") == 0) { int v = atoi(value); if (v >= 64 && v <= 8192) p.window_width = v; }
        else if (strcmp(key, "window_height") == 0) { int v = atoi(value); if (v >= 64 && v <= 8192) p.window_height = v; }
    }
    free(buf);
    if (!header) return false;
    *out = p;
    return true;
}

char* project_text(const ProjectSettings& p, size_t* size) {
    size_t cap = 2048;
    char* out = (char*)malloc(cap);
    if (!out) return nullptr;
    size_t n = (size_t)snprintf(out, cap, "farcade-project %d\n", kProjectFormatVersion);
    put_str(out, cap, &n, "name", p.name);
    put_str(out, cap, &n, "version", p.version);
    put_str(out, cap, &n, "entry_scene", p.entry_scene);
    n += (size_t)snprintf(out + n, cap - n, "window_width %d\nwindow_height %d\n", p.window_width, p.window_height);
    if (p.icon[0]) put_str(out, cap, &n, "icon", p.icon);
    if (size) *size = n;
    return out;
}

void project_file_name(const char* name, char* out, size_t cap) {
    size_t n = 0;
    for (const char* p = name; *p && n + 1 < cap; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out[n++] = ok ? c : '_';
    }
    if (n == 0) out[n++] = 'G', out[n++] = 'a', out[n++] = 'm', out[n++] = 'e';
    out[n < cap ? n : cap - 1] = 0;
}

}  // namespace fe
