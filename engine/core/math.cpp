#include "core/math.h"

namespace fe {

// ---- scalar functions (no libm; accurate to about 1e-6 for the editor's needs)

static float wrap_pi(float x) {
    // Into [-pi, pi].
    const float two_pi = 2.0f * kPi;
    float k = x / two_pi;
    long n = (long)(k + (k >= 0 ? 0.5f : -0.5f));
    return x - (float)n * two_pi;
}

float fsin(float x) {
    x = wrap_pi(x);
    // Fold into [-pi/2, pi/2]: sin(pi - x) = sin(x).
    if (x > kPi * 0.5f) x = kPi - x;
    else if (x < -kPi * 0.5f) x = -kPi - x;
    float x2 = x * x;
    // Taylor series to x^11 (error < 1e-7 on [-pi/2, pi/2]).
    return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880 + x2 * (-1.0f / 39916800))))));
}

float fcos(float x) { return fsin(x + kPi * 0.5f); }

float ftan(float x) {
    float c = fcos(x);
    return c != 0.0f ? fsin(x) / c : 1e30f;
}

static float atan_unit(float t) {
    // atan for |t| <= 1: minimax-style polynomial (error ~1e-6).
    float t2 = t * t;
    return t * (0.99997726f + t2 * (-0.33262347f + t2 * (0.19354346f + t2 * (-0.11643287f + t2 * (0.05265332f + t2 * -0.01172120f)))));
}

float fatan2(float y, float x) {
    if (x == 0.0f && y == 0.0f) return 0.0f;
    float ax = fabsf_(x), ay = fabsf_(y);
    float a = ax >= ay ? atan_unit(ay / ax) : (kPi * 0.5f - atan_unit(ax / ay));
    if (x < 0) a = kPi - a;
    return y < 0 ? -a : a;
}

float fasin(float x) {
    x = fclamp(x, -1.0f, 1.0f);
    return fatan2(x, fsqrt(1.0f - x * x));
}

// ---- quaternions

Quat Quat::axis_angle(const Vec3& axis, float rad) {
    Vec3 a = normalize(axis);
    float s = fsin(rad * 0.5f);
    return { a.x * s, a.y * s, a.z * s, fcos(rad * 0.5f) };
}

Quat Quat::euler_deg(const Vec3& deg) {
    Quat qx = axis_angle({ 1, 0, 0 }, radians(deg.x));
    Quat qy = axis_angle({ 0, 1, 0 }, radians(deg.y));
    Quat qz = axis_angle({ 0, 0, 1 }, radians(deg.z));
    return normalize(qz * qy * qx);
}

Vec3 Quat::to_euler_deg() const {
    // Inverse of euler_deg (R = Rz * Ry * Rx).
    float sinr = 2.0f * (w * x + y * z), cosr = 1.0f - 2.0f * (x * x + y * y);
    float sinp = 2.0f * (w * y - z * x);
    float siny = 2.0f * (w * z + x * y), cosy = 1.0f - 2.0f * (y * y + z * z);
    return { degrees(fatan2(sinr, cosr)), degrees(fasin(sinp)), degrees(fatan2(siny, cosy)) };
}

