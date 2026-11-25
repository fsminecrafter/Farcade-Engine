// @vertex
// Foliage shader: sways in the wind. Put it in a terrain foliage layer's Shader slot, or from a script:
//   fe.assign_shader_to_foliage("grass", "shaders/sway.fshader")
// The tip of a model moves the most: the sway grows from v = 1 (the base) to v = 0 (the tip). For models
// whose tips are at v = 1, change (1.0 - texcoord.y) to texcoord.y below. u_time.x is seconds.
uniform mat4 fgl_ModelViewProjection;
uniform vec4 u_time;
attribute vec3 position;
attribute vec3 normal;
attribute vec2 texcoord;
varying vec2 uv;
varying vec3 nrm;
void main() {
    uv = texcoord;
    nrm = normal;
    float bend = (1.0 - texcoord.y) * 0.12;
    float phase = position.x * 0.31 + position.z * 0.23;
    float w = sin(u_time.x * 1.7 + phase) + 0.35 * sin(u_time.x * 4.3 + phase * 2.0);
    vec3 p = position + vec3(w * bend, 0.0, w * bend * 0.6);
    gl_Position = fgl_ModelViewProjection * vec4(p, 1.0);
}

// @fragment
// The models' texture and colour (u_mat), lit from above.
uniform sampler2D u_texture;
uniform vec4 u_mat;
varying vec2 uv;
varying vec3 nrm;
void main() {
    float l = 0.55 + 0.45 * clamp(dot(normalize(nrm), vec3(0.3, 0.9, 0.3)), 0.0, 1.0);
    gl_FragColor = vec4(texture2D(u_texture, uv).rgb * u_mat.rgb * l, 1.0);
}
