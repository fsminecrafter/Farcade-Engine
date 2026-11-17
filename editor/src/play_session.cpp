#include "play_session.h"
#include "ecs/ecs.h"
#include "physics/physics.h"
#include "script/runtime.h"
#include "audio/audio.h"
#include "fx/fx.h"
#include "platform/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

void PlaySession::log(PlayLog level, const char* fmt, ...) {
    if (!host_.log) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    host_.log(level, buf, host_.user);
}

bool PlaySession::start(Scene& scene, const PlayHost& host, AudioDevice* audio_device) {
    stop();
    scene_ = &scene;
    host_ = host;
    world_ = new World();
    if (!world_->init()) {
        log(PlayLog::Error, "Play: the ECS world could not start");
        stop();
        return false;
    }
    int n = scene.count();
    objects_ = (ObjectId*)malloc(sizeof(ObjectId) * (n ? n : 1));
    entities_ = (uint64_t*)malloc(sizeof(uint64_t) * (n ? n : 1));
    count_ = 0;
    // All at the root first (a child can come before its parent in the
    // list), then the parents.
    for (int i = 0; i < n; i++) {
        Object& o = scene.at(i);
        Entity e = world_->create(o.name);
        if (!e) e = world_->create(nullptr);    // a name taken by a sibling, or reserved
        if (!e) continue;
        Transform* t = world_->transform(e);
        t->position = o.position;
        t->rotation = o.rotation;
        t->scale = o.scale;
        world_->load(e, o.modules);
        if (o.kind == ObjectKind::Mesh) {
            Renderable* r = world_->add_renderable(e);
            r->mesh = (uint8_t)o.mesh;
            memcpy(r->model_path, o.model_path, sizeof(r->model_path));
            memcpy(r->texture_path, o.texture_path, sizeof(r->texture_path));
            memcpy(r->shader_path, o.shader_path, sizeof(r->shader_path));
            r->color = o.color;
            r->visible = o.visible;
            r->roughness = o.roughness;
            r->metallic = o.metallic;
            r->unlit = o.unlit;
            r->cast_shadows = o.cast_shadows;
        }
        world_->bind_link(o.id, e);             // object links in modules (Joint 3D, Spawner) find their entity
        objects_[count_] = o.id;
        entities_[count_] = e;
        count_++;
    }
    for (int i = 0; i < count_; i++) {
        Object* o = scene.get(objects_[i]);
        if (o && o->parent) {
            Entity pe = entity_of(o->parent);
            if (pe && !world_->set_parent(entities_[i], pe)) {
                // A sibling has the same name: flecs names are unique per parent.
                world_->set_name(entities_[i], nullptr);
                world_->set_parent(entities_[i], pe);
            }
        }
    }
    // The current flags as the scene has them.
    for (int k = 0; k < kModuleKindCount; k++) {
        ObjectId c = scene.current((ModuleKind)k);
        if (Entity e = entity_of(c)) world_->set_current(e, (ModuleKind)k);
    }
    world_->update_transforms();
    // Scripts (Lua): one instance per script on each object, in Inspector
    // order; object inputs become the linked objects' entities.
    scripts_ = new ScriptRuntime();
    ScriptHost sh;
    sh.user = this;
    sh.log = [](ScriptLogLevel level, const char* text, void* u) {
        PlaySession* self = (PlaySession*)u;
        if (self->host_.log)
            self->host_.log(level == ScriptLogLevel::Error ? PlayLog::Error : (level == ScriptLogLevel::Warning ? PlayLog::Warning : PlayLog::Info),
                            text, self->host_.user);
    };
    script_count_ = 0;
    if (scripts_->init(world_, sh)) {
        // Before any instance: on_start may already call fe.graphics.
        if (host_.graphics) scripts_->set_graphics_settings(host_.graphics, host_.graphics_changed);
        for (int i = 0; i < count_; i++) {
            Object* o = scene.get(objects_[i]);
            if (!o) continue;
            for (int s = 0; s < o->script_count; s++) {
                ScriptSlot& sl = o->scripts[s];
                size_t size = 0;
                char* text = (char*)host_.read_file(sl.path, &size, host_.user);
                if (!text) { log(PlayLog::Error, "%s: cannot read %s", o->name, sl.path); continue; }
                Entity links[kMaxProps] = {};
                for (int k = 0; k < sl.prop_count; k++)
                    if (sl.props[k].type == PropType::Object) links[k] = entity_of(sl.props[k].object);
                if (scripts_->add_instance(entities_[i], sl.path, text, (uint32_t)size, sl.props, sl.prop_count, links)) script_count_++;
                free(text);
            }
        }
    } else {
        log(PlayLog::Error, "Play: the Lua runtime could not start");
    }
    // Audio: Audio Streams with "play on start" begin now; samplers play
    // when scripts call obj.audio_sampler:play().
    if (audio_device) {
        audio_ = new AudioSystem();
        AudioHost ah;
        ah.user = this;
        ah.read_file = [](const char* path, size_t* size, void* u) -> void* {
            PlaySession* self = (PlaySession*)u;
            return self->host_.read_file(path, size, self->host_.user);
        };
        ah.log = [](const char* line, void* u) { ((PlaySession*)u)->log(PlayLog::Warning, "Audio: %s", line); };
        if (audio_->init(world_, audio_device, ah)) {
            audio_->restart();
            audio_->clear_cache();      // the files may have changed since the last Play
            if (scripts_) {
                ScriptRuntime* rt = scripts_;
                rt->add_module_method(ModuleKind::AudioSampler, "play", [](Entity e, void* u) { return (int)((AudioSystem*)u)->play_sampler(e); }, audio_);
                rt->add_module_method(ModuleKind::AudioStream, "play", [](Entity e, void* u) { return (int)((AudioSystem*)u)->play_stream(e); }, audio_);
                rt->add_module_method(ModuleKind::AudioStream, "stop", [](Entity e, void* u) { ((AudioSystem*)u)->stop_stream(e); return 1; }, audio_);
                rt->add_module_method(ModuleKind::AudioStream, "pause", [](Entity e, void* u) { ((AudioSystem*)u)->pause_stream(e, true); return 1; }, audio_);
                rt->add_module_method(ModuleKind::AudioStream, "resume", [](Entity e, void* u) { ((AudioSystem*)u)->pause_stream(e, false); return 1; }, audio_);
                rt->add_module_method(ModuleKind::AudioStream, "is_playing", [](Entity e, void* u) { return (int)((AudioSystem*)u)->is_playing(e); }, audio_);
                rt->add_module_method(ModuleKind::AudioSampler, "is_playing", [](Entity e, void* u) { return (int)((AudioSystem*)u)->is_playing(e); }, audio_);
            }
        } else {
            log(PlayLog::Error, "Play: the audio system could not start");
        }
    }
    // Physics on the bodies and colliders (the built-in step, see physics.h).
    physics_ = new PhysicsWorld();
    PhysicsEvents pe;
    pe.user = this;
    pe.trigger_enter = [](Entity t, Entity o, void* u) { ((PlaySession*)u)->physics_event("entered", t, o); };
    pe.trigger_exit = [](Entity t, Entity o, void* u) { ((PlaySession*)u)->physics_event("left", t, o); };
    pe.collision = [](Entity a, Entity b, void* u) { ((PlaySession*)u)->physics_event("hit", a, b); };
    physics_->init(world_, pe);
    for (int i = 0; i < count_; i++) {
        physics_->add(entities_[i]);
        // A Mesh collider is the convex hull of its object's mesh.
        Object* o = scene.get(objects_[i]);
        if (o && o->kind == ObjectKind::Mesh && o->modules.has(ModuleKind::Collider3D) && o->modules.collider3d.shape == ColliderShape::Mesh) {
            const MeshData& md = Scene::mesh_for(*o);
            physics_->set_mesh(entities_[i], md.positions, (int)md.vertex_count);
        }
    }
    // Effects: particles, force fields, spawners, animation, billboards, text (engine/fx).
    fx_ = new FxWorld();
    FxHost fh;
    fh.user = this;
    fh.physics = physics_;
    fh.spawn = [](void* u, Entity src, const Mat4& m) -> Entity { return ((PlaySession*)u)->spawn_copy(src, m); };
    fh.read_file = [](const char* path, size_t* size, void* u) -> void* {
        PlaySession* self = (PlaySession*)u;
        return self->host_.read_file(path, size, self->host_.user);
    };
    fh.log = [](void* u, const char* what, const char* path) { ((PlaySession*)u)->log(PlayLog::Warning, "%s: %s", path, what); };
    if (fx_->init(world_, fh)) {
        fx_->quality = quality_;
        if (scripts_) fx_->bind_script(scripts_);
    } else {
        delete fx_;
        fx_ = nullptr;
        log(PlayLog::Error, "Play: the effects systems could not start");
    }
    return true;
}

