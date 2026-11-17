// The Lua script runtime. See runtime.h for the contract and docs/scripting.md
// for the API scripts see.
//
// Lua reports errors with longjmp, so the C functions below that can raise
// (luaL_error ...) hold no objects with destructors, and every callback into
// Lua runs under lua_pcall.
#include "script/runtime.h"
#include "script/internal.h"
#include "script/lua_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fe {

namespace {

const char* const kObjMeta = "fe.object";
const char* const kModMeta = "fe.module";
const char* const kMatMeta = "fe.material";
const char* const kVecMeta = "fe.vec3";

// ---------------------------------------------------------------- instances

struct Inst {
    int id = 0;
    Entity e = 0;
    char* path = nullptr;
    int env_ref = LUA_NOREF;
    int self_ref = LUA_NOREF;
    bool started = false;
    bool failed = false;
    bool dying = false;         // its object is about to be destroyed
    bool removed = false;       // unlinked; freed when no callback is running
    Inst* hnext = nullptr;      // chain of the entity hash
};

double now_seconds() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// "door" matches ".../door.lua" and "door.lua" (the rule of ScriptSlot::name_is).
const char* base_name(const char* path) {
    const char* s = path;
    for (const char* p = path; *p; p++)
        if (*p == '/' || *p == '\\') s = p + 1;
    return s;
}

bool name_matches(const char* path, const char* n) {
    const char* s = base_name(path);
    size_t len = strlen(n);
    if (strncmp(s, n, len) != 0) return false;
    return s[len] == 0 || (s[len] == '.' && !strchr(s + len + 1, '.'));
}

}  // namespace

struct ScriptRuntime::Impl {
    World* world = nullptr;
    ScriptHost host;
    lua_State* L = nullptr;
    Entity sys_start = 0, sys_update = 0;
    int lib_ref = LUA_NOREF;
    int fe_ref = LUA_NOREF;
    int cache_ref = LUA_NOREF;          // entity id -> handle (weak values)
    int console_env_ref = LUA_NOREF;
    double t0 = 0;

    Inst** list = nullptr;              // in add order (ids ascending)
    int count = 0, cap = 0;
    int next_id = 1;
    int live = 0;                       // not removed
    Inst** buckets = nullptr;
    int nbuckets = 0;
    bool dirty = false;                 // removed instances wait in `list`

    Entity* pending = nullptr;          // destroy requests
    int npending = 0, cap_pending = 0;

    int depth = 0;                      // running callbacks / entry points
    ScriptStats stats;

    // Methods on module proxies (add_module_method).
    struct Method { int kind; char name[32]; ScriptRuntime::ModuleMethod fn; void* user; };
    Method methods[32];
    int method_count = 0;

    // Input from the host (key_event / mouse_*): fe.key_down("w"), fe.mouse_x() ...
    struct KeyState { char name[16]; bool down, pressed, released; };
    KeyState keys[96];
    int key_count = 0;
    float mouse_x = 0, mouse_y = 0, mouse_wheel = 0;
    uint8_t mouse_down = 0, mouse_pressed = 0, mouse_released = 0;
    KeyState* key_slot(const char* name, bool create) {
        for (int i = 0; i < key_count; i++)
            if (strcmp(keys[i].name, name) == 0) return &keys[i];
        if (!create || key_count == 96 || strlen(name) >= sizeof keys[0].name) return nullptr;
        KeyState* k = &keys[key_count++];
        memset(k, 0, sizeof *k);
        strcpy(k->name, name);
        return k;
    }
    void end_input_frame() {
        for (int i = 0; i < key_count; i++) keys[i].pressed = keys[i].released = false;
        mouse_pressed = mouse_released = 0;
        mouse_wheel = 0;
    }