Quat operator*(const Quat& a, const Quat& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

Vec3 rotate(const Quat& q, const Vec3& v) {
    Vec3 u(q.x, q.y, q.z);
    Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

Quat normalize(const Quat& q) {
    float l = fsqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l <= 0) return Quat();
    float i = 1.0f / l;
    return { q.x * i, q.y * i, q.z * i, q.w * i };
}

// ---- matrices

Mat4 Mat4::translation(const Vec3& t) {
    Mat4 r;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

Mat4 Mat4::scale(const Vec3& s) {
    Mat4 r;
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

Mat4 Mat4::rotation(const Quat& q) {
    Mat4 r;
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r.at(0, 0) = 1 - 2 * (yy + zz); r.at(0, 1) = 2 * (xy - wz);     r.at(0, 2) = 2 * (xz + wy);
    r.at(1, 0) = 2 * (xy + wz);     r.at(1, 1) = 1 - 2 * (xx + zz); r.at(1, 2) = 2 * (yz - wx);
    r.at(2, 0) = 2 * (xz - wy);     r.at(2, 1) = 2 * (yz + wx);     r.at(2, 2) = 1 - 2 * (xx + yy);
    return r;
}

Mat4 Mat4::trs(const Vec3& t, const Quat& q, const Vec3& s) {
    Mat4 r = rotation(q);
    for (int i = 0; i < 3; i++) {
        r.at(i, 0) *= s.x;
        r.at(i, 1) *= s.y;
        r.at(i, 2) *= s.z;
    }
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

Mat4 Mat4::perspective(float fovy, float aspect, float zn, float zf) {
    Mat4 r;
    float f = 1.0f / ftan(fovy * 0.5f);
    for (float& v : r.m) v = 0;
    r.at(0, 0) = f / aspect;
    r.at(1, 1) = f;
    r.at(2, 2) = (zf + zn) / (zn - zf);
    r.at(2, 3) = 2.0f * zf * zn / (zn - zf);
    r.at(3, 2) = -1.0f;
    return r;
}

Mat4 Mat4::orthographic(float half_height, float aspect, float zn, float zf) {
    Mat4 r;
    float hw = half_height * aspect;
    r.at(0, 0) = 1.0f / hw;
    r.at(1, 1) = 1.0f / half_height;
    r.at(2, 2) = -2.0f / (zf - zn);
    r.at(2, 3) = -(zf + zn) / (zf - zn);
    return r;
}

Mat4 Mat4::look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Vec3 f = normalize(target - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r;
    r.at(0, 0) = s.x;  r.at(0, 1) = s.y;  r.at(0, 2) = s.z;  r.at(0, 3) = -dot(s, eye);
    r.at(1, 0) = u.x;  r.at(1, 1) = u.y;  r.at(1, 2) = u.z;  r.at(1, 3) = -dot(u, eye);
    r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z; r.at(2, 3) = dot(f, eye);
    return r;
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.at(row, k) * b.at(k, c);
            r.at(row, c) = s;
        }
    return r;
}

Vec4 operator*(const Mat4& a, const Vec4& v) {
    return { a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z + a.at(0, 3) * v.w,
             a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z + a.at(1, 3) * v.w,
             a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z + a.at(2, 3) * v.w,
             a.at(3, 0) * v.x + a.at(3, 1) * v.y + a.at(3, 2) * v.z + a.at(3, 3) * v.w };
}

Vec3 transform_point(const Mat4& a, const Vec3& p) {
    Vec4 r = a * Vec4{ p.x, p.y, p.z, 1.0f };
    return { r.x, r.y, r.z };
}

Vec3 transform_dir(const Mat4& a, const Vec3& d) {
    Vec4 r = a * Vec4{ d.x, d.y, d.z, 0.0f };
    return { r.x, r.y, r.z };
}

bool invert(const Mat4& a, Mat4* out) {
    const float* m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) out->m[i] = inv[i] * det;
    return true;
}

void decompose(const Mat4& a, Vec3* t, Quat* r, Vec3* s) {
    *t = { a.m[12], a.m[13], a.m[14] };
    Vec3 c0(a.m[0], a.m[1], a.m[2]), c1(a.m[4], a.m[5], a.m[6]), c2(a.m[8], a.m[9], a.m[10]);
    *s = { length(c0), length(c1), length(c2) };
    if (s->x > 0) c0 = c0 * (1.0f / s->x);
    if (s->y > 0) c1 = c1 * (1.0f / s->y);
    if (s->z > 0) c2 = c2 * (1.0f / s->z);
    // Rotation matrix to quaternion.
    float m00 = c0.x, m11 = c1.y, m22 = c2.z;
    float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0) {
        float k = fsqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * k;
        q.x = (c1.z - c2.y) / k;
        q.y = (c2.x - c0.z) / k;
        q.z = (c0.y - c1.x) / k;
    } else if (m00 > m11 && m00 > m22) {
        float k = fsqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (c1.z - c2.y) / k;
        q.x = 0.25f * k;
        q.y = (c1.x + c0.y) / k;
        q.z = (c2.x + c0.z) / k;
    } else if (m11 > m22) {
        float k = fsqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (c2.x - c0.z) / k;
        q.x = (c1.x + c0.y) / k;
        q.y = 0.25f * k;
        q.z = (c2.y + c1.z) / k;
    } else {
        float k = fsqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (c0.y - c1.x) / k;
        q.x = (c2.x + c0.z) / k;
        q.y = (c2.y + c1.z) / k;
        q.z = 0.25f * k;
    }
    *r = normalize(q);
}

float ray_triangle(const Ray& r, const Vec3& a, const Vec3& b, const Vec3& c) {
    Vec3 e1 = b - a, e2 = c - a;
    Vec3 p = cross(r.dir, e2);
    float det = dot(e1, p);
    if (det > -1e-8f && det < 1e-8f) return -1.0f;
    float inv = 1.0f / det;
    Vec3 s = r.origin - a;
    float u = dot(s, p) * inv;
    if (u < 0 || u > 1) return -1.0f;
    Vec3 q = cross(s, e1);
    float v = dot(r.dir, q) * inv;
    if (v < 0 || u + v > 1) return -1.0f;
    float t = dot(e2, q) * inv;
    return t > 0 ? t : -1.0f;
}

float ray_aabb(const Ray& r, const Vec3& lo, const Vec3& hi) {
    float tmin = -1e30f, tmax = 1e30f;
    const float* o = &r.origin.x;
    const float* d = &r.dir.x;
    const float* l = &lo.x;
    const float* h = &hi.x;
    for (int i = 0; i < 3; i++) {
        if (d[i] > -1e-12f && d[i] < 1e-12f) {
            if (o[i] < l[i] || o[i] > h[i]) return -1.0f;
            continue;
        }
        float inv = 1.0f / d[i];
        float t0 = (l[i] - o[i]) * inv, t1 = (h[i] - o[i]) * inv;
        if (t0 > t1) { float t = t0; t0 = t1; t1 = t; }
        tmin = fmaxf_(tmin, t0);
        tmax = fminf_(tmax, t1);
        if (tmin > tmax) return -1.0f;
    }
    if (tmax < 0) return -1.0f;
    return tmin > 0 ? tmin : 0.0f;
}

}  // namespace fe
