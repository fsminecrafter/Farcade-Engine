// @displace
// Foliage shader: sways in the wind. Put it in a terrain foliage layer's Shader slot, or from a script:
//   fe.assign_shader_to_foliage("grass", "shaders/sway.fshader")
// A displace hook: the renderer runs it in every pass that draws the mesh (shadow maps, depth, normals,
// lighting), so the shadows and the ambient occlusion sway with the grass and every effect stays on.
// p is the vertex in the mesh's own space (for foliage: terrain space), n its normal, uv its texture
// coordinate. The tip of a model moves the most: the sway grows from v = 1 (the base) to v = 0 (the tip).
// For models whose tips are at v = 1, change (1.0 - uv.y) to uv.y in both functions. u_time.x is seconds.
uniform vec4 u_time;
float sway(vec3 p) {
    float phase = p.x * 0.31 + p.z * 0.23;
    return sin(u_time.x * 1.7 + phase) + 0.35 * sin(u_time.x * 4.3 + phase * 2.0);
}
vec3 displace(vec3 p, vec3 n, vec2 uv) {
    float w = sway(p) * (1.0 - uv.y) * 0.12;
    return p + vec3(w, 0.0, w * 0.6);
}
// The normal leans with the blade (a blade bent along L turns its normals the same way). Without this
// function the renderer works the normal out from displace by itself, which suits displacements that
// depend on the position only (waves, wobble); this one depends on uv, which it cannot see.
vec3 displace_normal(vec3 p, vec3 n, vec2 uv) {
    vec3 lean = vec3(1.0, 0.0, 0.6) * (sway(p) * 0.3);
    return normalize(n + lean * n.y - vec3(0.0, dot(n, lean), 0.0));
}