    // ---- entity hash
    int bucket_of(Entity e) const {
        return (int)(((e * 0x9E3779B97F4A7C15ull) >> 40) & (uint64_t)(nbuckets - 1));
    }
    void hash_insert(Inst* in) {
        Inst** pp = &buckets[bucket_of(in->e)];
        while (*pp) pp = &(*pp)->hnext;
        in->hnext = nullptr;
        *pp = in;
    }
    void hash_unlink(Inst* in) {
        Inst** pp = &buckets[bucket_of(in->e)];
        while (*pp && *pp != in) pp = &(*pp)->hnext;
        if (*pp) *pp = in->hnext;
        in->hnext = nullptr;
    }
    void hash_grow() {
        int nb = nbuckets ? nbuckets * 4 : 256;
        Inst** nbk = (Inst**)calloc((size_t)nb, sizeof(Inst*));
        free(buckets);
        buckets = nbk;
        nbuckets = nb;
        for (int i = 0; i < count; i++)
            if (!list[i]->removed) hash_insert(list[i]);
    }
    // The live instances of e, in add order. Returns the count (at most max).
    int collect(Entity e, Inst** out, int max) const {
        int n = 0;
        if (!nbuckets) return 0;
        for (Inst* in = buckets[bucket_of(e)]; in && n < max; in = in->hnext)
            if (in->e == e && !in->removed) out[n++] = in;
        return n;
    }
    Inst* first_of(Entity e) const {
        if (!nbuckets) return nullptr;
        for (Inst* in = buckets[bucket_of(e)]; in; in = in->hnext)
            if (in->e == e && !in->removed) return in;
        return nullptr;
    }
    Inst* by_id(int id) const {
        int lo = 0, hi = count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (list[mid]->id == id) return list[mid]->removed ? nullptr : list[mid];
            if (list[mid]->id < id) lo = mid + 1; else hi = mid - 1;
        }
        return nullptr;
    }

    // ---- removal, destroy requests
    void remove_inst(Inst* in) {
        if (in->removed) return;
        in->removed = true;
        hash_unlink(in);
        live--;
        if (in->failed) stats.failed--;
        dirty = true;
    }
    void remove_entity(Entity e) {
        Inst* tmp[64];
        for (;;) {
            int n = collect(e, tmp, 64);
            for (int i = 0; i < n; i++) remove_inst(tmp[i]);
            if (n < 64) break;
        }
    }
    void remove_tree(Entity e) {
        remove_entity(e);
        int n = world->child_count(e);
        if (n > 0) {
            Entity* kids = (Entity*)malloc(sizeof(Entity) * (size_t)n);
            n = world->children(e, kids, n);
            for (int i = 0; i < n; i++) remove_tree(kids[i]);
            free(kids);
        }
    }
    bool is_pending(Entity e) const {
        for (int i = 0; i < npending; i++)
            if (pending[i] == e) return true;
        return false;
    }
    void mark_dying(Entity e) {
        Inst* tmp[64];
        int n = collect(e, tmp, 64);
        for (int i = 0; i < n; i++) tmp[i]->dying = true;
        int kn = world->child_count(e);
        if (kn > 0) {
            Entity* kids = (Entity*)malloc(sizeof(Entity) * (size_t)kn);
            kn = world->children(e, kids, kn);
            for (int i = 0; i < kn; i++) mark_dying(kids[i]);
            free(kids);
        }
    }
    void request_destroy(Entity e) {
        if (is_pending(e)) return;
        if (npending == cap_pending) {
            cap_pending = cap_pending ? cap_pending * 2 : 8;
            pending = (Entity*)realloc(pending, sizeof(Entity) * (size_t)cap_pending);
        }
        pending[npending++] = e;
        mark_dying(e);
    }
    bool dead(Entity e) const { return !world->alive(e) || is_pending(e); }

    void compact() {
        int w = 0;
        for (int i = 0; i < count; i++) {
            Inst* in = list[i];
            if (!in->removed) { list[w++] = in; continue; }
            luaL_unref(L, LUA_REGISTRYINDEX, in->env_ref);
            luaL_unref(L, LUA_REGISTRYINDEX, in->self_ref);
            free(in->path);
            delete in;
        }
        count = w;
        dirty = false;
    }
    // At depth 0: carry out destroy requests and free removed instances.
    void flush() {
        if (npending) {
            for (int i = 0; i < npending; i++) {
                Entity e = pending[i];
                if (!world->alive(e)) continue;
                remove_tree(e);
                world->destroy(e);
            }
            npending = 0;
        }
        if (dirty) compact();
    }

    struct Scope {
        Impl* I;
        explicit Scope(Impl* i) : I(i) { I->depth++; }
        ~Scope() { if (--I->depth == 0) I->flush(); }
    };

    void log(ScriptLogLevel level, const char* text) {
        if (host.log) host.log(level, text, host.user);
    }

    // ---- calling into Lua
    // Calls the function `fname` of the instance's environment as
    // fn(self, <args pushed by push>). False when the instance has none (or
    // cannot run); an error stops the instance.
    // `L` is the state the call comes from (a coroutine's, when a script sends
    // an event from one), so the arguments on its stack can be copied.
    template <class Push>
    bool invoke(lua_State* L, Inst* in, const char* fname, Push push) {
        if (in->removed || in->failed || in->dying) return false;
        if (!lua_checkstack(L, 8)) return false;
        const int base = lua_gettop(L);
        lua_pushcfunction(L, msgh);
        lua_rawgeti(L, LUA_REGISTRYINDEX, in->env_ref);
        lua_pushstring(L, fname);
        lua_rawget(L, -2);
        lua_remove(L, -2);
        if (!lua_isfunction(L, -1)) { lua_settop(L, base); return false; }
        lua_rawgeti(L, LUA_REGISTRYINDEX, in->self_ref);
        int n = push(L);
        depth++;
        int status = lua_pcall(L, 1 + n, 0, base + 1);
        depth--;
        if (status != LUA_OK) {
            const char* m = lua_tostring(L, -1);
            log(ScriptLogLevel::Error, m ? m : "script error");
            if (!in->failed) {
                in->failed = true;
                stats.failed++;
            }
        }
        lua_settop(L, base);
        return true;
    }
    bool invoke0(Inst* in, const char* fname) {
        return invoke(L, in, fname, [](lua_State*) { return 0; });
    }

    // on_<event>(self, args...) and on_event(self, event, args...) of one
    // instance; the arguments are the stack values argbase .. argbase+nargs-1.
    void deliver_one(lua_State* L, Inst* in, const char* event, int argbase, int nargs) {
        if (!lua_checkstack(L, nargs + 8)) return;
        if (strcmp(event, "event") != 0) {      // (an event called "event" only reaches on_event)
            char fname[96];
            snprintf(fname, sizeof fname, "on_%s", event);
            invoke(L, in, fname, [&](lua_State* l) {
                for (int i = 0; i < nargs; i++) lua_pushvalue(l, argbase + i);
                return nargs;
            });
        }
        invoke(L, in, "on_event", [&](lua_State* l) {
            lua_pushstring(l, event);
            for (int i = 0; i < nargs; i++) lua_pushvalue(l, argbase + i);
            return nargs + 1;
        });
    }
    void deliver(lua_State* L, Entity e, const char* event, int argbase, int nargs) {
        Scope s(this);
        Inst* tmp[64];
        int n = collect(e, tmp, 64);
        for (int i = 0; i < n; i++) deliver_one(L, tmp[i], event, argbase, nargs);
    }
    void broadcast(lua_State* L, const char* event, int argbase, int nargs) {
        Scope s(this);
        int n = count;
        for (int i = 0; i < n && i < count; i++) deliver_one(L, list[i], event, argbase, nargs);
    }

    // ---- the frame
    void set_dt(float dt) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, fe_ref);
        lua_pushnumber(L, dt);
        lua_setfield(L, -2, "dt");
        lua_pop(L, 1);
    }
    void run_starts(float dt) {
        Scope s(this);
        set_dt(dt);
        int n = count;
        for (int i = 0; i < n && i < count; i++) {
            Inst* in = list[i];
            if (in->removed || in->started) continue;
            if (!world->alive(in->e)) { remove_inst(in); continue; }
            if (in->failed || in->dying) continue;
            in->started = true;
            invoke0(in, "on_start");
        }
    }
    void run_updates(float dt) {
        double t = now_seconds();
        {
            Scope s(this);
            set_dt(dt);
            int n = count;
            for (int i = 0; i < n && i < count; i++) {
                Inst* in = list[i];
                if (in->removed) continue;
                if (!world->alive(in->e)) { remove_inst(in); continue; }
                if (in->failed || in->dying || !in->started) continue;
                invoke(L, in, "on_update", [dt](lua_State* l) { lua_pushnumber(l, dt); return 1; });
            }
        }
        stats.update_ms = (now_seconds() - t) * 1000.0;
        stats.instances = live;
        end_input_frame();
        lua_gc(L, LUA_GCSTEP, 0);
    }

    // ---- Lua functions
    static int msgh(lua_State* l);
};

namespace {

typedef ScriptRuntime::Impl Impl;

Impl* impl_of(lua_State* L) { return *(Impl**)lua_getextraspace(L); }

}  // namespace

int ScriptRuntime::Impl::msgh(lua_State* l) {
    const char* m = lua_tostring(l, 1);
    if (!m) {
        if (luaL_callmeta(l, 1, "__tostring") && lua_type(l, -1) == LUA_TSTRING) return 1;
        m = lua_pushfstring(l, "(error object is a %s value)", luaL_typename(l, 1));
    }
    luaL_traceback(l, l, m, 1);
    return 1;
}

namespace {

// ---------------------------------------------------------------- vec3

void push_vec3(lua_State* L, const Vec3& v) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z); lua_setfield(L, -2, "z");
    luaL_setmetatable(L, kVecMeta);
}

bool raw_number(lua_State* L, int t, const char* key, float* out) {
    lua_pushstring(L, key);
    lua_rawget(L, t);
    bool ok = lua_type(L, -1) == LUA_TNUMBER;
    if (ok) *out = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return ok;
}
bool raw_index(lua_State* L, int t, int i, float* out) {
    lua_rawgeti(L, t, i);
    bool ok = lua_type(L, -1) == LUA_TNUMBER;
    if (ok) *out = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return ok;
}

