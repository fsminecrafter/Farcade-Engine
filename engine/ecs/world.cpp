// The entity system on flecs. See ecs.h and docs/ecs.md.
#include "ecs/ecs.h"

#include "flecs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fe {

// ======================================================================= OS API
// flecs is built without its own OS layer (FLECS_NO_OS_API_IMPL); this is the
// same on the host and on Minimal-OS: libc malloc, the libc clock, logging to
// stdout, no threads.

namespace {

void* os_malloc(ecs_size_t n) { return malloc((size_t)n); }
void* os_realloc(void* p, ecs_size_t n) { return realloc(p, (size_t)n); }
void* os_calloc(ecs_size_t n) { return calloc(1, (size_t)n); }
void os_free(void* p) { free(p); }
char* os_strdup(const char* s) {
    if (!s) return nullptr;
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
int32_t os_ainc(int32_t* v) { return ++*v; }
int32_t os_adec(int32_t* v) { return --*v; }
int64_t os_lainc(int64_t* v) { return ++*v; }
int64_t os_ladec(int64_t* v) { return --*v; }
void os_sleep(int32_t sec, int32_t nsec) {
    timespec ts;
    ts.tv_sec = sec;
    ts.tv_nsec = nsec;
    nanosleep(&ts, nullptr);
}
uint64_t os_now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void os_get_time(ecs_time_t* t) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->sec = (uint32_t)ts.tv_sec;
    t->nanosec = (uint32_t)ts.tv_nsec;
}
void os_log(int32_t level, const char* file, int32_t line, const char* msg) {
    if (level <= -2) printf("[flecs] %s (%s:%d)\n", msg, file ? file : "", (int)line);
}
void os_abort() { abort(); }

bool g_os_ready = false;

void setup_os_api() {
    if (g_os_ready) return;
    g_os_ready = true;
    ecs_os_set_api_defaults();
    ecs_os_api_t api = ecs_os_get_api();
    api.malloc_ = os_malloc;
    api.realloc_ = os_realloc;
    api.calloc_ = os_calloc;
    api.free_ = os_free;
    api.strdup_ = os_strdup;
    api.ainc_ = os_ainc;
    api.adec_ = os_adec;
    api.lainc_ = os_lainc;
    api.ladec_ = os_ladec;
    api.sleep_ = os_sleep;
    api.now_ = os_now;
    api.get_time_ = os_get_time;
    api.log_ = os_log;
    api.abort_ = os_abort;
    ecs_os_set_api(&api);
    ecs_log_set_level(-1);
}

// ---- default values: components are created with these (flecs does not run
// C++ member initialisers).
const ModuleSet& default_modules() {
    static const ModuleSet s;
    return s;
}
const Transform& default_transform() { static const Transform t; return t; }
const WorldTransform& default_world_transform() { static const WorldTransform t; return t; }
const Renderable& default_renderable() { static const Renderable r; return r; }

// ctor hook: copies the default instance in hooks.ctx into every element.
void ctor_defaults(void* ptr, int32_t count, const ecs_type_info_t* ti) {
    const void* def = ti->hooks.ctx;
    size_t size = (size_t)ti->size;
    for (int32_t i = 0; i < count; i++) memcpy((char*)ptr + (size_t)i * size, def, size);
}

}  // namespace

// ======================================================================= Impl

