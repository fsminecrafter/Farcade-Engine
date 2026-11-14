#include "script.h"

#include <string.h>

namespace fe {

namespace {

struct Cursor {
    const char* p;
    const char* end;
    bool done() const { return p >= end; }
    char peek() const { return p < end ? *p : 0; }
    void skip_ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) p++; }
    bool eat(char c) { skip_ws(); if (peek() == c) { p++; return true; } return false; }
    bool eat_word(const char* w) {
        skip_ws();
        size_t n = strlen(w);
        if ((size_t)(end - p) < n || memcmp(p, w, n) != 0) return false;
        // A whole word only.
        char after = p + n < end ? p[n] : 0;
        if ((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') || (after >= '0' && after <= '9') || after == '_') return false;
        p += n;
        return true;
    }
};

bool ident_char(char c, bool first) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (!first && c >= '0' && c <= '9');
}

// An identifier into out (truncated to cap - 1). False when there is none.
bool read_ident(Cursor& c, char* out, int cap) {
    c.skip_ws();
    if (!ident_char(c.peek(), true)) return false;
    int n = 0;
    while (!c.done() && ident_char(c.peek(), n == 0)) {
        if (n < cap - 1) out[n++] = c.peek();
        c.p++;
    }
    out[n] = 0;
    return true;
}

// A decimal number ("5", "-0.25", "1e3"); no libm.
bool read_number(Cursor& c, float* out) {
    c.skip_ws();
    const char* s = c.p;
    bool neg = false;
    if (c.peek() == '-' || c.peek() == '+') { neg = c.peek() == '-'; c.p++; }
    double v = 0;
    int digits = 0;
    while (c.peek() >= '0' && c.peek() <= '9') { v = v * 10 + (c.peek() - '0'); c.p++; digits++; }
    if (c.peek() == '.') {
        c.p++;
        double f = 0.1;
        while (c.peek() >= '0' && c.peek() <= '9') { v += (c.peek() - '0') * f; f *= 0.1; c.p++; digits++; }
    }
    if (!digits) { c.p = s; return false; }
    if (c.peek() == 'e' || c.peek() == 'E') {
        const char* e = c.p++;
        bool eneg = false;
        if (c.peek() == '-' || c.peek() == '+') { eneg = c.peek() == '-'; c.p++; }
        int ex = 0, ed = 0;
        while (c.peek() >= '0' && c.peek() <= '9') { ex = ex * 10 + (c.peek() - '0'); c.p++; ed++; }
        if (!ed) c.p = e;
        else for (int i = 0; i < ex && i < 40; i++) v = eneg ? v / 10 : v * 10;
    }
    *out = (float)(neg ? -v : v);
    return true;
}

// A "quoted" string, or the rest of the line without trailing blanks.
void read_text(Cursor& c, char* out, int cap) {
    c.skip_ws();
    int n = 0;
    if (c.peek() == '"' || c.peek() == '\'') {
        char q = *c.p++;
        while (!c.done() && c.peek() != q) {
            char ch = *c.p++;
            if (ch == '\\' && !c.done()) {
                ch = *c.p++;
                if (ch == 'n') ch = '\n';
                else if (ch == 't') ch = '\t';
            }
            if (n < cap - 1) out[n++] = ch;
        }
        if (!c.done()) c.p++;
    } else {
        const char* s = c.p;
        const char* e = c.end;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
        while (s < e && n < cap - 1) out[n++] = *s++;
        c.p = c.end;
    }
    out[n] = 0;
}

// A list choice: up to ',' or ')' (or the end), trimmed.
void read_choice(Cursor& c, char* out, int cap) {
    c.skip_ws();
    const char* s = c.p;
    while (!c.done() && c.peek() != ',' && c.peek() != ')') c.p++;
    const char* e = c.p;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    if (e - s >= 2 && (*s == '"' || *s == '\'') && e[-1] == *s) { s++; e--; }
    int n = 0;
    while (s < e && n < cap - 1) out[n++] = *s++;
    out[n] = 0;
}

int find_choice(const ScriptProp& p, const char* text) {
    for (int i = 0; i < p.choice_count; i++)
        if (strcmp(p.choices[i], text) == 0) return i;
    return -1;
}

// One "@prop ..." declaration (the cursor just after "@prop").
bool parse_decl(Cursor& c, ScriptProp* out) {
    *out = ScriptProp();
    if (!read_ident(c, out->name, sizeof(out->name))) return false;
    char type[16];
    if (!read_ident(c, type, sizeof(type))) return false;
    if (!strcmp(type, "number") || !strcmp(type, "float") || !strcmp(type, "int")) out->type = PropType::Number;
    else if (!strcmp(type, "text") || !strcmp(type, "string")) out->type = PropType::Text;
    else if (!strcmp(type, "toggle") || !strcmp(type, "bool")) out->type = PropType::Toggle;
    else if (!strcmp(type, "list")) out->type = PropType::List;
    else if (!strcmp(type, "object")) out->type = PropType::Object;
    else return false;

    if (out->type == PropType::List) {
        if (!c.eat('(')) return false;
        while (!c.done() && c.peek() != ')') {
            char tmp[24];
            read_choice(c, tmp, sizeof(tmp));
            if (tmp[0] && out->choice_count < kMaxChoices) memcpy(out->choices[out->choice_count++], tmp, sizeof(tmp));
            if (!c.eat(',')) break;
        }
        if (!c.eat(')') || out->choice_count == 0) return false;
    }

    if (c.eat('=')) {
        switch (out->type) {
            case PropType::Number:
                if (!read_number(c, &out->number)) return false;
                break;
            case PropType::Text: {
                // Stop a bare default at nothing: the rest of the line is the text.
                read_text(c, out->text, sizeof(out->text));
                break;
            }
            case PropType::Toggle:
                if (c.eat_word("true") || c.eat_word("on") || c.eat_word("yes")) out->toggle = true;
                else if (c.eat_word("false") || c.eat_word("off") || c.eat_word("no")) out->toggle = false;
                else return false;
                break;
            case PropType::List: {
                char tmp[24];
                read_choice(c, tmp, sizeof(tmp));
                int i = find_choice(*out, tmp);
                if (i < 0) return false;
                out->choice = i;
                break;
            }
            case PropType::Object:
                return false;   // an object link has no default
        }
    }
    if (out->type == PropType::Number && c.eat_word("range")) {
        float a, b;
        if (!read_number(c, &a) || !read_number(c, &b) || b < a) return false;
        out->min = a;
        out->max = b;
        if (out->number < a) out->number = a;
        if (out->number > b) out->number = b;
    }
    c.skip_ws();
    // Anything after the declaration must be a comment ("-- ...").
    if (!c.done() && !(c.end - c.p >= 2 && c.p[0] == '-' && c.p[1] == '-')) return false;
    return true;
}

}  // namespace