// A vec3, {x=, y=, z=} or {1, 2, 3}.
bool read_vec3(lua_State* L, int idx, Vec3* out) {
    if (lua_type(L, idx) != LUA_TTABLE) return false;
    idx = lua_absindex(L, idx);
    Vec3 v;
    if (raw_number(L, idx, "x", &v.x) && raw_number(L, idx, "y", &v.y) && raw_number(L, idx, "z", &v.z)) { *out = v; return true; }
    if (raw_index(L, idx, 1, &v.x) && raw_index(L, idx, 2, &v.y) && raw_index(L, idx, 3, &v.z)) { *out = v; return true; }
    return false;
}
Vec3 check_vec3(lua_State* L, int idx) {
    Vec3 v;
    if (!read_vec3(L, idx, &v))
        luaL_error(L, "bad argument #%d: expected a vec3 ({x=,y=,z=} or {x, y, z}), got %s", idx, luaL_typename(L, idx));
    return v;
}

int f_vec3(lua_State* L) {
    Vec3 v;
    if (lua_gettop(L) == 1 && lua_type(L, 1) == LUA_TTABLE) v = check_vec3(L, 1);
    else v = Vec3((float)luaL_optnumber(L, 1, 0), (float)luaL_optnumber(L, 2, 0), (float)luaL_optnumber(L, 3, 0));
    push_vec3(L, v);
    return 1;
}
int v_add(lua_State* L) { push_vec3(L, check_vec3(L, 1) + check_vec3(L, 2)); return 1; }
int v_sub(lua_State* L) { push_vec3(L, check_vec3(L, 1) - check_vec3(L, 2)); return 1; }
int v_mul(lua_State* L) {
    if (lua_type(L, 1) == LUA_TNUMBER) push_vec3(L, check_vec3(L, 2) * (float)lua_tonumber(L, 1));
    else if (lua_type(L, 2) == LUA_TNUMBER) push_vec3(L, check_vec3(L, 1) * (float)lua_tonumber(L, 2));
    else push_vec3(L, mul(check_vec3(L, 1), check_vec3(L, 2)));
    return 1;
}
int v_div(lua_State* L) {
    push_vec3(L, check_vec3(L, 1) * (1.0f / (float)luaL_checknumber(L, 2)));
    return 1;
}
int v_unm(lua_State* L) { push_vec3(L, -check_vec3(L, 1)); return 1; }
int v_eq(lua_State* L) {
    Vec3 a, b;
    lua_pushboolean(L, read_vec3(L, 1, &a) && read_vec3(L, 2, &b) && a.x == b.x && a.y == b.y && a.z == b.z);
    return 1;
}
int v_tostring(lua_State* L) {
    Vec3 v = check_vec3(L, 1);
    char buf[96];
    snprintf(buf, sizeof buf, "vec3(%g, %g, %g)", (double)v.x, (double)v.y, (double)v.z);
    lua_pushstring(L, buf);
    return 1;
}
int v_length(lua_State* L) { lua_pushnumber(L, length(check_vec3(L, 1))); return 1; }
int v_normalized(lua_State* L) { push_vec3(L, normalize(check_vec3(L, 1))); return 1; }
int f_dot(lua_State* L) { lua_pushnumber(L, dot(check_vec3(L, 1), check_vec3(L, 2))); return 1; }
int f_cross(lua_State* L) { push_vec3(L, cross(check_vec3(L, 1), check_vec3(L, 2))); return 1; }

// self.<name> that is not set falls back to what the script's own chunk
// defined (its functions: other scripts call door:open()), but not to the
// library behind it (self.type, self.next, ... stay nil).
int self_index(lua_State* L) {
    if (!lua_getmetatable(L, 1)) return 0;
    lua_pushstring(L, "env");
    lua_rawget(L, -2);
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}

// ---------------------------------------------------------------- object handles

// One userdata per entity (cached, weakly), so == and table keys work.
void push_object(lua_State* L, Entity e) {
    Impl* I = impl_of(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, I->cache_ref);
    lua_rawgeti(L, -1, (lua_Integer)e);
    if (lua_type(L, -1) == LUA_TUSERDATA) { lua_remove(L, -2); return; }
    lua_pop(L, 1);
    Entity* ud = (Entity*)lua_newuserdatauv(L, sizeof(Entity), 0);
    *ud = e;
    luaL_setmetatable(L, kObjMeta);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, (lua_Integer)e);
    lua_remove(L, -2);
}

Entity obj_entity(lua_State* L, int idx) {
    Entity* ud = (Entity*)luaL_checkudata(L, idx, kObjMeta);
    return *ud;
}
// The entity of a handle that must still exist.
Entity live_entity(lua_State* L, int idx) {
    Entity e = obj_entity(L, idx);
    if (impl_of(L)->dead(e)) luaL_error(L, "the object no longer exists (it was destroyed)");
    return e;
}