void PlaySession::set_quality(const FxQuality& q) {
    quality_ = q;
    if (fx_) fx_->quality = q;
    if (audio_) audio_->reverb_enabled = q.reverb;
}

uint64_t PlaySession::spawn_copy(uint64_t source, const Mat4& world_matrix) {
    ObjectId src = object_of(source);
    if (!world_ || !src || !scene_->get(src)) return 0;
    uint64_t e = make_copy(src, 0, kNoObject, &world_matrix, 0);
    if (e) world_->update_transforms();
    return e;
}

uint64_t PlaySession::make_copy(ObjectId from, uint64_t parent_e, ObjectId parent_o, const Mat4* root, int depth) {
    Scene& sc = *scene_;
    Object* so = sc.get(from);
    if (!so || depth > 8) return 0;
    ObjectId id = sc.create(so->name, so->mesh);
    Object* o = sc.get(id);
    so = sc.get(from);
    char name[64];
    memcpy(name, o->name, sizeof name);
    *o = *so;
    o->id = id;
    memcpy(o->name, name, sizeof name);
    o->parent = parent_o;
    for (int k = 0; k < kModuleKindCount; k++)
        if (o->modules.has((ModuleKind)k)) set_current_flag(module_info((ModuleKind)k), o->modules.data((ModuleKind)k), false);
    if (root) decompose(*root, &o->position, &o->rotation, &o->scale);
    Entity e = world_->create(nullptr);
    if (!e) { sc.remove(id); return 0; }
    world_->bind_link(id, e);
    Transform* t = world_->transform(e);
    t->position = o->position; t->rotation = o->rotation; t->scale = o->scale;
    world_->load(e, o->modules);
    if (o->kind == ObjectKind::Mesh) {
        Renderable* r = world_->add_renderable(e);
        r->mesh = (uint8_t)o->mesh;
        memcpy(r->model_path, o->model_path, sizeof r->model_path);
        memcpy(r->texture_path, o->texture_path, sizeof r->texture_path);
        memcpy(r->shader_path, o->shader_path, sizeof r->shader_path);
        r->color = o->color; r->visible = o->visible; r->roughness = o->roughness; r->metallic = o->metallic;
        r->unlit = o->unlit; r->cast_shadows = o->cast_shadows;
    }
    objects_ = (ObjectId*)realloc(objects_, sizeof(ObjectId) * (size_t)(count_ + 1));
    entities_ = (uint64_t*)realloc(entities_, sizeof(uint64_t) * (size_t)(count_ + 1));
    objects_[count_] = id;
    entities_[count_] = e;
    count_++;
    if (parent_e) world_->set_parent(e, parent_e);
    if (physics_) {
        physics_->add(e);
        if (o->kind == ObjectKind::Mesh && o->modules.has(ModuleKind::Collider3D) && o->modules.collider3d.shape == ColliderShape::Mesh) {
            const MeshData& md = Scene::mesh_for(*o);
            physics_->set_mesh(e, md.positions, (int)md.vertex_count);
        }
    }
    if (scripts_) {
        for (int s = 0; s < o->script_count; s++) {
            ScriptSlot& sl = o->scripts[s];
            size_t size = 0;
            char* text = (char*)host_.read_file(sl.path, &size, host_.user);
            if (!text) continue;
            Entity links[kMaxProps] = {};
            for (int k = 0; k < sl.prop_count; k++)
                if (sl.props[k].type == PropType::Object) links[k] = entity_of(sl.props[k].object);
            scripts_->add_instance(e, sl.path, text, (uint32_t)size, sl.props, sl.prop_count, links);
            free(text);
        }
    }
    ObjectId kids[64];
    int nk = 0;
    for (int i = 0; i < sc.count() && nk < 64; i++) if (sc.at(i).parent == from) kids[nk++] = sc.at(i).id;
    for (int k = 0; k < nk; k++) make_copy(kids[k], e, id, nullptr, depth + 1);
    return e;
}