struct WorldImpl {
    ecs_world_t* w = nullptr;       // the real world
    ecs_world_t* cur = nullptr;     // what operations use: the real world, or the stage inside a callback
    ecs_entity_t c_transform = 0, c_world = 0, c_renderable = 0;
    ecs_entity_t c_module[kModuleKindCount] = {};
    ecs_entity_t t_vec3 = 0, t_quat = 0;
    ecs_entity_t sys_transform = 0;
    ecs_query_t* cur_query[kModuleKindCount] = {};
    struct CurCtx { WorldImpl* impl; int kind; } cur_ctx[kModuleKindCount] = {};
    // Object links: ids to entities (open addressing, id 0 = empty slot).
    struct LinkSlot { uint32_t id; Entity e; };
    LinkSlot* links = nullptr;
    uint32_t link_cap = 0, link_used = 0;
    ~WorldImpl() { free(links); }
    void link_put(uint32_t id, Entity e) {
        if ((link_used + 1) * 2 > link_cap) {
            uint32_t nc = link_cap ? link_cap * 2 : 64;
            LinkSlot* nl = (LinkSlot*)calloc(nc, sizeof(LinkSlot));
            for (uint32_t i = 0; i < link_cap; i++)
                if (links[i].id) {
                    uint32_t h = (links[i].id * 2654435761u) & (nc - 1);
                    while (nl[h].id) h = (h + 1) & (nc - 1);
                    nl[h] = links[i];
                }
            free(links);
            links = nl;
            link_cap = nc;
        }
        uint32_t h = (id * 2654435761u) & (link_cap - 1);
        while (links[h].id && links[h].id != id) h = (h + 1) & (link_cap - 1);
        if (!links[h].id) link_used++;
        links[h] = { id, e };
    }
    Entity link_get(uint32_t id) const {
        if (!id || !link_cap) return 0;
        uint32_t h = (id * 2654435761u) & (link_cap - 1);
        while (links[h].id) {
            if (links[h].id == id) return links[h].e;
            h = (h + 1) & (link_cap - 1);
        }
        return 0;
    }
};

