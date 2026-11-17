#include "fx/fx.h"
#include "script/runtime.h"

namespace fe {

bool FxWorld::init(World* world, const FxHost& host) {
    shutdown();
    if (!world || !world->valid()) return false;
    world_ = world;
    host_ = host;
    if (!particles.init(world)) { shutdown(); return false; }
    return true;
}

void FxWorld::shutdown() {
    particles.shutdown();
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
    rt->add_module_method_ex(ModuleKind::ParticleEmitter, "emit", [](Entity e, const Call& c, Res* r, void* u) {
        int n = c.argc > 0 ? (int)c.num[0] : 1;
        r->b = ((FxWorld*)u)->particles.emit(e, n < 0 ? 0 : n);
    }, this);
}

}  // namespace fe
