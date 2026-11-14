// imgui_smoke: Dear ImGui (docking) + ImGuizmo + the Minimal-OS runtime.
//
// Runs on Minimal-OS (tools/ci/run_os.sh) and prints "FE SMOKE PASS" or
// "FE SMOKE FAIL: <reason>". Checks: libm and printf/scanf against values
// computed on the PC, FILE I/O, malloc from two threads, an ImGui context
// with the default font and Inter (from 0:/programs/ when staged), five
// frames of the demo window with docking, one ImGuizmo::Manipulate, the
// draw data, and an ini settings round trip in memory and on disk.
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <time.h>
#include <errno.h>

// The SDK's headers next to the runtime's (they must coexist).
#include "minimalos.h"
#include "thread.h"
#include "vm.h"
#include "process.h"
#include "window.h"

static const char* g_fail = nullptr;
static char g_fail_buf[256];

// Every failed check is printed; the first one is the verdict.
static void fail(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("FE SMOKE check failed: %s\n", msg);
    if (!g_fail) {
        snprintf(g_fail_buf, sizeof g_fail_buf, "%s", msg);
        char* nl = strchr(g_fail_buf, '\n');
        if (nl) *nl = 0;
        g_fail = g_fail_buf;
    }
}

static double now_ms() { return (double)mos_clock_monotonic_ns() / 1e6; }

// ---- libm, printf, scanf, strtod ------------------------------------------

static void close_to(const char* what, double got, double want, double rel) {
    double err = fabs(got - want);
    double lim = rel * (fabs(want) > 1e-300 ? fabs(want) : 1.0);
    if (!(err <= lim)) fail("%s = %.17g, expected %.17g", what, got, want);
}

static void test_math() {
    // Expected values from glibc on the PC.
    close_to("sqrt(2)", sqrt(2.0), 1.4142135623730951, 0);
    close_to("sin(1)", sin(1.0), 0.8414709848078965, 3e-16);
    close_to("cos(1)", cos(1.0), 0.54030230586813977, 3e-16);
    close_to("tan(1)", tan(1.0), 1.5574077246549023, 5e-16);
    close_to("sin(1e6)", sin(1e6), -0.34999350217129294, 3e-16);
    close_to("sin(1e300)", sin(1e300), -0.8178819121159085, 3e-16);
    close_to("asin(0.5)", asin(0.5), 0.52359877559829893, 5e-16);
    close_to("acos(0.3)", acos(0.3), 1.2661036727794992, 5e-16);
    close_to("atan(3)", atan(3.0), 1.2490457723982544, 3e-16);
    close_to("atan2(1,-1)", atan2(1.0, -1.0), 2.3561944901923448, 3e-16);
    close_to("exp(1)", exp(1.0), 2.7182818284590451, 3e-16);
    close_to("exp(-700)", exp(-700.0), 9.85967654375977e-305, 3e-16);
    close_to("log(10)", log(10.0), 2.3025850929940459, 3e-16);
    close_to("log2(10)", log2(10.0), 3.3219280948873622, 3e-16);
    close_to("log10(2)", log10(2.0), 0.3010299956639812, 3e-16);
    close_to("log10(1000)", log10(1000.0), 3.0, 0);
    close_to("pow(2,0.5)", pow(2.0, 0.5), 1.4142135623730951, 3e-16);
    close_to("pow(10,22)", pow(10.0, 22.0), 1e22, 0);
    close_to("pow(1.0001,10000)", pow(1.0001, 10000.0), 2.7181459268249255, 3e-16);
    close_to("fmod(10,3)", fmod(10.0, 3.0), 1.0, 0);
    close_to("fmod(-7.5,2)", fmod(-7.5, 2.0), -1.5, 0);
    close_to("floor(-1.5)", floor(-1.5), -2.0, 0);
    close_to("ceil(-1.5)", ceil(-1.5), -1.0, 0);
    close_to("round(2.5)", round(2.5), 3.0, 0);
    close_to("trunc(-2.7)", trunc(-2.7), -2.0, 0);
    close_to("sinf(0.5f)", sinf(0.5f), 0.47942554950714111, 1e-7);
    close_to("powf(3,2.5)", powf(3.0f, 2.5f), 15.588457107543945, 1e-7);
    close_to("sqrtf(3)", sqrtf(3.0f), 1.7320507764816284, 1e-7);
    close_to("atan2f(-1,2)", atan2f(-1.0f, 2.0f), -0.46364760398864746, 1e-7);
    if (!isnan(sqrt(-1.0)) || !isinf(HUGE_VAL) || !isnan(NAN) || isinf(1.0)) fail("isnan/isinf");
    if (copysign(3.0, -0.0) != -3.0 || fmin(1.0, 2.0) != 1.0 || fmaxf(1.0f, 2.0f) != 2.0f) fail("copysign/fmin/fmax");
}