namespace {

struct SysCtx {
    World* world;
    WorldImpl* impl;
    FrameFn fn;
    void* user;
};

void sys_run(ecs_iter_t* it) {
    SysCtx* c = (SysCtx*)it->ctx;
    ecs_world_t* prev = c->impl->cur;
    c->impl->cur = it->world;
    c->fn(*c->world, it->delta_time, c->user);
    c->impl->cur = prev;
}
void sys_ctx_free(void* p) { delete (SysCtx*)p; }

void transform_cb(ecs_iter_t* it) {
    const Transform* t = (const Transform*)ecs_field_w_size(it, sizeof(Transform), 0);
    WorldTransform* wt = (WorldTransform*)ecs_field_w_size(it, sizeof(WorldTransform), 1);
    const WorldTransform* pw = ecs_field_is_set(it, 2)
        ? (const WorldTransform*)ecs_field_w_size(it, sizeof(WorldTransform), 2) : nullptr;
    for (int32_t i = 0; i < it->count; i++) {
        Mat4 local = Mat4::trs(t[i].position, t[i].rotation, t[i].scale);
        wt[i].matrix = pw ? pw->matrix * local : local;
    }
}

// ---- the current rule

// Clears the flag on every entity of `kind` except `keep`.
void enforce_current(WorldImpl* I, int kind, ecs_entity_t keep) {
    const ModuleInfo& mi = module_info((ModuleKind)kind);
    ecs_iter_t qit = ecs_query_iter(I->w, I->cur_query[kind]);
    while (ecs_query_next(&qit)) {
        char* d = (char*)ecs_field_w_size(&qit, mi.size, 0);
        for (int32_t i = 0; i < qit.count; i++) {
            if (qit.entities[i] != keep && get_current(mi, d + (size_t)i * mi.size))
                set_current_flag(mi, d + (size_t)i * mi.size, false);
        }
    }
}

// OnSet of a module with a current flag: whoever turned it on is the current one.
void on_current_set(ecs_iter_t* it) {
    WorldImpl::CurCtx* c = (WorldImpl::CurCtx*)it->ctx;
    const ModuleInfo& mi = module_info((ModuleKind)c->kind);
    const char* d = (const char*)ecs_field_w_size(it, mi.size, 0);
    ecs_entity_t keep = 0;
    for (int32_t i = 0; i < it->count; i++)
        if (get_current(mi, d + (size_t)i * mi.size)) keep = it->entities[i];
    if (keep) enforce_current(c->impl, c->kind, keep);
}

// ---- registration

ecs_entity_t make_named(ecs_world_t* w, const char* name) {
    ecs_entity_desc_t ed = {};
    ed.name = name;
    return ecs_entity_init(w, &ed);
}

ecs_entity_t make_component(ecs_world_t* w, const char* name, size_t size, size_t align, const void* defaults) {
    ecs_entity_t e = make_named(w, name);
    ecs_component_desc_t cd = {};
    cd.entity = e;
    cd.type.size = (ecs_size_t)size;
    cd.type.alignment = (ecs_size_t)align;
    cd.type.hooks.ctor = ctor_defaults;
    cd.type.hooks.ctx = const_cast<void*>(defaults);
    return ecs_component_init(w, &cd);
}

struct MemberDef {
    const char* name;
    ecs_entity_t type;
    int32_t count;
    int32_t offset;
};

bool reg_fail(int line) {
    printf("[ecs] registration failed (world.cpp:%d)\n", line);
    return false;
}

bool make_struct(ecs_world_t* w, ecs_entity_t e, const MemberDef* m, int n, size_t size) {
    if (n > ECS_MEMBER_DESC_CACHE_SIZE) return reg_fail(__LINE__);
    ecs_struct_desc_t sd = {};
    sd.entity = e;
    for (int i = 0; i < n; i++) {
        sd.members[i].name = m[i].name;
        sd.members[i].type = m[i].type;
        sd.members[i].count = m[i].count;
        sd.members[i].offset = m[i].offset;
        sd.members[i].use_offset = true;
    }
    if (!ecs_struct_init(w, &sd)) return reg_fail(__LINE__);
    const EcsComponent* c = ecs_get(w, e, EcsComponent);
    if (!c || (size_t)c->size != size) return reg_fail(__LINE__);
    return true;
}

#define MEMBER(T, field, ty) { #field, ty, 0, (int32_t)offsetof(T, field) }

bool register_meta_types(WorldImpl* I) {
    ecs_world_t* w = I->w;
    // Vec3 {x,y,z}, Quat {x,y,z,w}
    I->t_vec3 = make_component(w, "fe.Vec3", sizeof(Vec3), alignof(Vec3), nullptr);
    I->t_quat = make_component(w, "fe.Quat", sizeof(Quat), alignof(Quat), nullptr);
    {
        MemberDef m[] = { MEMBER(Vec3, x, ecs_id(ecs_f32_t)), MEMBER(Vec3, y, ecs_id(ecs_f32_t)),
                          MEMBER(Vec3, z, ecs_id(ecs_f32_t)) };
        if (!make_struct(w, I->t_vec3, m, 3, sizeof(Vec3))) return reg_fail(__LINE__);
        MemberDef q[] = { MEMBER(Quat, x, ecs_id(ecs_f32_t)), MEMBER(Quat, y, ecs_id(ecs_f32_t)),
                          MEMBER(Quat, z, ecs_id(ecs_f32_t)), MEMBER(Quat, w, ecs_id(ecs_f32_t)) };
        if (!make_struct(w, I->t_quat, q, 4, sizeof(Quat))) return reg_fail(__LINE__);
    }
    return true;
}

ecs_entity_t make_enum(ecs_world_t* w, const char* name, const char* const* names) {
    ecs_enum_desc_t ed = {};
    ed.entity = make_named(w, name);
    ed.underlying_type = ecs_id(ecs_u8_t);
    int n = 0;
    for (; names[n] && n < ECS_MEMBER_DESC_CACHE_SIZE; n++) {
        ed.constants[n].name = names[n];
        ed.constants[n].value_unsigned = (uint64_t)n;     // the underlying type is unsigned
    }
    return ecs_enum_init(w, &ed);
}

bool register_module_meta(WorldImpl* I, ModuleKind k) {
    ecs_world_t* w = I->w;
    const ModuleInfo& mi = module_info(k);
    MemberDef m[ECS_MEMBER_DESC_CACHE_SIZE];
    // flecs describes at most 32 members of a struct. The meta data is only reflection (field access goes
    // through FieldInfo), so a longer table (Terrain with its foliage layers) lists its first 32 fields.
    int count = mi.field_count < ECS_MEMBER_DESC_CACHE_SIZE ? mi.field_count : ECS_MEMBER_DESC_CACHE_SIZE;
    for (int i = 0; i < count; i++) {
        const FieldInfo& f = mi.fields[i];
        m[i].name = f.name;
        m[i].count = 0;
        m[i].offset = f.offset;
        switch (f.type) {
        case FieldType::Float: m[i].type = ecs_id(ecs_f32_t); break;
        case FieldType::Int: m[i].type = ecs_id(ecs_i32_t); break;
        case FieldType::Bool: m[i].type = ecs_id(ecs_bool_t); break;
        case FieldType::Bits:
        case FieldType::Object: m[i].type = ecs_id(ecs_u32_t); break;
        case FieldType::Vec3:
        case FieldType::Color: m[i].type = I->t_vec3; break;
        case FieldType::Text:
        case FieldType::Path: m[i].type = ecs_id(ecs_char_t); m[i].count = kPathMax; break;
        case FieldType::Enum: {
            char nm[96];
            snprintf(nm, sizeof nm, "fe.enums.%s_%s", mi.name, f.name);
            m[i].type = make_enum(w, nm, f.enum_names);
            if (!m[i].type) return reg_fail(__LINE__);
            break;
        }
        }
    }
    return make_struct(w, I->c_module[(int)k], m, count, mi.size);
}

bool register_all(WorldImpl* I) {
    ecs_world_t* w = I->w;
    if (!register_meta_types(I)) return reg_fail(__LINE__);

    // Transform, WorldTransform, Renderable
    I->c_transform = make_component(w, "fe.Transform", sizeof(Transform), alignof(Transform), &default_transform());
    I->c_world = make_component(w, "fe.WorldTransform", sizeof(WorldTransform), alignof(WorldTransform),
                                &default_world_transform());
    I->c_renderable = make_component(w, "fe.Renderable", sizeof(Renderable), alignof(Renderable), &default_renderable());
    {
        MemberDef t[] = { MEMBER(Transform, position, I->t_vec3), MEMBER(Transform, rotation, I->t_quat),
                          MEMBER(Transform, scale, I->t_vec3) };
        if (!make_struct(w, I->c_transform, t, 3, sizeof(Transform))) return reg_fail(__LINE__);
        MemberDef wt[] = { { "matrix", ecs_id(ecs_f32_t), 16, (int32_t)offsetof(WorldTransform, matrix) } };
        if (!make_struct(w, I->c_world, wt, 1, sizeof(WorldTransform))) return reg_fail(__LINE__);
        MemberDef r[] = { MEMBER(Renderable, mesh, ecs_id(ecs_u8_t)),
                          { "model_path", ecs_id(ecs_char_t), kPathMax, (int32_t)offsetof(Renderable, model_path) },
                          { "texture_path", ecs_id(ecs_char_t), kPathMax, (int32_t)offsetof(Renderable, texture_path) },
                          { "shader_path", ecs_id(ecs_char_t), kPathMax, (int32_t)offsetof(Renderable, shader_path) },
                          MEMBER(Renderable, color, I->t_vec3), MEMBER(Renderable, visible, ecs_id(ecs_bool_t)),
                          MEMBER(Renderable, roughness, ecs_id(ecs_f32_t)), MEMBER(Renderable, metallic, ecs_id(ecs_f32_t)),
                          MEMBER(Renderable, unlit, ecs_id(ecs_bool_t)), MEMBER(Renderable, cast_shadows, ecs_id(ecs_bool_t)) };
        if (!make_struct(w, I->c_renderable, r, 10, sizeof(Renderable))) return reg_fail(__LINE__);
    }

    // every ModuleKind
    for (int k = 0; k < kModuleKindCount; k++) {
        const ModuleInfo& mi = module_info((ModuleKind)k);
        char nm[64];
        snprintf(nm, sizeof nm, "fe.%s", mi.name);
        I->c_module[k] = make_component(w, nm, mi.size, mi.align, default_modules().data((ModuleKind)k));
        if (!I->c_module[k]) return reg_fail(__LINE__);
    }
    for (int k = 0; k < kModuleKindCount; k++)
        if (!register_module_meta(I, (ModuleKind)k)) return reg_fail(__LINE__);

    // the current rule: a query over each such kind, and an OnSet observer
    for (int k = 0; k < kModuleKindCount; k++) {
        const ModuleInfo& mi = module_info((ModuleKind)k);
        if (!mi.has_current) continue;
        I->cur_ctx[k] = { I, k };
        ecs_query_desc_t qd = {};
        qd.terms[0].id = I->c_module[k];
        I->cur_query[k] = ecs_query_init(w, &qd);
        ecs_observer_desc_t od = {};
        od.query.terms[0].id = I->c_module[k];
        od.events[0] = EcsOnSet;
        od.callback = on_current_set;
        od.ctx = &I->cur_ctx[k];
        if (!I->cur_query[k] || !ecs_observer_init(w, &od)) return reg_fail(__LINE__);
    }

    // the transform system: parents before children (cascade over ChildOf),
    // in PostUpdate so what runs in PreUpdate/OnUpdate (scripts, physics) is
    // already applied, and what runs in OnStore (audio, render) sees it.
    ecs_system_desc_t sd = {};
    sd.entity = make_named(w, "fe.systems.transform");
    sd.query.terms[0].id = I->c_transform;
    sd.query.terms[0].inout = EcsIn;
    sd.query.terms[1].id = I->c_world;
    sd.query.terms[1].inout = EcsOut;
    sd.query.terms[2].id = I->c_world;
    sd.query.terms[2].src.id = EcsCascade;
    sd.query.terms[2].trav = EcsChildOf;
    sd.query.terms[2].oper = EcsOptional;
    sd.query.terms[2].inout = EcsIn;
    sd.phase = EcsPostUpdate;
    sd.callback = transform_cb;
    I->sys_transform = ecs_system_init(w, &sd);
    return I->sys_transform != 0;
}

ecs_entity_t phase_id(Phase p) {
    switch (p) {
    case Phase::PreUpdate: return EcsPreUpdate;
    case Phase::Update: return EcsOnUpdate;
    case Phase::PostUpdate: return EcsPostUpdate;
    case Phase::Store: return EcsOnStore;
    }
    return EcsOnUpdate;
}

}  // namespace

