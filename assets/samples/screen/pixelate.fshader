// Screen shader: big pixels. u_params.y = pixel size in screen pixels (8 when 0).
uniform sampler2D u_screen;
uniform vec4 u_res;
uniform vec4 u_params;
varying vec2 uv;
void main() {
    float size = u_params.y > 0.5 ? u_params.y : 8.0;
    vec2 cell = vec2(size, size) * u_res.zw;
    vec2 p = (floor(uv / cell) + 0.5) * cell;
    gl_FragColor = vec4(texture2D(u_screen, p).rgb, 1.0);
}
