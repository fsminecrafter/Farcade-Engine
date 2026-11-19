// Screen shader: an old CRT. Scanlines, a slight colour fringe and a vignette.
// u_params.x = strength (0..1).
uniform sampler2D u_screen;
uniform vec4 u_res;
uniform vec4 u_time;
uniform vec4 u_params;
varying vec2 uv;
void main() {
    float k = clamp(u_params.x, 0.0, 1.0);
    vec2 d = uv - vec2(0.5, 0.5);
    float off = 1.5 * k * u_res.z;
    float r = texture2D(u_screen, uv + vec2(off, 0.0)).r;
    float g = texture2D(u_screen, uv).g;
    float b = texture2D(u_screen, uv - vec2(off, 0.0)).b;
    float scan = 0.5 + 0.5 * sin(uv.y * u_res.y * 3.14159 + u_time.x * 2.0);
    float vig = 1.0 - dot(d, d) * 1.6 * k;
    vec3 c = vec3(r, g, b) * mix(1.0, 0.75 + 0.25 * scan, k) * vig;
    gl_FragColor = vec4(c, 1.0);
}