const char* prop_type_name(PropType t) {
    switch (t) {
        case PropType::Number: return "number";
        case PropType::Text: return "text";
        case PropType::Toggle: return "toggle";
        case PropType::List: return "list";
        case PropType::Object: return "object";
    }
    return "?";
}

ScriptProp* ScriptSlot::prop(const char* n) {
    for (int i = 0; i < prop_count; i++)
        if (strcmp(props[i].name, n) == 0) return &props[i];
    return nullptr;
}

const ScriptProp* ScriptSlot::prop(const char* n) const { return const_cast<ScriptSlot*>(this)->prop(n); }

const char* ScriptSlot::name() const {
    const char* s = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\') s = p + 1;
    return s;
}

bool ScriptSlot::name_is(const char* n) const {
    // "ball" matches "ball.lua"; "ball.lua" matches too.
    const char* s = name();
    size_t len = strlen(n);
    if (strncmp(s, n, len) != 0) return false;
    return s[len] == 0 || (s[len] == '.' && !strchr(s + len + 1, '.'));
}

int parse_script_props(const char* text, uint32_t size, ScriptProp* out, int max, int* bad) {
    int count = 0, nbad = 0;
    const char* p = text;
    const char* end = text + size;
    while (p < end) {
        const char* eol = p;
        while (eol < end && *eol != '\n') eol++;
        Cursor c{ p, eol };
        c.skip_ws();
        if (c.end - c.p >= 2 && c.p[0] == '-' && c.p[1] == '-') {
            c.p += 2;
            while (c.peek() == '-') c.p++;          // "---" too
            c.skip_ws();
            if (c.peek() == '@') {
                c.p++;
                if (c.eat_word("prop")) {
                    ScriptProp tmp;
                    bool dup = false;
                    if (parse_decl(c, &tmp)) {
                        for (int i = 0; i < count; i++) dup |= strcmp(out[i].name, tmp.name) == 0;
                        if (dup) nbad++;
                        else if (count < max) out[count++] = tmp;
                    } else {
                        nbad++;
                    }
                }
            }
        }
        p = eol + 1;
    }
    if (bad) *bad = nbad;
    return count;
}

void merge_script_props(ScriptSlot* slot, const ScriptProp* declared, int count) {
    ScriptProp merged[kMaxProps];
    if (count > kMaxProps) count = kMaxProps;
    for (int i = 0; i < count; i++) {
        merged[i] = declared[i];
        const ScriptProp* old = slot->prop(declared[i].name);
        if (!old || old->type != declared[i].type) continue;
        ScriptProp& m = merged[i];
        switch (m.type) {
            case PropType::Number:
                m.number = old->number;
                if (m.min < m.max) {
                    if (m.number < m.min) m.number = m.min;
                    if (m.number > m.max) m.number = m.max;
                }
                break;
            case PropType::Text: memcpy(m.text, old->text, sizeof(m.text)); break;
            case PropType::Toggle: m.toggle = old->toggle; break;
            case PropType::List: {
                int k = old->choice >= 0 && old->choice < old->choice_count ? find_choice(m, old->choices[old->choice]) : -1;
                if (k >= 0) m.choice = k;
                break;
            }
            case PropType::Object: m.object = old->object; break;
        }
    }
    for (int i = 0; i < count; i++) slot->props[i] = merged[i];
    for (int i = count; i < kMaxProps; i++) slot->props[i] = ScriptProp();
    slot->prop_count = count;
}

}  // namespace fe