int obj_children(lua_State* L) {
    Impl* I = impl_of(L);
    Entity e = live_entity(L, 1);
    int n = I->world->child_count(e);
    lua_createtable(L, n, 0);
    if (n <= 0) return 1;
    Entity* kids = (Entity*)malloc(sizeof(Entity) * (size_t)n);
    n = I->world->children(e, kids, n);
    for (int i = 0; i < n; i++) {
        push_object(L, kids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    free(kids);
    return 1;
}

int obj_script(lua_State* L) {
    Impl* I = impl_of(L);
    Entity e = live_entity(L, 1);
    const char* name = luaL_checkstring(L, 2);
    Inst* tmp[64];
    int n = I->collect(e, tmp, 64);
    for (int i = 0; i < n; i++) {
        if (name_matches(tmp[i]->path, name)) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, tmp[i]->self_ref);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

int obj_scripts(lua_State* L) {
    Impl* I = impl_of(L);
    Entity e = live_entity(L, 1);
    Inst* tmp[64];
    int n = I->collect(e, tmp, 64);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, tmp[i]->self_ref);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int obj_send(lua_State* L) {
    Impl* I = impl_of(L);
    Entity e = live_entity(L, 1);
    const char* event = luaL_checkstring(L, 2);
    if (strlen(event) > 80) return luaL_error(L, "event name too long");
    I->deliver(L, e, event, 3, lua_gettop(L) - 2);
    return 0;
}

int obj_move(lua_State* L) {
    Impl* I = impl_of(L);
    Entity e = live_entity(L, 1);
    Vec3 d;
    if (lua_type(L, 2) == LUA_TTABLE) d = check_vec3(L, 2);
    else d = Vec3((float)luaL_optnumber(L, 2, 0), (float)luaL_optnumber(L, 3, 0), (float)luaL_optnumber(L, 4, 0));
    Transform* t = I->world->transform(e);
    if (t) t->position += d;
    return 0;
}

int obj_destroy(lua_State* L) {
    Impl* I = impl_of(L);
    Entity e = obj_entity(L, 1);
    if (!I->dead(e)) I->request_destroy(e);
    return 0;
}

void push_module(lua_State* L, Entity e, ModuleKind k) {
    struct Mod { Entity e; int kind; };
    Mod* m = (Mod*)lua_newuserdatauv(L, sizeof(Mod), 0);
    m->e = e;
    m->kind = (int)k;
    luaL_setmetatable(L, kModMeta);
}

int obj_index(lua_State* L) {
    Impl* I = impl_of(L);
    World* w = I->world;
    Entity e = obj_entity(L, 1);
    if (lua_type(L, 2) != LUA_TSTRING) { lua_pushnil(L); return 1; }
    const char* k = lua_tostring(L, 2);
    if (strcmp(k, "alive") == 0) { lua_pushboolean(L, !I->dead(e)); return 1; }
    // methods
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 2);
    if (I->dead(e)) return luaL_error(L, "cannot read '%s': the object no longer exists (it was destroyed)", k);
    if (strcmp(k, "name") == 0) {
        const char* n = w->name(e);
        lua_pushstring(L, n ? n : "");
    } else if (strcmp(k, "position") == 0) {
        push_vec3(L, w->transform(e)->position);
    } else if (strcmp(k, "rotation") == 0) {
        push_vec3(L, w->transform(e)->rotation.to_euler_deg());
    } else if (strcmp(k, "scale") == 0) {
        push_vec3(L, w->transform(e)->scale);
    } else if (strcmp(k, "world_position") == 0) {
        const WorldTransform* wt = w->world_transform(e);
        push_vec3(L, wt ? Vec3(wt->matrix.m[12], wt->matrix.m[13], wt->matrix.m[14]) : Vec3());
    } else if (strcmp(k, "parent") == 0) {
        Entity p = w->parent(e);
        if (p) push_object(L, p); else lua_pushnil(L);
    } else if (strcmp(k, "material") == 0) {
        // Only objects that draw a mesh have one.
        if (w->renderable(e)) {
            Entity* m = (Entity*)lua_newuserdatauv(L, sizeof(Entity), 0);
            *m = e;
            luaL_setmetatable(L, kMatMeta);
        } else {
            lua_pushnil(L);
        }
    } else {
        ModuleKind mk = module_by_name(k);
        if (mk != ModuleKind::Count) {
            if (w->has(e, mk)) push_module(L, e, mk); else lua_pushnil(L);
        } else {
            lua_pushnil(L);
        }
    }
    return 1;
}

int obj_newindex(lua_State* L) {
    Impl* I = impl_of(L);
    World* w = I->world;
    Entity e = live_entity(L, 1);
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "name") == 0) {
        const char* n = luaL_checkstring(L, 3);
        if (!w->set_name(e, n)) return luaL_error(L, "cannot rename the object to '%s': a sibling already has that name", n);
    } else if (strcmp(k, "position") == 0) {
        w->transform(e)->position = check_vec3(L, 3);
    } else if (strcmp(k, "rotation") == 0) {
        w->transform(e)->rotation = Quat::euler_deg(check_vec3(L, 3));
    } else if (strcmp(k, "scale") == 0) {
        if (lua_type(L, 3) == LUA_TNUMBER) {
            float s = (float)lua_tonumber(L, 3);
            w->transform(e)->scale = Vec3(s, s, s);
        } else {
            w->transform(e)->scale = check_vec3(L, 3);
        }
    } else if (strcmp(k, "world_position") == 0 || strcmp(k, "parent") == 0 || strcmp(k, "alive") == 0 || strcmp(k, "material") == 0) {
        return luaL_error(L, "'%s' is read only", k);
    } else {
        return luaL_error(L, "cannot set '%s' on an object (settable: name, position, rotation, scale; modules are read with obj.<module>)", k);
    }
    return 0;
}

int obj_eq(lua_State* L) {
    lua_pushboolean(L, obj_entity(L, 1) == obj_entity(L, 2));
    return 1;
}

int obj_tostring(lua_State* L) {
    Impl* I = impl_of(L);
    Entity e = obj_entity(L, 1);
    if (I->dead(e)) { lua_pushstring(L, "object(destroyed)"); return 1; }
    const char* n = I->world->name(e);
    if (n && *n) lua_pushfstring(L, "object(%s)", n);
    else lua_pushfstring(L, "object(#%I)", (lua_Integer)(e & 0xffffffffu));
    return 1;
}

// ---------------------------------------------------------------- material proxy
//
// obj.material.color / roughness / metallic / unlit / cast_shadows / visible:
// reads and writes the entity's Renderable.

Renderable* check_material(lua_State* L, const char* key) {
    Impl* I = impl_of(L);
    Entity e = *(Entity*)luaL_checkudata(L, 1, kMatMeta);
    if (I->dead(e)) { luaL_error(L, "cannot use material.%s: the object no longer exists", key); return nullptr; }
    Renderable* r = I->world->renderable(e);
    if (!r) luaL_error(L, "the object has no mesh, so no material");
    return r;
}

int mat_index(lua_State* L) {
    const char* k = luaL_checkstring(L, 2);
    Renderable* r = check_material(L, k);
    if (strcmp(k, "color") == 0) push_vec3(L, r->color);
    else if (strcmp(k, "roughness") == 0) lua_pushnumber(L, r->roughness);
    else if (strcmp(k, "metallic") == 0) lua_pushnumber(L, r->metallic);
    else if (strcmp(k, "unlit") == 0) lua_pushboolean(L, r->unlit);
    else if (strcmp(k, "cast_shadows") == 0) lua_pushboolean(L, r->cast_shadows);
    else if (strcmp(k, "visible") == 0) lua_pushboolean(L, r->visible);
    else if (strcmp(k, "texture") == 0) lua_pushstring(L, r->texture_path);
    else if (strcmp(k, "shader") == 0) lua_pushstring(L, r->shader_path);
    else return luaL_error(L, "material has no field '%s' (color, roughness, metallic, unlit, cast_shadows, visible, texture, shader)", k);
    return 1;
}

int mat_newindex(lua_State* L) {
    const char* k = luaL_checkstring(L, 2);
    Renderable* r = check_material(L, k);
    auto number01 = [&](float* out) {
        if (lua_type(L, 3) != LUA_TNUMBER) return luaL_error(L, "material.%s expects a number from 0 to 1, got %s", k, luaL_typename(L, 3));
        float v = (float)lua_tonumber(L, 3);
        *out = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        return 0;
    };
    auto boolean = [&](bool* out) {
        if (lua_type(L, 3) != LUA_TBOOLEAN) return luaL_error(L, "material.%s expects true or false, got %s", k, luaL_typename(L, 3));
        *out = lua_toboolean(L, 3) != 0;
        return 0;
    };
    auto file = [&](char* out, size_t cap) {
        if (lua_type(L, 3) != LUA_TSTRING)
            return luaL_error(L, "material.%s expects a string path, got %s", k, luaL_typename(L, 3));
        size_t n = 0;
        const char* value = luaL_checklstring(L, 3, &n);
        if (n >= cap) return luaL_error(L, "material.%s path is too long (maximum %d bytes)", k, (int)cap - 1);
        memcpy(out, value, n);
        out[n] = 0;
        return 0;
    };
    if (strcmp(k, "color") == 0) r->color = check_vec3(L, 3);
    else if (strcmp(k, "roughness") == 0) number01(&r->roughness);
    else if (strcmp(k, "metallic") == 0) number01(&r->metallic);
    else if (strcmp(k, "unlit") == 0) boolean(&r->unlit);
    else if (strcmp(k, "cast_shadows") == 0) boolean(&r->cast_shadows);
    else if (strcmp(k, "visible") == 0) boolean(&r->visible);
    else if (strcmp(k, "texture") == 0) file(r->texture_path, sizeof(r->texture_path));
    else if (strcmp(k, "shader") == 0) file(r->shader_path, sizeof(r->shader_path));
    else return luaL_error(L, "material has no field '%s' (color, roughness, metallic, unlit, cast_shadows, visible, texture, shader)", k);
    return 0;
}

