// Small vector math for the engine and editor: Vec2/3/4, Quat, Mat4.
//
// Mat4 is column-major (m[col*4 + row]), the layout OpenGL, ImGuizmo and
// FarcadeGL use; vectors are columns (p' = M * p). Right-handed, y up,
// camera looking down -z. No libm: the few functions needed come from the
// compiler (sqrtf) or are written here.
#pragma once

namespace fe {

float fsin(float x);
float fcos(float x);
float ftan(float x);
float fatan2(float y, float x);
float fasin(float x);
inline float fsqrt(float x) { return __builtin_sqrtf(x); }
inline float fabsf_(float x) { return x < 0 ? -x : x; }
inline float fminf_(float a, float b) { return a < b ? a : b; }
inline float fmaxf_(float a, float b) { return a > b ? a : b; }
inline float fclamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

constexpr float kPi = 3.14159265358979323846f;
inline float radians(float deg) { return deg * (kPi / 180.0f); }
inline float degrees(float rad) { return rad * (180.0f / kPi); }

struct Vec2 { float x = 0, y = 0; };

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
    Vec3 operator-() const { return { -x, -y, -z }; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float length(const Vec3& v) { return fsqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) { float l = length(v); return l > 0 ? v * (1.0f / l) : Vec3(0, 0, 0); }
inline Vec3 mul(const Vec3& a, const Vec3& b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline Vec3 vmin(const Vec3& a, const Vec3& b) { return { fminf_(a.x, b.x), fminf_(a.y, b.y), fminf_(a.z, b.z) }; }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return { fmaxf_(a.x, b.x), fmaxf_(a.y, b.y), fmaxf_(a.z, b.z) }; }

struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    static Quat axis_angle(const Vec3& axis, float rad);
    // Euler angles in degrees, applied in the order X, then Y, then Z.
    static Quat euler_deg(const Vec3& deg);
    Vec3 to_euler_deg() const;
};
Quat operator*(const Quat& a, const Quat& b);
Vec3 rotate(const Quat& q, const Vec3& v);
Quat normalize(const Quat& q);

struct Mat4 {
    float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
    static Mat4 identity() { return Mat4(); }
    static Mat4 translation(const Vec3& t);
    static Mat4 scale(const Vec3& s);
    static Mat4 rotation(const Quat& q);
    static Mat4 trs(const Vec3& t, const Quat& r, const Vec3& s);
    static Mat4 perspective(float fovy_rad, float aspect, float znear, float zfar);
    // OpenGL clip conventions (z from -1 at near to +1 at far), like perspective.
    static Mat4 orthographic(float half_height, float aspect, float znear, float zfar);
    static Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up);
};
Mat4 operator*(const Mat4& a, const Mat4& b);
Vec4 operator*(const Mat4& a, const Vec4& v);
Vec3 transform_point(const Mat4& a, const Vec3& p);
Vec3 transform_dir(const Mat4& a, const Vec3& d);
bool invert(const Mat4& a, Mat4* out);
// Splits an affine matrix into translation, rotation and (positive) scale.
void decompose(const Mat4& a, Vec3* t, Quat* r, Vec3* s);

// A ray for picking.
struct Ray { Vec3 origin, dir; };
// Distance along the ray to the triangle, or a negative value (Möller–Trumbore).
float ray_triangle(const Ray& r, const Vec3& a, const Vec3& b, const Vec3& c);
// Entry distance into the box, or a negative value when missed.
float ray_aabb(const Ray& r, const Vec3& lo, const Vec3& hi);

}  // namespace fe