// ======================================================================= World

World::World() {}
World::~World() { shutdown(); }

bool World::init() {
    if (impl_) return true;
    setup_os_api();
    WorldImpl* I = new WorldImpl();
    I->w = ecs_init();
    I->cur = I->w;
    if (!I->w) { delete I; return false; }
    if (!register_all(I)) {
        printf("[ecs] registration failed\n");
        ecs_fini(I->w);
        delete I;
        return false;
    }
    impl_ = I;
    return true;
}

void World::shutdown() {
    if (!impl_) return;
    ecs_fini(impl_->w);
    delete impl_;
    impl_ = nullptr;
}

ecs_world_t* World::raw() const { return impl_ ? impl_->cur : nullptr; }
Entity World::component(ModuleKind k) const { return impl_->c_module[(int)k]; }
Entity World::transform_component() const { return impl_->c_transform; }
Entity World::world_transform_component() const { return impl_->c_world; }
Entity World::renderable_component() const { return impl_->c_renderable; }

// ---- entities

bool World::alive(Entity e) const { return impl_ && e && ecs_is_alive(impl_->cur, e); }

Entity World::create(const char* name, Entity parent) {
    ecs_world_t* w = impl_->cur;
    if (parent && !alive(parent)) return 0;
    if (name && !*name) name = nullptr;
    if (name && ecs_lookup_child(w, parent, name)) return 0;
    ecs_entity_t e = ecs_new(w);
    if (parent) ecs_add_pair(w, e, EcsChildOf, parent);
    if (name) ecs_set_name(w, e, name);
    ecs_add_id(w, e, impl_->c_transform);
    ecs_add_id(w, e, impl_->c_world);
    return e;
}