static void test_text() {
    char buf[256];
    int n = snprintf(buf, sizeof buf, "%d|%5.2f|%s|%08x|%lld|%g|%c", -42, 3.14159, "abc", 0xbeefu, -9000000000LL, 1e-5, 'Z');
    const char* want = "-42| 3.14|abc|0000beef|-9000000000|1e-05|Z";
    if (strcmp(buf, want) != 0 || n != (int)strlen(want)) fail("snprintf gave \"%s\"", buf);
    int a = 0, r = 0;
    unsigned u = 0;
    float f = 0;
    double d = 0;
    char s[32] = { 0 };
    r = sscanf("Pos=12,-34 0x0000ABCD 2.5 1e300 word", "Pos=%d,%d 0x%08X %f %lf %31s", &a, &n, &u, &f, &d, s);
    if (r != 6 || a != 12 || n != -34 || u != 0xabcd || f != 2.5f || d != 1e300 || strcmp(s, "word") != 0)
        fail("sscanf returned %d (%d %d %x %g %g %s)", r, a, n, u, f, d, s);
    int consumed = 0;
    r = sscanf("ID=0x00000011 Rest", "ID=0x%08X%n", &u, &consumed);
    if (r != 1 || u != 0x11 || consumed != 13) fail("sscanf %%n: %d %x %d", r, u, consumed);
    if (strtod("0.1", nullptr) != 0.1 || strtod("-1.7976931348623157e308", nullptr) != -1.7976931348623157e308 ||
        strtod("4.9406564584124654e-324", nullptr) != 4.9406564584124654e-324)
        fail("strtod");
    if (strtol("-0x7f", nullptr, 0) != -127 || strtoul("777", nullptr, 8) != 511u || atoi(" 42z") != 42 || atof("2.5e3") != 2500.0)
        fail("strtol/atoi/atof");
    // stb_sprintf is exact to about 16 significant digits (%.17g may be off
    // in the last digit), which covers floats (%.9g) and %.15g doubles.
    snprintf(buf, sizeof buf, "%.9g|%.15g|%.3e", (double)0.1f, 0.1 + 0.2, 6.02214076e23);
    if (strcmp(buf, "0.100000001|0.3|6.022e+23") != 0) fail("%%.9g/%%.15g/%%.3e gave %s", buf);
    int arr[] = { 5, 3, 9, 1, 7, 2, 8, 6, 4, 0 };
    qsort(arr, 10, sizeof(int), [](const void* x, const void* y) { return *(const int*)x - *(const int*)y; });
    for (int i = 0; i < 10; i++) if (arr[i] != i) { fail("qsort"); break; }
    char* dup = strdup("Hello, Minimal-OS");
    if (!dup || strstr(dup, "Minimal") != dup + 7 || strrchr(dup, 'l') != dup + 13 || strchr(dup, ',') != dup + 5) fail("strdup/strstr/strrchr");
    char tok[] = "a,b;;c";
    char* t1 = strtok(tok, ",;"); char* t2 = strtok(nullptr, ",;"); char* t3 = strtok(nullptr, ",;");
    if (!t1 || !t2 || !t3 || strcmp(t3, "c") != 0 || strtok(nullptr, ",;")) fail("strtok");
    free(dup);
    time_t tt = time(nullptr);
    if (tt < 1500000000) printf("FE SMOKE note: time() = %ld (system clock not set?)\n", (long)tt);
}

// ---- files ----------------------------------------------------------------

