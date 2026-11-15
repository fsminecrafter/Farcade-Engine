// fe-gfx-shot <out.png> [width height]: renders a demo scene (ground plane,
// cubes and spheres, a sun, a point and a spot light, a gradient sky, ACES)
// with fe::gfx::Renderer on the host FGPU device and writes it as a PNG.
#include "gfx/renderer.h"
#include "gfx/meshgen.h"
#include "scene_demo.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: fe-gfx-shot <out.png> [width height]\n"); return 2; }
    int w = argc > 3 ? atoi(argv[2]) : 800, h = argc > 3 ? atoi(argv[3]) : 530;
    fe::gfx::Renderer r;
    if (!r.init(w, h)) { fprintf(stderr, "init: %s\n", r.error()); return 1; }
    fe::gfx::DemoScene demo;
    demo.build((float)w / (float)h);
    // The first frame uploads the meshes and compiles the shader; time the second.
    for (int i = 0; i < 2; i++) {
        if (!r.render(demo.scene)) { fprintf(stderr, "render: %s\n", r.error()); return 1; }
        printf("frame %d: %.1f ms, %d draws, %d triangles, %d uploads\n", i, r.stats().ms, r.stats().draws,
               r.stats().triangles, r.stats().uploads);
    }
    if (!stbi_write_png(argv[1], w, h, 4, r.pixels(), w * 4)) { fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
    printf("wrote %s (%dx%d)\n", argv[1], w, h);
    return 0;
}