void World::destroy(Entity e) {
    if (alive(e)) ecs_delete(impl_->cur, e);
}

Entity World::lookup(const char* name) const { return lookup_child(0, name); }

Entity World::lookup_child(Entity parent, const char* name) const {
    if (!name || !*name || (parent && !alive(parent))) return 0;
    return ecs_lookup_child(impl_->cur, parent, name);
}

const char* World::name(Entity e) const { return alive(e) ? ecs_get_name(impl_->cur, e) : nullptr; }

bool World::set_name(Entity e, const char* name) {
    if (!alive(e)) return false;
    ecs_world_t* w = impl_->cur;
    if (!name || !*name) {
        ecs_remove_pair(w, e, ecs_id(EcsIdentifier), EcsName);
        return true;
    }
    ecs_entity_t other = ecs_lookup_child(w, ecs_get_parent(w, e), name);
    if (other && other != e) return false;
    ecs_set_name(w, e, name);
    return true;
}

// ---- hierarchy

Entity World::parent(Entity e) const { return alive(e) ? ecs_get_parent(impl_->cur, e) : 0; }

bool World::set_parent(Entity child, Entity parent) {
    ecs_world_t* w = impl_->cur;
    if (!alive(child) || (parent && !alive(parent)) || child == parent) return false;
    for (ecs_entity_t p = parent; p; p = ecs_get_parent(w, p))
        if (p == child) return false;                       // a cycle
    const char* nm = ecs_get_name(w, child);
    if (nm) {
        ecs_entity_t other = ecs_lookup_child(w, parent, nm);
        if (other && other != child) return false;          // a sibling has this name
    }
    if (parent) ecs_add_pair(w, child, EcsChildOf, parent);
    else ecs_remove_pair(w, child, EcsChildOf, EcsWildcard);
    return true;
}