int mat_tostring(lua_State* L) {
    Impl* I = impl_of(L);
    Entity e = *(Entity*)luaL_checkudata(L, 1, kMatMeta);
    const Renderable* r = I->dead(e) ? nullptr : I->world->renderable(e);
    if (!r) { lua_pushstring(L, "material(none)"); return 1; }
    char buf[96];
    snprintf(buf, sizeof buf, "material(roughness %.2f, metallic %.2f)", (double)r->roughness, (double)r->metallic);
    lua_pushstring(L, buf);
    return 1;
}

// ---------------------------------------------------------------- module proxies

struct ModRef { Entity e; int kind; };

ModRef* check_mod(lua_State* L, int idx) { return (ModRef*)luaL_checkudata(L, idx, kModMeta); }

const FieldInfo* mod_field(lua_State* L, ModRef* m, const char* key) {
    const ModuleInfo& mi = module_info((ModuleKind)m->kind);
    const FieldInfo* f = find_field(mi, key);
    if (!f) luaL_error(L, "module '%s' has no field '%s'", mi.name, key);
    return f;
}

// obj.audio_sampler:play(): calls the registered function with the entity.
int mod_method_call(lua_State* L) {
    Impl* I = impl_of(L);
    ModRef* m = check_mod(L, 1);
    int idx = (int)lua_tointeger(L, lua_upvalueindex(1));
    if (idx < 0 || idx >= I->method_count) return 0;
    const Impl::Method& md = I->methods[idx];
    if (md.kind != m->kind) return luaL_error(L, "%s is not a method of %s", md.name, module_info((ModuleKind)m->kind).name);
    if (I->dead(m->e)) return luaL_error(L, "cannot call %s: the object no longer exists", md.name);
    lua_pushboolean(L, md.fn(m->e, md.user) != 0);
    return 1;
}

int mod_index(lua_State* L) {
    Impl* I = impl_of(L);
    ModRef* m = check_mod(L, 1);
    const char* key = luaL_checkstring(L, 2);
    const ModuleInfo& mi = module_info((ModuleKind)m->kind);
    for (int i = 0; i < I->method_count; i++)
        if (I->methods[i].kind == m->kind && strcmp(I->methods[i].name, key) == 0) {
            lua_pushinteger(L, i);
            lua_pushcclosure(L, mod_method_call, 1);
            return 1;
        }
    const FieldInfo* f = mod_field(L, m, key);
    if (I->dead(m->e)) return luaL_error(L, "cannot read %s.%s: the object no longer exists", mi.name, key);
    FieldValue v;
    if (!I->world->get_field(m->e, (ModuleKind)m->kind, key, &v))
        return luaL_error(L, "cannot read %s.%s: the object no longer has a %s module", mi.name, key, mi.name);
    switch (f->type) {
    case FieldType::Float: lua_pushnumber(L, v.f); break;
    case FieldType::Int: lua_pushinteger(L, v.i); break;
    case FieldType::Bool: lua_pushboolean(L, v.b); break;
    case FieldType::Enum: if (v.s) lua_pushstring(L, v.s); else lua_pushinteger(L, v.i); break;
    case FieldType::Color:
    case FieldType::Vec3: push_vec3(L, v.v); break;
    case FieldType::Path: lua_pushstring(L, v.s ? v.s : ""); break;
    case FieldType::Bits: lua_pushinteger(L, (lua_Integer)(uint32_t)v.i); break;
    }
    return 1;
}

int mod_newindex(lua_State* L) {
    Impl* I = impl_of(L);
    ModRef* m = check_mod(L, 1);
    const char* key = luaL_checkstring(L, 2);
    const ModuleInfo& mi = module_info((ModuleKind)m->kind);
    const FieldInfo* f = mod_field(L, m, key);
    if (I->dead(m->e)) return luaL_error(L, "cannot set %s.%s: the object no longer exists", mi.name, key);
    const int t = lua_type(L, 3);
    FieldValue v;
    switch (f->type) {
    case FieldType::Float:
        if (t != LUA_TNUMBER) return luaL_error(L, "%s.%s expects a number, got %s", mi.name, key, luaL_typename(L, 3));
        v = FieldValue::from_float((float)lua_tonumber(L, 3));
        break;
    case FieldType::Int: {
        if (t != LUA_TNUMBER) return luaL_error(L, "%s.%s expects a number, got %s", mi.name, key, luaL_typename(L, 3));
        double d = lua_tonumber(L, 3);
        if (d > 2147483647.0) d = 2147483647.0;
        if (d < -2147483648.0) d = -2147483648.0;
        v = FieldValue::from_int((int32_t)d);
        break;
    }
    case FieldType::Bool:
        if (t != LUA_TBOOLEAN) return luaL_error(L, "%s.%s expects true or false, got %s", mi.name, key, luaL_typename(L, 3));
        v = FieldValue::from_bool(lua_toboolean(L, 3) != 0);
        break;
    case FieldType::Enum:
        if (t == LUA_TSTRING) v = FieldValue::from_string(lua_tostring(L, 3));
        else if (t == LUA_TNUMBER) v = FieldValue::from_enum((int32_t)lua_tonumber(L, 3));
        else return luaL_error(L, "%s.%s expects one of its names as a string, got %s", mi.name, key, luaL_typename(L, 3));
        break;
    case FieldType::Color:
    case FieldType::Vec3: {
        Vec3 x;
        if (!read_vec3(L, 3, &x)) return luaL_error(L, "%s.%s expects a vec3 (or {x=,y=,z=} or {r, g, b}), got %s", mi.name, key, luaL_typename(L, 3));
        v = f->type == FieldType::Color ? FieldValue::from_color(x) : FieldValue::from_vec3(x);
        break;
    }
    case FieldType::Path:
        if (t != LUA_TSTRING) return luaL_error(L, "%s.%s expects a string, got %s", mi.name, key, luaL_typename(L, 3));
        v = FieldValue::from_string(lua_tostring(L, 3));
        break;
    case FieldType::Bits:
        if (t != LUA_TNUMBER) return luaL_error(L, "%s.%s expects a number, got %s", mi.name, key, luaL_typename(L, 3));
        v = FieldValue::from_bits((uint32_t)(int64_t)lua_tonumber(L, 3));
        break;
    }
    if (!I->world->set_field(m->e, (ModuleKind)m->kind, key, v)) {
        if (f->type == FieldType::Enum && f->enum_names) {
            char names[256];
            names[0] = 0;
            for (int i = 0; f->enum_names[i]; i++) {
                if (i) strncat(names, ", ", sizeof names - strlen(names) - 1);
                strncat(names, f->enum_names[i], sizeof names - strlen(names) - 1);
            }
            return luaL_error(L, "%s.%s: not a valid value (one of: %s)", mi.name, key, names);
        }
        return luaL_error(L, "cannot set %s.%s (the object has no %s module?)", mi.name, key, mi.name);
    }
    return 0;
}

int mod_tostring(lua_State* L) {
    ModRef* m = check_mod(L, 1);
    lua_pushfstring(L, "module(%s)", module_info((ModuleKind)m->kind).name);
    return 1;
}

