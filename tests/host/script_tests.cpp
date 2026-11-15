// Tests of the Lua script runtime (engine/script): the checks shared with the
// Minimal-OS smoke program (tests/common/script_checks.h) and a performance run.
#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_failures = 0;
#define SCRIPT_CHECK(cond) \
    do { g_checks++; if (!(cond)) { g_failures++; \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#include "../common/script_checks.h"

using namespace fe_script_checks;

template <class F>
static void run(const char* name, F f) {
    int before = g_failures;
    f();
    printf("  %-14s %s\n", name, g_failures == before ? "ok" : "FAILED");
}

int main() {
    run("load errors", check_load_errors);
    run("inputs", check_inputs);
    run("lifecycle", check_lifecycle);
    run("vec3", check_vec3);
    run("transforms", check_transforms);
    run("modules", check_modules);
    run("cross script", check_cross_script);
    run("events", check_events);
    run("find", check_find);
    run("physics events", check_physics_events);
    run("errors", check_errors);
    run("sandbox", check_sandbox);
    run("destroy", check_destroy);
    double ms = 0;
    run("2000 instances", [&] { check_many(&ms, 2000); });
    printf("script: 2000 instances on_update: %.3f ms/frame\n", ms);
    printf("script: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
