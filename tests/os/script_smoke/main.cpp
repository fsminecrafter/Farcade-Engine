// script_smoke: the Lua script runtime on Minimal-OS. Runs the shared checks
// of tests/common/script_checks.h (they exercise Lua's setjmp/longjmp error
// path, number formatting, the ECS bindings) and prints "FE SCRIPT PASS" or
// "FE SCRIPT FAIL <what>".
#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_failures = 0;
static char g_first[256];

#define SCRIPT_CHECK(cond) \
    do { g_checks++; if (!(cond)) { g_failures++; \
        printf("FE SCRIPT check failed: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        if (!g_first[0]) snprintf(g_first, sizeof g_first, "%s:%d: %s", __FILE__, __LINE__, #cond); } } while (0)

#include "../../common/script_checks.h"

using namespace fe_script_checks;

int main() {
    check_load_errors();
    check_inputs();
    check_lifecycle();
    check_vec3();
    check_transforms();
    check_modules();
    check_cross_script();
    check_events();
    check_coroutines_and_limits();
    check_find();
    check_physics_events();
    check_errors();
    check_sandbox();
    check_destroy();
    double ms = 0;
    check_many(&ms, 500);
    printf("script: 500 instances on_update: %.3f ms/frame\n", ms);
    printf("script: %d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        printf("FE SCRIPT FAIL %s\n", g_first);
        return 1;
    }
    printf("FE SCRIPT PASS\n");
    return 0;
}