// ---------------------------------------------------------------- fe library

int f_find(lua_State* L) {
    Impl* I = impl_of(L);
    const char* name = luaL_checkstring(L, 1);
    Entity e = script_find_by_name(*I->world, name);
    if (e) push_object(L, e); else lua_pushnil(L);
    return 1;
}

int f_current(lua_State* L) {
    Impl* I = impl_of(L);
    const char* name = luaL_checkstring(L, 1);
    ModuleKind k = module_by_name(name);
    if (k == ModuleKind::Count) return luaL_error(L, "unknown module '%s'", name);
    Entity e = I->world->current(k);
    if (e) push_object(L, e); else lua_pushnil(L);
    return 1;
}

int f_broadcast(lua_State* L) {
    Impl* I = impl_of(L);
    const char* event = luaL_checkstring(L, 1);
    if (strlen(event) > 80) return luaL_error(L, "event name too long");
    I->broadcast(L, event, 2, lua_gettop(L) - 1);
    return 0;
}

// Input: fe.key_down(name), fe.key_pressed(name), fe.key_released(name) (pressed
// and released are true for the frame after the event), fe.mouse_x(),
// fe.mouse_y(), fe.mouse_down(button), fe.mouse_pressed(button), fe.mouse_wheel().
// Key names: "a".."z", "0".."9", "space", "enter", "escape", "tab", "backspace",
// "left", "right", "up", "down", "shift", "ctrl", "alt", "f1".."f12"; buttons 0 left, 1 right, 2 middle.
int key_query(lua_State* L, int which) {
    Impl* I = impl_of(L);
    Impl::KeyState* k = I->key_slot(luaL_checkstring(L, 1), false);
    lua_pushboolean(L, k && (which == 0 ? k->down : which == 1 ? k->pressed : k->released));
    return 1;
}
int f_key_down(lua_State* L) { return key_query(L, 0); }
int f_key_pressed(lua_State* L) { return key_query(L, 1); }
int f_key_released(lua_State* L) { return key_query(L, 2); }
int f_mouse_x(lua_State* L) { lua_pushnumber(L, impl_of(L)->mouse_x); return 1; }
int f_mouse_y(lua_State* L) { lua_pushnumber(L, impl_of(L)->mouse_y); return 1; }
int f_mouse_wheel(lua_State* L) { lua_pushnumber(L, impl_of(L)->mouse_wheel); return 1; }
int f_mouse_down(lua_State* L) {
    int b = (int)luaL_optinteger(L, 1, 0);
    lua_pushboolean(L, b >= 0 && b < 8 && (impl_of(L)->mouse_down >> b & 1));
    return 1;
}
int f_mouse_pressed(lua_State* L) {
    int b = (int)luaL_optinteger(L, 1, 0);
    lua_pushboolean(L, b >= 0 && b < 8 && (impl_of(L)->mouse_pressed >> b & 1));
    return 1;
}

int f_time(lua_State* L) {
    Impl* I = impl_of(L);
    lua_pushnumber(L, now_seconds() - I->t0);
    return 1;
}

// print / fe.log (Info), fe.warn (Warning), fe.error (Error): the arguments
// as text, separated by tabs.
int f_print(lua_State* L) {
    Impl* I = impl_of(L);
    int level = (int)lua_tointeger(L, lua_upvalueindex(1));
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, '\t');         // (only luaL_addvalue may run with a value on the stack)
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    I->log((ScriptLogLevel)level, lua_tostring(L, -1));
    return 0;
}

int f_collectgarbage(lua_State* L) {
    static const char* const opts[] = { "collect", "count", "step", nullptr };
    switch (luaL_checkoption(L, 1, "collect", opts)) {
    case 0: lua_gc(L, LUA_GCCOLLECT); return 0;
    case 1: lua_pushnumber(L, (lua_Number)lua_gc(L, LUA_GCCOUNT) + (lua_Number)lua_gc(L, LUA_GCCOUNTB) / 1024.0); return 1;
    default: lua_gc(L, LUA_GCSTEP, 0); return 0;
    }
}

void* lua_alloc(void*, void* ptr, size_t, size_t nsize) {
    if (nsize == 0) { free(ptr); return nullptr; }
    return realloc(ptr, nsize);
}

int lua_panic(lua_State* L) {
    Impl* I = impl_of(L);
    const char* m = lua_tostring(L, -1);
    if (I) I->log(ScriptLogLevel::Error, m ? m : "lua panic");
    return 0;       // Lua aborts
}

void set_funcs_with_upvalue(lua_State* L, const char* name, lua_CFunction f, int upvalue) {
    lua_pushinteger(L, upvalue);
    lua_pushcclosure(L, f, 1);
    lua_setfield(L, -2, name);
}

// A metatable named `name` in the registry with the given functions.
void make_meta(lua_State* L, const char* name, const luaL_Reg* funcs) {
    luaL_newmetatable(L, name);
    luaL_setfuncs(L, funcs, 0);
    lua_pop(L, 1);
}

}  // namespace

// ---------------------------------------------------------------- ScriptRuntime

ScriptRuntime::ScriptRuntime() {}
ScriptRuntime::~ScriptRuntime() { shutdown(); }

