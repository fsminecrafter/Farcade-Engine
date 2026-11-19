// Screen shader: the negative image. u_params.x = amount.
uniform sampler2D u_screen;
uniform vec4 u_params;
varying vec2 uv;
void main() {
    vec3 c = texture2D(u_screen, uv).rgb;
    gl_FragColor = vec4(mix(c, vec3(1.0, 1.0, 1.0) - c, clamp(u_params.x, 0.0, 1.0)), 1.0);
}
