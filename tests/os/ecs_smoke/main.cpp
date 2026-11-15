// ecs_smoke: the entity system (flecs + engine/ecs) on Minimal-OS.
// Runs the shared checks of tests/common/ecs_checks.h plus a small
// performance run and prints "FE ECS PASS" or "FE ECS FAIL <what>".
#include "ecs/ecs.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_checks = 0, g_failures = 0;
static char g_first[256];

#define ECS_CHECK(cond) \
    do { g_checks++; if (!(cond)) { g_failures++; \
        printf("FE ECS check failed: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        if (!g_first[0]) snprintf(g_first, sizeof g_first, "%s:%d: %s", __FILE__, __LINE__, #cond); } } while (0)

#include "../../common/ecs_checks.h"

static double now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main() {
    fe::World w;
    if (!w.init()) {
        printf("FE ECS FAIL init\n");
        return 1;
    }
    using namespace fe_ecs_checks;
    check_entities(w);
    check_hierarchy(w);
    check_modules(w);
    check_fields(w);
    check_current(w);
    check_frame(w);

    // 2000 entities in 20 groups of 100
    fe::Entity root = w.create("perf");
    double t0 = now_ms();
    for (int g = 0; g < 20; g++) {
        fe::Entity p = w.create(nullptr, root);
        w.transform(p)->position = fe::Vec3((float)g, 0, 0);
        for (int i = 0; i < 99; i++) {
            fe::Entity c = w.create(nullptr, p);
            w.transform(c)->position = fe::Vec3(0, (float)i, 0);
        }
    }
    double t1 = now_ms();
    for (int f = 0; f < 10; f++) w.progress(0.016f);
    double t2 = now_ms();
    printf("ecs: 2000 entities created in %.1f ms, progress %.2f ms/frame\n", t1 - t0, (t2 - t1) / 10);
    ECS_CHECK(w.child_count(root) == 20);

    w.shutdown();
    printf("ecs: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        printf("FE ECS FAIL %s\n", g_first);
        return 1;
    }
    printf("FE ECS PASS\n");
    return 0;
}