bool ScriptRuntime::init(World* world, const ScriptHost& host) {
    shutdown();
    if (!world || !world->valid()) return false;
    Impl* I = new Impl();
    impl_ = I;
    I->world = world;
    I->host = host;
    I->t0 = now_seconds();
    I->hash_grow();
    lua_State* L = lua_newstate(lua_alloc, nullptr);
    if (!L) { delete I; impl_ = nullptr; return false; }
    I->L = L;
    *(Impl**)lua_getextraspace(L) = I;
    lua_atpanic(L, lua_panic);
    lua_gc(L, LUA_GCGEN, 0, 0);

    // The standard libraries a script may see: base (without dofile,
    // loadfile, load, print, warn), coroutine, table, string, math, utf8.
    // No io, os, package or debug.
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L, 1);

    // handle cache, weak values
    lua_newtable(L);
    lua_createtable(L, 0, 1);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    I->cache_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // metatables
    static const luaL_Reg vec_meta[] = {
        { "__add", v_add }, { "__sub", v_sub }, { "__mul", v_mul }, { "__div", v_div }, { "__unm", v_unm },
        { "__eq", v_eq }, { "__tostring", v_tostring }, { nullptr, nullptr } };
    static const luaL_Reg vec_methods[] = {
        { "length", v_length }, { "normalized", v_normalized }, { "dot", f_dot }, { "cross", f_cross }, { nullptr, nullptr } };
    make_meta(L, kVecMeta, vec_meta);
    luaL_getmetatable(L, kVecMeta);
    luaL_newlib(L, vec_methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    static const luaL_Reg obj_meta[] = {
        { "__newindex", obj_newindex }, { "__eq", obj_eq }, { "__tostring", obj_tostring }, { nullptr, nullptr } };
    static const luaL_Reg obj_methods[] = {
        { "children", obj_children }, { "script", obj_script }, { "scripts", obj_scripts }, { "send", obj_send },
        { "move", obj_move }, { "destroy", obj_destroy }, { nullptr, nullptr } };
    make_meta(L, kObjMeta, obj_meta);
    luaL_getmetatable(L, kObjMeta);
    luaL_newlib(L, obj_methods);
    lua_pushcclosure(L, obj_index, 1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    static const luaL_Reg mod_meta[] = {
        { "__index", mod_index }, { "__newindex", mod_newindex }, { "__tostring", mod_tostring }, { nullptr, nullptr } };
    make_meta(L, kModMeta, mod_meta);
    static const luaL_Reg mat_meta[] = {
        { "__index", mat_index }, { "__newindex", mat_newindex }, { "__tostring", mat_tostring }, { nullptr, nullptr } };
    make_meta(L, kMatMeta, mat_meta);

    // the shared library table: the safe base functions, the libraries, fe
    lua_newtable(L);                                    // lib
    static const char* const keep[] = { "assert", "error", "getmetatable", "ipairs", "next", "pairs", "pcall", "rawequal",
        "rawget", "rawlen", "rawset", "select", "setmetatable", "tonumber", "tostring", "type", "xpcall",
        LUA_COLIBNAME, LUA_TABLIBNAME, LUA_STRLIBNAME, LUA_MATHLIBNAME, LUA_UTF8LIBNAME, nullptr };
    lua_pushglobaltable(L);
    for (int i = 0; keep[i]; i++) {
        lua_getfield(L, -1, keep[i]);
        lua_setfield(L, -3, keep[i]);
    }
    lua_pop(L, 1);
    lua_pushcfunction(L, f_collectgarbage);
    lua_setfield(L, -2, "collectgarbage");
    set_funcs_with_upvalue(L, "print", f_print, (int)ScriptLogLevel::Info);

    lua_newtable(L);                                    // fe
    static const luaL_Reg fe_funcs[] = {
        { "vec3", f_vec3 }, { "dot", f_dot }, { "cross", f_cross }, { "find", f_find }, { "current", f_current },
        { "broadcast", f_broadcast }, { "time", f_time },
        { "key_down", f_key_down }, { "key_pressed", f_key_pressed }, { "key_released", f_key_released },
        { "mouse_x", f_mouse_x }, { "mouse_y", f_mouse_y }, { "mouse_wheel", f_mouse_wheel },
        { "mouse_down", f_mouse_down }, { "mouse_pressed", f_mouse_pressed }, { nullptr, nullptr } };
    luaL_setfuncs(L, fe_funcs, 0);
    set_funcs_with_upvalue(L, "log", f_print, (int)ScriptLogLevel::Info);
    set_funcs_with_upvalue(L, "warn", f_print, (int)ScriptLogLevel::Warning);
    set_funcs_with_upvalue(L, "error", f_print, (int)ScriptLogLevel::Error);
    lua_pushnumber(L, 0);
    lua_setfield(L, -2, "dt");
    lua_pushvalue(L, -1);
    I->fe_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "vec3");
    lua_setfield(L, -3, "vec3");                        // lib.vec3 = fe.vec3
    lua_setfield(L, -2, "fe");
    lua_pushvalue(L, -1);
    I->lib_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // the console's environment (ScriptRuntime::run)
    lua_newtable(L);
    lua_createtable(L, 0, 1);
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_G");
    I->console_env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);                                      // lib
    lua_settop(L, 0);

    I->sys_start = world->add_system(Phase::PreUpdate, [](World&, float dt, void* u) { ((Impl*)u)->run_starts(dt); }, I);
    I->sys_update = world->add_system(Phase::Update, [](World&, float dt, void* u) { ((Impl*)u)->run_updates(dt); }, I);
    return true;
}

void ScriptRuntime::shutdown() {
    Impl* I = impl_;
    if (!I) return;
    impl_ = nullptr;
    if (I->world && I->world->valid()) {
        if (I->sys_start) I->world->remove_system(I->sys_start);
        if (I->sys_update) I->world->remove_system(I->sys_update);
    }
    if (I->L) lua_close(I->L);
    for (int i = 0; i < I->count; i++) {
        free(I->list[i]->path);
        delete I->list[i];
    }
    free(I->list);
    free(I->buckets);
    free(I->pending);
    delete I;
}

int ScriptRuntime::add_instance(Entity e, const char* path, const char* source, uint32_t size, const ScriptProp* props, int prop_count,
                                const Entity* object_entities) {
    Impl* I = impl_;
    if (!I || !path || !source || !I->world->alive(e)) return 0;
    lua_State* L = I->L;
    Impl::Scope scope(I);
    const int base = lua_gettop(L);
    luaL_checkstack(L, 12, "add_instance");

    // environment: globals of the script, falling back to the library
    lua_newtable(L);                                    // env
    lua_createtable(L, 0, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, I->lib_ref);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_G");

    // the chunk, text only, with the environment as _ENV
    char chunk[300];
    snprintf(chunk, sizeof chunk, "@%s", path);
    if (luaL_loadbufferx(L, source, size, chunk, "t") != LUA_OK) {
        I->log(ScriptLogLevel::Error, lua_tostring(L, -1));
        lua_settop(L, base);
        return 0;
    }
    lua_pushvalue(L, base + 1);
    lua_setupvalue(L, -2, 1);                           // stack: env chunk

    // self: the inputs, object, name; unknown keys fall back to the environment
    lua_createtable(L, 0, prop_count + 4);              // env chunk self
    for (int i = 0; i < prop_count && props; i++) {
        const ScriptProp& p = props[i];
        switch (p.type) {
        case PropType::Number: {
            double d = p.number;
            if (d == (double)(long long)d && d > -9e15 && d < 9e15) lua_pushinteger(L, (lua_Integer)d);
            else lua_pushnumber(L, d);
            break;
        }
        case PropType::Text: lua_pushstring(L, p.text); break;
        case PropType::Toggle: lua_pushboolean(L, p.toggle); break;
        case PropType::List:
            lua_pushstring(L, p.choice >= 0 && p.choice < p.choice_count ? p.choices[p.choice] : "");
            break;
        case PropType::Object: {
            Entity t = object_entities ? object_entities[i] : 0;
            if (t && I->world->alive(t)) push_object(L, t); else lua_pushnil(L);
            break;
        }
        case PropType::File: lua_pushstring(L, p.file); break;
        }
        lua_setfield(L, -2, p.name);
    }
    push_object(L, e);
    lua_setfield(L, -2, "object");
    {
        char nm[64];
        snprintf(nm, sizeof nm, "%s", base_name(path));
        char* dot = strrchr(nm, '.');
        if (dot && dot != nm) *dot = 0;
        lua_pushstring(L, nm);
        lua_setfield(L, -2, "name");
    }
    lua_createtable(L, 0, 2);
    lua_pushvalue(L, base + 1);
    lua_setfield(L, -2, "env");
    lua_pushcfunction(L, self_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);

    // run the chunk (defines the callbacks in the environment)
    lua_pushcfunction(L, Impl::msgh);                   // env chunk self msgh
    lua_pushvalue(L, base + 2);
    int status;
    I->depth++;
    status = lua_pcall(L, 0, 0, base + 4);
    I->depth--;
    if (status != LUA_OK) {
        I->log(ScriptLogLevel::Error, lua_tostring(L, -1));
        lua_settop(L, base);
        return 0;
    }
    lua_pop(L, 1);                                      // msgh

    Inst* in = new Inst();
    in->id = I->next_id++;
    in->e = e;
    in->path = (char*)malloc(strlen(path) + 1);
    memcpy(in->path, path, strlen(path) + 1);
    lua_pushvalue(L, base + 1);
    in->env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, base + 3);
    in->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_settop(L, base);

    if (I->count == I->cap) {
        I->cap = I->cap ? I->cap * 2 : 64;
        I->list = (Inst**)realloc(I->list, sizeof(Inst*) * (size_t)I->cap);
    }
    I->list[I->count++] = in;
    I->live++;
    I->stats.instances = I->live;
    if (I->live > I->nbuckets * 2) I->hash_grow(); else I->hash_insert(in);
    return in->id;
}

