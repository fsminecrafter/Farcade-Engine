// Screen shader: black and white. u_params.x = amount (0 = colour, 1 = grey).
uniform sampler2D u_screen;
uniform vec4 u_params;
varying vec2 uv;
void main() {
    vec3 c = texture2D(u_screen, uv).rgb;
    float g = dot(c, vec3(0.2126, 0.7152, 0.0722));
    gl_FragColor = vec4(mix(c, vec3(g, g, g), clamp(u_params.x, 0.0, 1.0)), 1.0);
}