int World::children(Entity parent, Entity* out, int max) const {
    if (!alive(parent)) return 0;
    int n = 0;
    ecs_iter_t it = ecs_children(impl_->cur, parent);
    while (ecs_children_next(&it)) {
        for (int32_t i = 0; i < it.count; i++) {
            if (out && n < max) out[n] = it.entities[i];
            n++;
        }
    }
    return n;
}

int World::child_count(Entity parent) const { return children(parent, nullptr, 0); }

// ---- transform, renderable

Transform* World::transform(Entity e) {
    return alive(e) ? (Transform*)ecs_get_mut_id(impl_->cur, e, impl_->c_transform) : nullptr;
}
const Transform* World::transform(Entity e) const {
    return alive(e) ? (const Transform*)ecs_get_id(impl_->cur, e, impl_->c_transform) : nullptr;
}
const WorldTransform* World::world_transform(Entity e) const {
    return alive(e) ? (const WorldTransform*)ecs_get_id(impl_->cur, e, impl_->c_world) : nullptr;
}
void World::update_transforms() { ecs_run(impl_->w, impl_->sys_transform, 0.0f, nullptr); }

Renderable* World::add_renderable(Entity e) {
    if (!alive(e)) return nullptr;
    ecs_add_id(impl_->cur, e, impl_->c_renderable);
    return (Renderable*)ecs_get_mut_id(impl_->cur, e, impl_->c_renderable);
}
Renderable* World::renderable(Entity e) {
    return alive(e) ? (Renderable*)ecs_get_mut_id(impl_->cur, e, impl_->c_renderable) : nullptr;
}
const Renderable* World::renderable(Entity e) const {
    return alive(e) ? (const Renderable*)ecs_get_id(impl_->cur, e, impl_->c_renderable) : nullptr;
}
void World::remove_renderable(Entity e) {
    if (alive(e)) ecs_remove_id(impl_->cur, e, impl_->c_renderable);
}

// ---- modules