void ScriptRuntime::remove_instances(Entity e) {
    Impl* I = impl_;
    if (!I) return;
    I->remove_entity(e);
    I->stats.instances = I->live;
    if (I->depth == 0) I->flush();
}

void ScriptRuntime::key_event(const char* name, bool down) {
    if (!impl_ || !name) return;
    Impl::KeyState* k = impl_->key_slot(name, true);
    if (!k || k->down == down) return;
    k->down = down;
    (down ? k->pressed : k->released) = true;
}

void ScriptRuntime::mouse_move(float x, float y) {
    if (!impl_) return;
    impl_->mouse_x = x;
    impl_->mouse_y = y;
}

void ScriptRuntime::mouse_button(int button, bool down) {
    if (!impl_ || button < 0 || button > 7) return;
    uint8_t bit = (uint8_t)(1u << button);
    if (((impl_->mouse_down & bit) != 0) == down) return;
    if (down) { impl_->mouse_down |= bit; impl_->mouse_pressed |= bit; }
    else { impl_->mouse_down &= (uint8_t)~bit; impl_->mouse_released |= bit; }
}

void ScriptRuntime::mouse_wheel(float dy) {
    if (impl_) impl_->mouse_wheel += dy;
}

void ScriptRuntime::send(Entity target, const char* event) {
    Impl* I = impl_;
    if (!I || !event || I->dead(target)) return;
    I->deliver(I->L, target, event, 0, 0);
}

void ScriptRuntime::trigger_enter(Entity owner, Entity other) {
    Impl* I = impl_;
    if (!I || I->dead(owner) || I->dead(other)) return;
    Impl::Scope s(I);
    Inst* tmp[64];
    int n = I->collect(owner, tmp, 64);
    for (int i = 0; i < n; i++)
        I->invoke(I->L, tmp[i], "on_trigger_enter", [other](lua_State* l) { push_object(l, other); return 1; });
}

void ScriptRuntime::trigger_exit(Entity owner, Entity other) {
    Impl* I = impl_;
    if (!I || I->dead(owner) || I->dead(other)) return;
    Impl::Scope s(I);
    Inst* tmp[64];
    int n = I->collect(owner, tmp, 64);
    for (int i = 0; i < n; i++)
        I->invoke(I->L, tmp[i], "on_trigger_exit", [other](lua_State* l) { push_object(l, other); return 1; });
}

void ScriptRuntime::collision(Entity a, Entity b) {
    Impl* I = impl_;
    if (!I || I->dead(a) || I->dead(b)) return;
    Impl::Scope s(I);
    Inst* tmp[64];
    int n = I->collect(a, tmp, 64);
    for (int i = 0; i < n; i++)
        I->invoke(I->L, tmp[i], "on_collision", [b](lua_State* l) { push_object(l, b); return 1; });
    n = I->collect(b, tmp, 64);
    for (int i = 0; i < n; i++)
        I->invoke(I->L, tmp[i], "on_collision", [a](lua_State* l) { push_object(l, a); return 1; });
}

namespace {

// Pushes self.<field> of an instance; false (nothing pushed) if there is none.
bool push_field(Impl* I, int instance, const char* field) {
    Inst* in = I->by_id(instance);
    if (!in) return false;
    lua_State* L = I->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, in->self_ref);
    lua_pushstring(L, field);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    return true;
}

}  // namespace

bool ScriptRuntime::get_number(int instance, const char* field, double* out) {
    Impl* I = impl_;
    if (!I || !push_field(I, instance, field)) return false;
    bool ok = lua_type(I->L, -1) == LUA_TNUMBER;
    if (ok && out) *out = lua_tonumber(I->L, -1);
    lua_pop(I->L, 1);
    return ok;
}

bool ScriptRuntime::get_bool(int instance, const char* field, bool* out) {
    Impl* I = impl_;
    if (!I || !push_field(I, instance, field)) return false;
    bool ok = lua_type(I->L, -1) == LUA_TBOOLEAN;
    if (ok && out) *out = lua_toboolean(I->L, -1) != 0;
    lua_pop(I->L, 1);
    return ok;
}

bool ScriptRuntime::get_string(int instance, const char* field, char* out, int cap) {
    Impl* I = impl_;
    if (!I || !push_field(I, instance, field)) return false;
    bool ok = lua_type(I->L, -1) == LUA_TSTRING;
    if (ok && out && cap > 0) {
        size_t len;
        const char* s = lua_tolstring(I->L, -1, &len);
        size_t n = len < (size_t)cap - 1 ? len : (size_t)cap - 1;
        memcpy(out, s, n);
        out[n] = 0;
    }
    lua_pop(I->L, 1);
    return ok;
}

bool ScriptRuntime::add_module_method(ModuleKind kind, const char* name, ModuleMethod fn, void* user) {
    if (!impl_ || !name || !fn || strlen(name) >= sizeof(impl_->methods[0].name)) return false;
    Impl* I = impl_;
    int slot = I->method_count;
    for (int i = 0; i < I->method_count; i++)
        if (I->methods[i].kind == (int)kind && strcmp(I->methods[i].name, name) == 0) slot = i;
    if (slot == I->method_count) {
        if (I->method_count >= 32) return false;
        I->method_count++;
    }
    Impl::Method& m = I->methods[slot];
    m.kind = (int)kind;
    strcpy(m.name, name);
    m.fn = fn;
    m.user = user;
    return true;
}

bool ScriptRuntime::run(const char* code) {
    Impl* I = impl_;
    if (!I || !code) return false;
    lua_State* L = I->L;
    Impl::Scope scope(I);
    const int base = lua_gettop(L);
    lua_pushcfunction(L, Impl::msgh);
    if (luaL_loadbufferx(L, code, strlen(code), "=console", "t") != LUA_OK) {
        I->log(ScriptLogLevel::Error, lua_tostring(L, -1));
        lua_settop(L, base);
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, I->console_env_ref);
    lua_setupvalue(L, -2, 1);
    I->depth++;
    int status = lua_pcall(L, 0, 0, base + 1);
    I->depth--;
    bool ok = status == LUA_OK;
    if (!ok) I->log(ScriptLogLevel::Error, lua_tostring(L, -1));
    lua_settop(L, base);
    return ok;
}

const ScriptStats& ScriptRuntime::stats() const {
    static const ScriptStats none;
    if (!impl_) return none;
    impl_->stats.instances = impl_->live;
    return impl_->stats;
}

double ScriptRuntime::time() const { return impl_ ? now_seconds() - impl_->t0 : 0; }

}  // namespace fe
