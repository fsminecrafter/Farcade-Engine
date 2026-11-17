#include "fx/fx.h"
#include "script/runtime.h"
#include "physics/physics.h"

namespace fe {

bool FxWorld::init(World* world, const FxHost& host) {
    shutdown();
    if (!world || !world->valid()) return false;
    world_ = world;
    host_ = host;
    if (!particles.init(world) || !spawner.init(world, &host_) || !animator.init(world, &host_)) { shutdown(); return false; }
    return true;
}

void FxWorld::shutdown() {
    particles.shutdown();
    spawner.shutdown();
    animator.shutdown();
    world_ = nullptr;
}

void FxWorld::bind_script(ScriptRuntime* rt) {
    if (!rt) return;
    typedef ScriptRuntime::MethodCall Call;
    typedef ScriptRuntime::MethodResult Res;
    // obj.particles:play() / stop() / clear() / emit(n)
    rt->add_module_method_ex(ModuleKind::ParticleEmitter, "play", [](Entity e, const Call&, Res* r, void* u) { r->b = ((FxWorld*)u)->particles.play(e); }, this);
    rt->add_module_method_ex(ModuleKind::ParticleEmitter, "stop", [](Entity e, const Call&, Res* r, void* u) { r->b = ((FxWorld*)u)->particles.stop(e); }, this);
    rt->add_module_method_ex(ModuleKind::ParticleEmitter, "clear", [](Entity e, const Call&, Res* r, void* u) { r->b = ((FxWorld*)u)->particles.clear(e); }, this);
    // obj.animator:play(name, fade) / stop()
    rt->add_module_method_ex(ModuleKind::Animator, "play", [](Entity e, const Call& c, Res* r, void* u) {
        r->b = ((FxWorld*)u)->animator.play(e, c.argc > 0 ? c.str[0] : nullptr, c.argc > 1 ? (float)c.num[1] : -1.0f);
    }, this);
    rt->add_module_method_ex(ModuleKind::Animator, "stop", [](Entity e, const Call&, Res* r, void* u) { r->b = ((FxWorld*)u)->animator.stop(e); }, this);
    // obj.spawner:spawn() / clear()
    rt->add_module_method_ex(ModuleKind::Spawner, "spawn", [](Entity e, const Call&, Res* r, void* u) { r->b = ((FxWorld*)u)->spawner.spawn(e); }, this);
    rt->add_module_method_ex(ModuleKind::Spawner, "clear", [](Entity e, const Call&, Res* r, void* u) { r->b = ((FxWorld*)u)->spawner.clear(e); }, this);
    // obj.joint3d:snap() (break is a Lua keyword; break_joint is the same)
    auto snap = [](Entity e, const Call&, Res* r, void* u) {
        FxWorld* f = (FxWorld*)u;
        r->b = f->host_.physics ? f->host_.physics->break_joint(e) : false;
    };
    rt->add_module_method_ex(ModuleKind::Joint3D, "snap", snap, this);
    rt->add_module_method_ex(ModuleKind::Joint3D, "break_joint", snap, this);
    rt->add_module_method_ex(ModuleKind::ParticleEmitter, "emit", [](Entity e, const Call& c, Res* r, void* u) {
        int n = c.argc > 0 ? (int)c.num[0] : 1;
        r->b = ((FxWorld*)u)->particles.emit(e, n < 0 ? 0 : n);
    }, this);
}

}  // namespace fe
