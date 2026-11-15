// Tests of the entity system (engine/ecs): the checks shared with the
// Minimal-OS smoke program, flecs meta registration, and a performance run.
#include "flecs.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_checks = 0, g_failures = 0;
#define ECS_CHECK(cond) \
    do { g_checks++; if (!(cond)) { g_failures++; \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#include "../common/ecs_checks.h"

using namespace fe;
using namespace fe_ecs_checks;

static double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// A fresh world per group of checks.
template <class F>
static void run(const char* name, F f) {
    int before = g_failures;
    World w;
    ECS_CHECK(w.init());
    if (w.valid()) f(w);
    w.shutdown();
    printf("  %-12s %s\n", name, g_failures == before ? "ok" : "FAILED");
}

static void test_meta(World& w) {
    ecs_world_t* ew = w.raw();
    for (int k = 0; k < kModuleKindCount; k++) {
        const ModuleInfo& mi = module_info((ModuleKind)k);
        ecs_entity_t c = w.component((ModuleKind)k);
        ECS_CHECK(c != 0);
        const EcsComponent* ci = ecs_get(ew, c, EcsComponent);
        ECS_CHECK(ci && (uint32_t)ci->size == mi.size && (uint32_t)ci->alignment == mi.align);
        const EcsStruct* st = ecs_get(ew, c, EcsStruct);
        ECS_CHECK(st != nullptr);
        if (!st) continue;
        ECS_CHECK(ecs_vec_count(&st->members) == mi.field_count);
        const ecs_member_t* m = (const ecs_member_t*)ecs_vec_first(&st->members);
        for (int i = 0; i < mi.field_count && i < ecs_vec_count(&st->members); i++) {
            ECS_CHECK(strcmp(m[i].name, mi.fields[i].name) == 0);
            ECS_CHECK(m[i].offset == mi.fields[i].offset);
            const FieldInfo& f = mi.fields[i];
            switch (f.type) {
            case FieldType::Float: ECS_CHECK(m[i].type == ecs_id(ecs_f32_t)); break;
            case FieldType::Int: ECS_CHECK(m[i].type == ecs_id(ecs_i32_t)); break;
            case FieldType::Bool: ECS_CHECK(m[i].type == ecs_id(ecs_bool_t)); break;
            case FieldType::Bits: ECS_CHECK(m[i].type == ecs_id(ecs_u32_t)); break;
            case FieldType::Vec3:
            case FieldType::Color: {
                const EcsStruct* vs = ecs_get(ew, m[i].type, EcsStruct);
                ECS_CHECK(vs && ecs_vec_count(&vs->members) == 3);
                break;
            }
            case FieldType::Path:
                ECS_CHECK(m[i].type == ecs_id(ecs_char_t) && m[i].count == kPathMax);
                break;
            case FieldType::Enum: {
                const EcsEnum* en = ecs_get(ew, m[i].type, EcsEnum);
                ECS_CHECK(en != nullptr);
                int n = 0;
                while (f.enum_names[n]) n++;
                // one constant per name, named like it
                for (int c2 = 0; c2 < n; c2++) {
                    ecs_entity_t ce = ecs_lookup_child(ew, m[i].type, f.enum_names[c2]);
                    ECS_CHECK(ce != 0);
                }
                ECS_CHECK(ecs_get(ew, m[i].type, EcsComponent)->size == 1);
                break;
            }
            }
        }
    }
    // Light: the member count, an enum's constants and the cursor API walking the data
    const EcsStruct* ls = ecs_get(ew, w.component(ModuleKind::Light), EcsStruct);
    ECS_CHECK(ls && ecs_vec_count(&ls->members) == module_info(ModuleKind::Light).field_count);
    ECS_CHECK(ecs_lookup_child(ew, ecs_lookup(ew, "fe.enums.light_type"), "Spot") != 0);

    // Transform, WorldTransform, Renderable
    const EcsStruct* ts = ecs_get(ew, w.transform_component(), EcsStruct);
    ECS_CHECK(ts && ecs_vec_count(&ts->members) == 3);
    const EcsStruct* ws = ecs_get(ew, w.world_transform_component(), EcsStruct);
    ECS_CHECK(ws && ecs_vec_count(&ws->members) == 1);
    const EcsStruct* rs = ecs_get(ew, w.renderable_component(), EcsStruct);
    ECS_CHECK(rs && ecs_vec_count(&rs->members) == 8);

    // meta cursor reads the same values as the struct
    Entity e = w.create("meta");
    w.add(e, ModuleKind::Light);
    w.set_field(e, ModuleKind::Light, "intensity", FieldValue::from_float(6.5f));
    w.set_field(e, ModuleKind::Light, "color", FieldValue::from_color(Vec3(0.25f, 0.5f, 0.75f)));
    void* ld = w.get_mut(e, ModuleKind::Light);
    ecs_meta_cursor_t cur = ecs_meta_cursor(ew, w.component(ModuleKind::Light), ld);
    ecs_meta_push(&cur);
    ECS_CHECK(strcmp(ecs_meta_get_member(&cur), "type") == 0);
    ECS_CHECK(ecs_meta_get_int(&cur) == (int)LightType::Point);
    ecs_meta_next(&cur);
    ECS_CHECK(strcmp(ecs_meta_get_member(&cur), "color") == 0);
    ecs_meta_push(&cur);
    ecs_meta_next(&cur);
    ECS_CHECK(near_((float)ecs_meta_get_float(&cur), 0.5f));
    ecs_meta_pop(&cur);
    ecs_meta_next(&cur);
    ECS_CHECK(strcmp(ecs_meta_get_member(&cur), "intensity") == 0);
    ECS_CHECK(near_((float)ecs_meta_get_float(&cur), 6.5f));

    // meta can write an enum by name
    ecs_meta_cursor_t c2 = ecs_meta_cursor(ew, w.component(ModuleKind::Light), ld);
    ecs_meta_push(&c2);
    ecs_meta_set_string(&c2, "Spot");
    ECS_CHECK(((const Light*)ld)->type == LightType::Spot);
}

static void test_perf(World& w) {
    const int kGroups = 100, kPer = 100;      // 10k entities, two levels
    double t0 = now_ms();
    Entity root = w.create("root");
    for (int g = 0; g < kGroups; g++) {
        Entity p = w.create(nullptr, root);
        w.transform(p)->position = Vec3((float)g, 0, 0);
        for (int i = 0; i < kPer - 1; i++) {
            Entity c = w.create(nullptr, p);
            w.transform(c)->position = Vec3(0, (float)i, 0);
            w.transform(c)->rotation = Quat::axis_angle(Vec3(0, 1, 0), 0.01f * (float)i);
        }
    }
    double t1 = now_ms();
    w.progress(0.016f);                         // first frame builds the query tables
    double t2 = now_ms();
    const int kFrames = 20;
    for (int f = 0; f < kFrames; f++) w.progress(0.016f);
    double t3 = now_ms();
    double per_frame = (t3 - t2) / kFrames;
    printf("  perf: %d entities (hierarchy %dx%d) created in %.1f ms, first frame %.2f ms, steady %.3f ms/frame\n",
           kGroups * kPer + 1, kGroups, kPer, t1 - t0, t2 - t1, per_frame);
    ECS_CHECK(per_frame < 100.0);
    ECS_CHECK(t1 - t0 < 2000.0);
    ECS_CHECK(w.child_count(root) == kGroups);

    // flat: 10k roots
    Entity first = 0, last = 0;
    double f0 = now_ms();
    for (int i = 0; i < 10000; i++) {
        Entity e = w.create();
        w.transform(e)->position = Vec3((float)i, 1, 2);
        if (!first) first = e;
        last = e;
    }
    double f1 = now_ms();
    w.progress(0.016f);
    double f2 = now_ms();
    for (int f = 0; f < kFrames; f++) w.progress(0.016f);
    double f3 = now_ms();
    printf("  perf: +10000 flat entities created in %.1f ms, steady %.3f ms/frame (all %d)\n",
           f1 - f0, (f3 - f2) / kFrames, kGroups * kPer + 1 + 10000);
    ECS_CHECK((f3 - f2) / kFrames < 100.0);
    ECS_CHECK_VEC(translation(w, last), 9999, 1, 2);
    (void)first;

    // destroying the whole tree
    double d0 = now_ms();
    w.destroy(root);
    double d1 = now_ms();
    printf("  perf: destroy of %d entities with children %.1f ms\n", kGroups * kPer + 1, d1 - d0);
    ECS_CHECK(!w.alive(root));
}

int main() {
    printf("ecs_tests\n");
    run("entities", [](World& w) { check_entities(w); });
    run("hierarchy", [](World& w) { check_hierarchy(w); });
    run("modules", [](World& w) { check_modules(w); });
    run("fields", [](World& w) { check_fields(w); });
    run("current", [](World& w) { check_current(w); });
    run("frame", [](World& w) { check_frame(w); });
    run("meta", test_meta);
    run("perf", test_perf);

    // two worlds at once, and re-init after shutdown
    {
        World a, b;
        ECS_CHECK(a.init() && b.init());
        Entity ea = a.create("x"), eb = b.create("x");
        ECS_CHECK(ea != 0 && eb != 0 && a.lookup("x") == ea && b.lookup("x") == eb);
        a.add(ea, ModuleKind::Camera);
        b.add(eb, ModuleKind::Camera);
        a.set_current(ea, ModuleKind::Camera);
        b.set_current(eb, ModuleKind::Camera);
        ECS_CHECK(a.current(ModuleKind::Camera) == ea && b.current(ModuleKind::Camera) == eb);
        a.shutdown();
        ECS_CHECK(a.init() && a.lookup("x") == 0);
    }
    printf("ecs_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