static void test_files() {
    const char* path = "0:/fe_smoke.txt";
    FILE* f = fopen(path, "w");
    if (!f) { fail("fopen(w) failed, errno %d", errno); return; }
    for (int i = 0; i < 200; i++) fprintf(f, "line %d: %s\n", i, "the quick brown fox");
    fclose(f);
    f = fopen(path, "rb");
    if (!f) { fail("fopen(r) failed"); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char line[128];
    int lines = 0;
    while (fgets(line, sizeof line, f)) {
        char expect[128];
        snprintf(expect, sizeof expect, "line %d: %s\n", lines, "the quick brown fox");
        if (strcmp(line, expect) != 0) { fail("file line %d is \"%s\"", lines, line); break; }
        lines++;
    }
    fclose(f);
    if (lines != 200) fail("read %d lines back, expected 200 (size %ld)", lines, size);
    // "w" replaces, "a" appends
    f = fopen(path, "w"); fputs("short", f); fclose(f);
    f = fopen(path, "a"); fputs("+more", f); fclose(f);
    f = fopen(path, "r");
    size_t got = f ? fread(line, 1, sizeof line - 1, f) : 0;
    line[got] = 0;
    if (f) fclose(f);
    if (strcmp(line, "short+more") != 0) fail("w/a modes gave \"%s\"", line);
    if (remove(path) != 0 || fopen(path, "r")) fail("remove");
}

// ---- malloc from two threads ----------------------------------------------

struct AllocJob { unsigned seed; int errors; long ops; };

static void alloc_worker(void* arg) {
    AllocJob* job = (AllocJob*)arg;
    enum { SLOTS = 512 };
    unsigned char* ptrs[SLOTS] = {};
    size_t sizes[SLOTS] = {};
    unsigned s = job->seed;
    for (long i = 0; i < 60000; i++) {
        s = s * 1103515245u + 12345u;
        unsigned slot = (s >> 8) % SLOTS;
        if (ptrs[slot]) {
            unsigned char v = (unsigned char)(slot ^ job->seed);
            if (ptrs[slot][0] != v || ptrs[slot][sizes[slot] - 1] != v) job->errors++;
            if ((s >> 20) & 1) {
                size_t ns = 1 + ((s >> 3) % 3000);
                unsigned char* np = (unsigned char*)realloc(ptrs[slot], ns);
                if (!np) { job->errors++; continue; }
                if (np[0] != v) job->errors++;
                memset(np, v, ns);
                ptrs[slot] = np; sizes[slot] = ns;
            } else {
                free(ptrs[slot]);
                ptrs[slot] = nullptr;
            }
        } else {
            size_t n = (s >> 4) % 97 == 0 ? 70000 + (s % 300000) : 1 + ((s >> 3) % 2000);
            unsigned char* p = (unsigned char*)((s >> 9) % 13 == 0 ? aligned_alloc(64, n) : malloc(n));
            if (!p || ((uintptr_t)p & 15)) { job->errors++; continue; }
            memset(p, (unsigned char)(slot ^ job->seed), n);
            ptrs[slot] = p; sizes[slot] = n;
        }
        job->ops++;
    }
    for (int i = 0; i < SLOTS; i++) free(ptrs[i]);
}

static void test_threads() {
    AllocJob jobs[2] = { { 1234u, 0, 0 }, { 98765u, 0, 0 } };
    mos_thread_t t[2];
    double t0 = now_ms();
    int started = 0;
    for (int i = 0; i < 2; i++)
        if (mos_thread_create_vm(&t[i], alloc_worker, &jobs[i], 256 * 1024, nullptr) == 0) started++;
    if (started != 2) { fail("could not start the allocation threads (%d)", started); }
    for (int i = 0; i < started; i++) mos_thread_join(&t[i]);
    double t1 = now_ms();
    fe_malloc_stats_t st;
    fe_malloc_stats(&st);
    printf("FE SMOKE malloc: 2 threads x %ld ops in %.1f ms, errors %d/%d; live small %zu large %zu, mapped %zu KiB (cpus %u)\n",
           jobs[0].ops, t1 - t0, jobs[0].errors, jobs[1].errors, st.small_allocs, st.large_allocs, st.mapped_bytes / 1024,
           mos_cpu_count());
    if (jobs[0].errors || jobs[1].errors) fail("allocator: %d/%d corrupted blocks", jobs[0].errors, jobs[1].errors);
}

// ---- ImGui ----------------------------------------------------------------

static void handle_textures(ImDrawData* dd) {
    if (!dd->Textures) return;
    for (ImTextureData* tex : *dd->Textures) {
        if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
            tex->SetTexID((ImTextureID)1);
            tex->SetStatus(ImTextureStatus_OK);
        } else if (tex->Status == ImTextureStatus_WantDestroy) {
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

static void test_imgui() {
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0f / 60.0f;

    double t0 = now_ms();
    ImFont* def = io.Fonts->AddFontDefault();
    ImFont* inter = nullptr;
    const char* inter_path = "0:/programs/Inter-Regular.otf";
    if (mos_exists(inter_path) > 0) {
        inter = io.Fonts->AddFontFromFileTTF(inter_path, 18.0f);
        if (!inter) fail("AddFontFromFileTTF(%s) failed", inter_path);
    } else {
        printf("FE SMOKE note: %s not staged, Inter not loaded\n", inter_path);
    }
    if (!def) fail("AddFontDefault failed");
    double t1 = now_ms();

    float view[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -8, 1 };
    float proj[16];
    {
        float fov = 60.0f * 3.14159265f / 180.0f, aspect = 1280.0f / 800.0f, n = 0.1f, fa = 100.0f;
        float y = 1.0f / tanf(fov * 0.5f);
        memset(proj, 0, sizeof proj);
        proj[0] = y / aspect; proj[5] = y; proj[10] = -(fa + n) / (fa - n); proj[11] = -1.0f; proj[14] = -2.0f * fa * n / (fa - n);
    }
    float model[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0.25f, 0, 1 };

    long total_vtx = 0, total_idx = 0, total_cmds = 0;
    double frame_ms[5];
    for (int frame = 0; frame < 5; frame++) {
        double fs = now_ms();
        io.AddMousePosEvent(300.0f + frame * 40.0f, 200.0f + frame * 25.0f);
        io.AddMouseButtonEvent(0, frame == 2);
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        ImGui::DockSpaceOverViewport();
        ImGui::ShowDemoWindow();
        ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
        ImGui::Begin("Gizmo");
        if (inter) { ImGui::PushFont(inter, 0.0f); ImGui::TextUnformatted("Inter Regular: The quick brown fox"); ImGui::PopFont(); }
        ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(p.x, p.y, sz.x, sz.y);
        if (frame == 4) {
            ImGuizmo::Manipulate(view, proj, ImGuizmo::TRANSLATE | ImGuizmo::ROTATE, ImGuizmo::WORLD, model);
            ImGui::Text("gizmo over: %d", ImGuizmo::IsOver() ? 1 : 0);
        }
        ImGui::End();
        ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData();
        handle_textures(dd);
        long vtx = 0, idx = 0, cmds = 0;
        for (ImDrawList* dl : dd->CmdLists) {
            vtx += dl->VtxBuffer.Size;
            idx += dl->IdxBuffer.Size;
            cmds += dl->CmdBuffer.Size;
        }
        if (vtx != dd->TotalVtxCount || idx != dd->TotalIdxCount) fail("draw data counts do not add up");
        total_vtx += vtx; total_idx += idx; total_cmds += cmds;
        frame_ms[frame] = now_ms() - fs;
        printf("FE SMOKE frame %d: %ld lists, %ld vertices, %ld indices, %ld commands, %.1f ms\n",
               frame, (long)dd->CmdListsCount, vtx, idx, cmds, frame_ms[frame]);
    }
    double frames = 0;
    for (double f : frame_ms) frames += f;
    if (total_vtx < 1000) fail("too few vertices: %ld", total_vtx);
    ImFontAtlas* atlas = io.Fonts;
    int tw = atlas->TexData ? atlas->TexData->Width : 0, th = atlas->TexData ? atlas->TexData->Height : 0;
    printf("FE SMOKE imgui %s: fonts added in %.1f ms, 5 frames in %.1f ms (%.1f ms/frame), %ld vertices total, atlas %dx%d\n",
           ImGui::GetVersion(), t1 - t0, frames, frames / 5, total_vtx, tw, th);

    // ini round trip: memory, then disk
    size_t len = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&len);
    char* s1 = (char*)malloc(len + 1);
    memcpy(s1, ini, len + 1);
    if (!strstr(s1, "[Window][Dear ImGui Demo]") || !strstr(s1, "[Docking]")) fail("ini lacks expected sections:\n%s", s1);
    ImGui::LoadIniSettingsFromMemory(s1, len);
    // the dock nodes are rebuilt from the loaded settings by the next frame
    ImGui::NewFrame();
    ImGui::DockSpaceOverViewport();
    ImGui::ShowDemoWindow();
    ImGui::Begin("Gizmo");
    ImGui::End();
    ImGui::Render();
    handle_textures(ImGui::GetDrawData());
    const char* ini2 = ImGui::SaveIniSettingsToMemory(&len);
    if (strcmp(s1, ini2) != 0) fail("ini changed after save/load/frame/save:\n%s\n----\n%s", s1, ini2);
    ImGui::SaveIniSettingsToDisk("0:/fe_smoke.ini");
    FILE* f = fopen("0:/fe_smoke.ini", "rb");
    if (!f) fail("ini file not written");
    else {
        fseek(f, 0, SEEK_END);
        long fl = ftell(f);
        fclose(f);
        if (fl != (long)strlen(s1)) fail("ini file has %ld bytes, expected %zu", fl, strlen(s1));
    }
    ImGui::LoadIniSettingsFromDisk("0:/fe_smoke.ini");
    remove("0:/fe_smoke.ini");
    printf("FE SMOKE ini: %zu bytes, round trip ok\n", strlen(s1));
    free(s1);
    ImGui::DestroyContext(ctx);
}

// A C++ global with a constructor: crt0 must have run it (.ctors/.init_array).
struct CtorCheck { volatile int v; CtorCheck() : v(42) {} };
static CtorCheck g_ctor_check;

// ... and its destructor runs in exit() (__cxa_atexit), after main returns.
struct DtorCheck { ~DtorCheck() { printf("FE SMOKE static destructor ran\n"); } };
static DtorCheck g_dtor_check;

// The kernel fills a buffer that was never touched (fresh demand-zero pages
// of a big malloc block) through SYS_READ, and the data matches fread.
static void test_kernel_writes_untouched(const char* self) {
    long fd = mos_open(self, SYS_O_RDONLY);
    if (fd <= 0) { fail("cannot open %s", self); return; }
    long size = mos_size(fd);
    unsigned char* raw = (unsigned char*)malloc((size_t)size);
    long got = 0, n;
    while (got < size && (n = mos_read(fd, raw + got, (size_t)(size - got))) > 0) got += n;
    mos_close(fd);
    FILE* f = fopen(self, "rb");
    unsigned char* viaf = (unsigned char*)malloc((size_t)size);
    size_t fgot = f ? fread(viaf, 1, (size_t)size, f) : 0;
    if (f) fclose(f);
    if (got != size || (long)fgot != size || memcmp(raw, viaf, (size_t)size) != 0)
        fail("SYS_READ into untouched memory: %ld/%zu of %ld bytes, %s", got, fgot, size,
             got == size && (long)fgot == size ? "contents differ" : "short read");
    else
        printf("FE SMOKE SYS_READ into untouched heap memory: %ld bytes ok\n", size);
    free(raw);
    free(viaf);
}

static void at_exit_note() { printf("FE SMOKE atexit handler ran\n"); }

int main(int argc, char** argv) {
    printf("FE SMOKE start (%s)\n", argc > 0 ? argv[0] : "?");
    atexit(at_exit_note);
    if (g_ctor_check.v != 42) fail("static constructors did not run");
    double t0 = now_ms();
    test_math();
    test_text();
    test_files();
    test_threads();
    test_imgui();
    test_kernel_writes_untouched(argc > 0 ? argv[0] : "0:/programs/imgui_smoke.run");
    printf("FE SMOKE total %.1f ms\n", now_ms() - t0);
    if (g_fail) printf("FE SMOKE FAIL: %s\n", g_fail);
    else printf("FE SMOKE PASS\n");
    return g_fail ? 1 : 0;
}