bool World::has(Entity e, ModuleKind k) const {
    return alive(e) && (int)k < kModuleKindCount && ecs_has_id(impl_->cur, e, impl_->c_module[(int)k]);
}
bool World::add(Entity e, ModuleKind k) {
    if (!alive(e) || (int)k >= kModuleKindCount) return false;
    ecs_add_id(impl_->cur, e, impl_->c_module[(int)k]);
    return true;
}
void World::remove(Entity e, ModuleKind k) {
    if (alive(e) && (int)k < kModuleKindCount) ecs_remove_id(impl_->cur, e, impl_->c_module[(int)k]);
}
void* World::get_mut(Entity e, ModuleKind k) {
    if (!alive(e) || (int)k >= kModuleKindCount) return nullptr;
    return ecs_get_mut_id(impl_->cur, e, impl_->c_module[(int)k]);
}
const void* World::get(Entity e, ModuleKind k) const {
    if (!alive(e) || (int)k >= kModuleKindCount) return nullptr;
    return ecs_get_id(impl_->cur, e, impl_->c_module[(int)k]);
}
void World::modified(Entity e, ModuleKind k) {
    if (has(e, k)) ecs_modified_id(impl_->cur, e, impl_->c_module[(int)k]);
}
bool World::set(Entity e, ModuleKind k, const void* data) {
    if (!alive(e) || (int)k >= kModuleKindCount || !data) return false;
    ecs_set_id(impl_->cur, e, impl_->c_module[(int)k], module_info(k).size, data);
    return true;
}
void World::load(Entity e, const ModuleSet& set) {
    for (int k = 0; k < kModuleKindCount; k++)
        if (set.has((ModuleKind)k)) this->set(e, (ModuleKind)k, set.data((ModuleKind)k));
}
void World::store(Entity e, ModuleSet* out) const {
    out->present = 0;
    for (int k = 0; k < kModuleKindCount; k++) {
        const void* d = get(e, (ModuleKind)k);
        if (!d) continue;
        memcpy(out->data((ModuleKind)k), d, module_info((ModuleKind)k).size);
        out->add((ModuleKind)k);
    }
}

// ---- fields

bool World::get_field(Entity e, ModuleKind k, const char* field, FieldValue* out) const {
    if ((int)k >= kModuleKindCount || !field || !out) return false;
    const FieldInfo* f = find_field(module_info(k), field);
    const char* d = (const char*)get(e, k);
    if (!f || !d) return false;
    const char* p = d + f->offset;
    FieldValue r;
    r.type = f->type;
    switch (f->type) {
    case FieldType::Float: r.f = *(const float*)p; r.i = (int32_t)r.f; break;
    case FieldType::Int: r.i = *(const int32_t*)p; r.f = (float)r.i; break;
    case FieldType::Bool: r.b = *(const bool*)p; break;
    case FieldType::Enum: {
        r.i = *(const uint8_t*)p;
        int n = 0;
        while (f->enum_names && f->enum_names[n]) n++;
        r.s = r.i < n ? f->enum_names[r.i] : nullptr;
        break;
    }
    case FieldType::Bits:
    case FieldType::Object: r.i = (int32_t)*(const uint32_t*)p; break;
    case FieldType::Vec3:
    case FieldType::Color: r.v = *(const Vec3*)p; break;
    case FieldType::Text:
    case FieldType::Path: r.s = p; break;
    }
    *out = r;
    return true;
}