void PlaySession::physics_event(const char* what, uint64_t a, uint64_t b) {
    // Scripts get them as on_trigger_enter / on_trigger_exit / on_collision.
    if (scripts_) {
        if (what[0] == 'e') scripts_->trigger_enter(a, b);
        else if (what[0] == 'l') scripts_->trigger_exit(a, b);
        else scripts_->collision(a, b);
    }
    if (host_.physics_event) host_.physics_event(what, a, b, host_.user);
}

ObjectId PlaySession::object_of(uint64_t e) const {
    for (int i = 0; i < count_; i++)
        if (entities_[i] == e) return objects_[i];
    return kNoObject;
}

uint64_t PlaySession::entity_of(ObjectId id) const {
    if (!id) return 0;
    for (int i = 0; i < count_; i++)
        if (objects_[i] == id) return entities_[i];
    return 0;
}

void PlaySession::step(float dt) {
    if (!world_) return;
    world_->progress(dt);
    if (fx_) fx_->post_frame(dt);                       // spawners (they make and destroy entities)
    if (audio_) audio_->reverb_enabled = quality_.reverb;
    // Objects whose entity was destroyed (a script's destroy(), a spawner's lifetime) leave the scene.
    for (int i = 0; i < count_;) {
        if (world_->alive(entities_[i])) { i++; continue; }
        ObjectId dead = objects_[i];
        objects_[i] = objects_[count_ - 1];
        entities_[i] = entities_[count_ - 1];
        count_--;
        scene_->remove(dead);
    }
    // Transforms and current flags back into the scene, for the viewport
    // and the Inspector.
    for (int i = 0; i < count_; i++) {
        Object* o = scene_->get(objects_[i]);
        Entity e = entities_[i];
        if (!o || !world_->alive(e)) continue;
        if (const Transform* t = world_->transform(e)) {
            o->position = t->position;
            o->rotation = t->rotation;
            o->scale = t->scale;
        }
        // The look: scripts change it through obj.material.
        if (const Renderable* r = world_->renderable(e)) {
            o->color = r->color;
            o->visible = r->visible;
            o->roughness = r->roughness;
            o->metallic = r->metallic;
            o->unlit = r->unlit;
            o->cast_shadows = r->cast_shadows;
            memcpy(o->texture_path, r->texture_path, sizeof(o->texture_path));
            memcpy(o->shader_path, r->shader_path, sizeof(o->shader_path));
        }
        // Modules too: scripts and physics change them (a light's
        // intensity, a body's velocity, which camera is current).
        world_->store(e, &o->modules);
    }
}

void PlaySession::stop() {
    if (fx_) { fx_->shutdown(); delete fx_; }
    fx_ = nullptr;
    if (audio_) { audio_->shutdown(); delete audio_; }
    audio_ = nullptr;
    if (scripts_) { scripts_->shutdown(); delete scripts_; }
    scripts_ = nullptr;
    if (physics_) { physics_->shutdown(); delete physics_; }
    physics_ = nullptr;
    if (world_) { world_->shutdown(); delete world_; }
    world_ = nullptr;
    free(objects_);
    free(entities_);
    objects_ = nullptr;
    entities_ = nullptr;
    count_ = 0;
    script_count_ = 0;
}

// ---- input

namespace {

// The name scripts use for a platform key ("a", "space", "f5" ...), or null.
const char* key_name(uint16_t k, char* buf) {
    using namespace platform;
    if (k >= KEY_A && k < KEY_A + 26) { buf[0] = (char)('a' + (k - KEY_A)); buf[1] = 0; return buf; }
    if (k >= KEY_0 && k < KEY_0 + 10) { buf[0] = (char)('0' + (k - KEY_0)); buf[1] = 0; return buf; }
    if (k >= KEY_F1 && k <= KEY_F12) { snprintf(buf, 8, "f%d", (int)(k - KEY_F1) + 1); return buf; }
    switch (k) {
        case KEY_SPACE: return "space";
        case KEY_ENTER: return "enter";
        case KEY_ESCAPE: return "escape";
        case KEY_TAB: return "tab";
        case KEY_BACKSPACE: return "backspace";
        case KEY_LEFT: return "left";
        case KEY_RIGHT: return "right";
        case KEY_UP: return "up";
        case KEY_DOWN: return "down";
        case KEY_PAGEUP: return "pageup";
        case KEY_PAGEDOWN: return "pagedown";
        case KEY_HOME: return "home";
        case KEY_END: return "end";
        case KEY_INSERT: return "insert";
        case KEY_DELETE: return "delete";
        case KEY_LSHIFT: case KEY_RSHIFT: return "shift";
        case KEY_LCTRL: case KEY_RCTRL: return "ctrl";
        case KEY_LALT: case KEY_RALT: return "alt";
        default: return nullptr;
    }
}

}  // namespace

bool PlaySession::forward_event(const platform::Event& e) {
    if (!scripts_) return false;
    using platform::EventType;
    switch (e.type) {
        case EventType::Key: {
            char buf[8];
            const char* n = key_name(e.key, buf);
            if (!n) return false;
            scripts_->key_event(n, e.down);
            return true;
        }
        case EventType::MouseMove: scripts_->mouse_move(e.x, e.y); return true;
        case EventType::MouseButton: scripts_->mouse_move(e.x, e.y); scripts_->mouse_button(e.button, e.down); return true;
        case EventType::MouseWheel: scripts_->mouse_wheel(e.wheel_y); return true;
        default: return false;
    }
}

}  // namespace fe