bool World::set_field(Entity e, ModuleKind k, const char* field, const FieldValue& v) {
    if ((int)k >= kModuleKindCount || !field) return false;
    const FieldInfo* f = find_field(module_info(k), field);
    char* d = (char*)get_mut(e, k);
    if (!f || !d) return false;
    char* p = d + f->offset;
    const bool ranged = f->min != f->max;
    switch (f->type) {
    case FieldType::Float: {
        float x;
        if (v.type == FieldType::Float) x = v.f;
        else if (v.type == FieldType::Int) x = (float)v.i;
        else return false;
        if (x != x) return false;
        if (ranged) x = fclamp(x, f->min, f->max);
        *(float*)p = x;
        break;
    }
    case FieldType::Int: {
        float x;
        if (v.type == FieldType::Int) x = (float)v.i;
        else if (v.type == FieldType::Float) x = v.f;
        else return false;
        if (x != x) return false;
        if (ranged) x = fclamp(x, f->min, f->max);
        *(int32_t*)p = v.type == FieldType::Int && !ranged ? v.i : (int32_t)(x < 0 ? x - 0.5f : x + 0.5f);
        break;
    }
    case FieldType::Bool:
        if (v.type != FieldType::Bool) return false;
        *(bool*)p = v.b;
        break;
    case FieldType::Enum: {
        int n = 0;
        while (f->enum_names && f->enum_names[n]) n++;
        int idx = -1;
        if (v.type == FieldType::Enum || v.type == FieldType::Int) idx = v.i;
        else if (v.type == FieldType::Path && v.s) {
            for (int i = 0; i < n; i++)
                if (strcmp(f->enum_names[i], v.s) == 0) idx = i;
        }
        if (idx < 0 || idx >= n) return false;
        *(uint8_t*)p = (uint8_t)idx;
        break;
    }
    case FieldType::Bits:
        if (v.type != FieldType::Bits && v.type != FieldType::Int) return false;
        *(uint32_t*)p = (uint32_t)v.i;
        break;
    case FieldType::Object:
        if (v.type != FieldType::Object && v.type != FieldType::Int) return false;
        *(uint32_t*)p = (uint32_t)v.i;
        break;
    case FieldType::Vec3:
    case FieldType::Color:
        if (v.type != FieldType::Vec3 && v.type != FieldType::Color) return false;
        *(Vec3*)p = v.v;
        break;
    case FieldType::Text:
    case FieldType::Path: {
        if (v.type != FieldType::Path || !v.s) return false;
        size_t n = strlen(v.s);
        if (n > (size_t)kPathMax - 1) n = (size_t)kPathMax - 1;
        char tmp[kPathMax];
        memcpy(tmp, v.s, n);      // v.s may point into the component itself
        memset(p, 0, kPathMax);
        memcpy(p, tmp, n);
        break;
    }
    }
    modified(e, k);
    return true;
}

// ---- links

void World::bind_link(uint32_t link, Entity e) {
    if (impl_ && link) impl_->link_put(link, e);
}
Entity World::link_entity(uint32_t link) const {
    if (!impl_) return 0;
    Entity e = impl_->link_get(link);
    return e && alive(e) ? e : 0;
}
uint32_t World::link_of(Entity e) const {
    if (!impl_ || !e) return 0;
    for (uint32_t i = 0; i < impl_->link_cap; i++)
        if (impl_->links[i].id && impl_->links[i].e == e) return impl_->links[i].id;
    return 0;
}

// ---- current

bool World::set_current(Entity e, ModuleKind k) {
    if ((int)k >= kModuleKindCount || !module_info(k).has_current) return false;
    void* d = get_mut(e, k);
    if (!d) return false;
    set_current_flag(module_info(k), d, true);
    modified(e, k);
    return true;
}

void World::clear_current(Entity e, ModuleKind k) {
    if ((int)k >= kModuleKindCount || !module_info(k).has_current) return;
    void* d = get_mut(e, k);
    if (!d) return;
    set_current_flag(module_info(k), d, false);
    modified(e, k);
}

Entity World::current(ModuleKind k) const {
    if ((int)k >= kModuleKindCount || !impl_ || !impl_->cur_query[(int)k]) return 0;
    const ModuleInfo& mi = module_info(k);
    ecs_iter_t it = ecs_query_iter(impl_->cur, impl_->cur_query[(int)k]);
    while (ecs_query_next(&it)) {
        const char* d = (const char*)ecs_field_w_size(&it, mi.size, 0);
        for (int32_t i = 0; i < it.count; i++) {
            if (get_current(mi, d + (size_t)i * mi.size)) {
                Entity r = it.entities[i];
                ecs_iter_fini(&it);
                return r;
            }
        }
    }
    return 0;
}

// ---- frame

void World::progress(float dt) { ecs_progress(impl_->w, dt); }

Entity World::add_system(Phase phase, FrameFn fn, void* user) {
    if (!fn) return 0;
    SysCtx* c = new SysCtx{ this, impl_, fn, user };
    ecs_system_desc_t sd = {};
    sd.phase = phase_id(phase);
    sd.run = sys_run;
    sd.ctx = c;
    sd.ctx_free = sys_ctx_free;
    ecs_entity_t s = ecs_system_init(impl_->cur, &sd);
    if (!s) delete c;
    return s;
}

void World::remove_system(Entity system) {
    if (alive(system)) ecs_delete(impl_->cur, system);
}

}  // namespace fe
