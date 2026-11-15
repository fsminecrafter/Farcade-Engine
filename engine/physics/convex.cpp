#include "physics/convex.h"

#include <stdlib.h>
#include <string.h>

namespace fe {

namespace {

float absf(float a) { return a < 0 ? -a : a; }
Vec3 dir_in_frame(const Convex& s, const Vec3& d) { return Vec3(dot(d, s.ax[0]), dot(d, s.ax[1]), dot(d, s.ax[2])); }
Vec3 from_frame(const Convex& s, const Vec3& l) { return s.c + s.ax[0] * l.x + s.ax[1] * l.y + s.ax[2] * l.z; }
float sgn(float x) { return x < 0 ? -1.0f : 1.0f; }

float rounding(const Convex& s) { return (s.kind == ShapeKind::Sphere || s.kind == ShapeKind::Capsule) ? s.r : 0.0f; }

// ------------------------------------------------------------- cylinder ring

constexpr int kRing = 12;
float g_cs[kRing][2];
bool g_cs_ready = false;
void ring_init() {
    if (g_cs_ready) return;
    for (int i = 0; i < kRing; i++) {
        float a = 2.0f * kPi * (float)i / (float)kRing;
        g_cs[i][0] = fcos(a);
        g_cs[i][1] = fsin(a);
    }
    g_cs_ready = true;
}

// ------------------------------------------------------------------- hull

int hull_best_vertex(const Convex& s, const Vec3& dl) {
    const Hull* h = s.hull;
    Vec3 q(dl.x * s.scale.x, dl.y * s.scale.y, dl.z * s.scale.z);
    int best = 0;
    float bd = -1e30f;
    for (int i = 0; i < h->nv; i++) {
        float d = dot(q, h->v[i]);
        if (d > bd) { bd = d; best = i; }
    }
    return best;
}

}  // namespace

Vec3 convex_support(const Convex& s, const Vec3& d) {
    switch (s.kind) {
    case ShapeKind::Sphere: return s.c;
    case ShapeKind::Box:
        return s.c + s.ax[0] * (sgn(dot(d, s.ax[0])) * s.he.x) + s.ax[1] * (sgn(dot(d, s.ax[1])) * s.he.y) +
               s.ax[2] * (sgn(dot(d, s.ax[2])) * s.he.z);
    case ShapeKind::Capsule: return s.c + s.ax[1] * (sgn(dot(d, s.ax[1])) * s.he.y);
    case ShapeKind::Cylinder: {
        float dx = dot(d, s.ax[0]), dy = dot(d, s.ax[1]), dz = dot(d, s.ax[2]);
        float l2 = dx * dx + dz * dz;
        Vec3 p = s.c + s.ax[1] * (sgn(dy) * s.he.y);
        if (l2 > 1e-12f) {
            float k = s.he.x / fsqrt(l2);
            p += s.ax[0] * (dx * k) + s.ax[2] * (dz * k);
        }
        return p;
    }
    case ShapeKind::Hull: {
        Vec3 dl = dir_in_frame(s, d);
        const Vec3& v = s.hull->v[hull_best_vertex(s, dl)];
        return from_frame(s, Vec3(v.x * s.scale.x, v.y * s.scale.y, v.z * s.scale.z));
    }
    default: return s.c;
    }
}

void convex_bounds(Convex* s) {
    float r = rounding(*s);
    Vec3 lo, hi;
    lo.x = convex_support(*s, Vec3(-1, 0, 0)).x - r;
    hi.x = convex_support(*s, Vec3(1, 0, 0)).x + r;
    lo.y = convex_support(*s, Vec3(0, -1, 0)).y - r;
    hi.y = convex_support(*s, Vec3(0, 1, 0)).y + r;
    lo.z = convex_support(*s, Vec3(0, 0, -1)).z - r;
    hi.z = convex_support(*s, Vec3(0, 0, 1)).z + r;
    s->lo = lo;
    s->hi = hi;
}

namespace {

// ---------------------------------------------------------------------- GJK

struct SP { Vec3 w, a, b; };       // Minkowski point of A - B and its two sources

SP make_sp(const Convex& A, const Convex& B, const Vec3& d) {
    SP s;
    s.a = convex_support(A, d);
    s.b = convex_support(B, -d);
    s.w = s.a - s.b;
    return s;
}

// Barycentric weights of the point of triangle abc closest to the origin
// (a weight is 0 for a vertex that is not part of the closest feature).
void closest_tri(const Vec3& a, const Vec3& b, const Vec3& c, float* l) {
    Vec3 ab = b - a, ac = c - a, ap = -a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { l[0] = 1; l[1] = 0; l[2] = 0; return; }
    Vec3 bp = -b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { l[0] = 0; l[1] = 1; l[2] = 0; return; }
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        float v = d1 / (d1 - d3);
        l[0] = 1 - v; l[1] = v; l[2] = 0;
        return;
    }
    Vec3 cp = -c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { l[0] = 0; l[1] = 0; l[2] = 1; return; }
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        float w = d2 / (d2 - d6);
        l[0] = 1 - w; l[1] = 0; l[2] = w;
        return;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        l[0] = 0; l[1] = 1 - w; l[2] = w;
        return;
    }
    float sum = va + vb + vc;
    if (absf(sum) < 1e-30f) { l[0] = 1; l[1] = 0; l[2] = 0; return; }
    float den = 1.0f / sum;
    float v = vb * den, w = vc * den;
    l[0] = 1 - v - w; l[1] = v; l[2] = w;
}

// Reduces the simplex to the feature closest to the origin. Returns true if
// the origin is inside a tetrahedron. v, pa, pb: the closest point and its sources.
bool reduce(SP* s, int* n, Vec3* v, Vec3* pa, Vec3* pb) {
    float l[4] = { 1, 0, 0, 0 };
    int keep[4] = { 0, 1, 2, 3 };
    int cnt = *n;
    if (*n == 2) {
        Vec3 ab = s[1].w - s[0].w;
        float den = dot(ab, ab);
        float t = den > 1e-30f ? -dot(s[0].w, ab) / den : 0.0f;
        if (t <= 0) { l[0] = 1; l[1] = 0; }
        else if (t >= 1) { l[0] = 0; l[1] = 1; }
        else { l[0] = 1 - t; l[1] = t; }
    } else if (*n == 3) {
        closest_tri(s[0].w, s[1].w, s[2].w, l);
    } else if (*n == 4) {
        static const int F[4][4] = { { 0, 1, 2, 3 }, { 0, 1, 3, 2 }, { 0, 2, 3, 1 }, { 1, 2, 3, 0 } };
        float best = 1e30f;
        bool outside = false;
        for (int f = 0; f < 4; f++) {
            const Vec3& p0 = s[F[f][0]].w;
            Vec3 nrm = cross(s[F[f][1]].w - p0, s[F[f][2]].w - p0);
            float so = dot(nrm, -p0), sm = dot(nrm, s[F[f][3]].w - p0);
            if (so * sm > 0) continue;
            outside = true;
            float tl[3];
            closest_tri(p0, s[F[f][1]].w, s[F[f][2]].w, tl);
            Vec3 q = p0 * tl[0] + s[F[f][1]].w * tl[1] + s[F[f][2]].w * tl[2];
            float d2 = dot(q, q);
            if (d2 < best) {
                best = d2;
                l[0] = l[1] = l[2] = l[3] = 0;
                for (int k = 0; k < 3; k++) l[F[f][k]] = tl[k];
            }
        }
        if (!outside) return true;
    }
    int m = 0;
    SP out[4];
    float lw[4];
    for (int i = 0; i < cnt; i++)
        if (l[i] > 0 || (cnt == 1)) { out[m] = s[keep[i]]; lw[m] = l[i]; m++; }
    if (m == 0) { out[0] = s[0]; lw[0] = 1; m = 1; }
    Vec3 vv, a, b;
    for (int i = 0; i < m; i++) {
        s[i] = out[i];
        vv += out[i].w * lw[i];
        a += out[i].a * lw[i];
        b += out[i].b * lw[i];
    }
    *n = m;
    *v = vv; *pa = a; *pb = b;
    return false;
}

struct Gjk {
    bool intersect = false;
    float dist = 0;
    Vec3 n;                         // from A to B (valid when not intersecting)
    Vec3 pa, pb;
    SP s[4];
    int ns = 0;
};

void gjk(const Convex& A, const Convex& B, Gjk* R) {
    Vec3 d = B.c - A.c;
    if (dot(d, d) < 1e-12f) d = Vec3(1, 0, 0);
    SP s[4];
    int n = 1;
    s[0] = make_sp(A, B, d);
    Vec3 v = s[0].w, pa = s[0].a, pb = s[0].b;
    bool inside = false;
    for (int it = 0; it < 40; it++) {
        float vv = dot(v, v);
        if (vv < 1e-12f) { inside = true; break; }
        SP w = make_sp(A, B, -v);
        if (vv - dot(v, w.w) <= 1e-6f * vv + 1e-10f) break;
        bool dup = false;
        for (int i = 0; i < n; i++) {
            Vec3 e = s[i].w - w.w;
            if (dot(e, e) < 1e-14f) dup = true;
        }
        if (dup) break;
        s[n++] = w;
        if (reduce(s, &n, &v, &pa, &pb)) { inside = true; break; }
    }
    R->intersect = inside;
    for (int i = 0; i < n; i++) R->s[i] = s[i];
    R->ns = n;
    float dist = fsqrt(dot(v, v));
    R->dist = dist;
    R->pa = pa; R->pb = pb;
    R->n = dist > 1e-9f ? v * (-1.0f / dist) : Vec3(0, 1, 0);
}

// ---------------------------------------------------------------------- EPA

struct EFace { int v[3]; Vec3 n; float d; bool alive; };

bool epa(const Convex& A, const Convex& B, const Gjk& g, Vec3* nrm, float* depth, Vec3* pa, Vec3* pb) {
    SP vt[64];
    int nv = 0;
    for (int i = 0; i < g.ns; i++) vt[nv++] = g.s[i];
    static const Vec3 dirs[6] = { Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(0, 0, -1) };
    if (nv == 1) {
        for (int i = 0; i < 6 && nv < 2; i++) {
            SP p = make_sp(A, B, dirs[i]);
            Vec3 e = p.w - vt[0].w;
            if (dot(e, e) > 1e-10f) vt[nv++] = p;
        }
    }
    if (nv == 2) {
        Vec3 e = vt[1].w - vt[0].w;
        Vec3 ax = absf(e.x) < absf(e.y) && absf(e.x) < absf(e.z) ? Vec3(1, 0, 0) : (absf(e.y) < absf(e.z) ? Vec3(0, 1, 0) : Vec3(0, 0, 1));
        Vec3 p1 = normalize(cross(e, ax));
        Vec3 p2 = normalize(cross(e, p1));
        Vec3 cand[4] = { p1, -p1, p2, -p2 };
        for (int i = 0; i < 4 && nv < 3; i++) {
            SP p = make_sp(A, B, cand[i]);
            Vec3 c = cross(p.w - vt[0].w, e);
            if (dot(c, c) > 1e-10f) vt[nv++] = p;
        }
    }
    if (nv == 3) {
        Vec3 nn = cross(vt[1].w - vt[0].w, vt[2].w - vt[0].w);
        float l = length(nn);
        if (l > 1e-9f) {
            nn = nn * (1.0f / l);
            for (int i = 0; i < 2 && nv < 4; i++) {
                SP p = make_sp(A, B, i ? -nn : nn);
                if (absf(dot(p.w - vt[0].w, nn)) > 1e-6f) vt[nv++] = p;
            }
        }
    }
    if (nv < 4) return false;

    EFace faces[128];
    int nf = 0;
    Vec3 cen = (vt[0].w + vt[1].w + vt[2].w + vt[3].w) * 0.25f;
    static const int T[4][3] = { { 0, 1, 2 }, { 0, 1, 3 }, { 0, 2, 3 }, { 1, 2, 3 } };
    for (int i = 0; i < 4; i++) {
        EFace& f = faces[nf++];
        f.v[0] = T[i][0]; f.v[1] = T[i][1]; f.v[2] = T[i][2];
        Vec3 nn = cross(vt[f.v[1]].w - vt[f.v[0]].w, vt[f.v[2]].w - vt[f.v[0]].w);
        if (dot(nn, vt[f.v[0]].w - cen) < 0) { int t = f.v[1]; f.v[1] = f.v[2]; f.v[2] = t; nn = -nn; }
        f.n = normalize(nn);
        f.d = dot(f.n, vt[f.v[0]].w);
        f.alive = true;
    }
    int best = -1;
    for (int it = 0; it < 48; it++) {
        best = -1;
        float bd = 1e30f;
        for (int i = 0; i < nf; i++)
            if (faces[i].alive && faces[i].d < bd) { bd = faces[i].d; best = i; }
        if (best < 0) return false;
        EFace bf = faces[best];
        SP w = make_sp(A, B, bf.n);
        if (dot(w.w, bf.n) - bf.d < 1e-4f * (1.0f + bf.d) * 0.1f || nv >= 64) break;
        int wi = nv++;
        vt[wi] = w;
        int edges[96][2];
        int ne = 0;
        for (int i = 0; i < nf; i++) {
            EFace& f = faces[i];
            if (!f.alive) continue;
            if (dot(f.n, w.w - vt[f.v[0]].w) > 1e-6f) {
                f.alive = false;
                for (int k = 0; k < 3; k++) {
                    int a = f.v[k], b = f.v[(k + 1) % 3];
                    bool found = false;
                    for (int e = 0; e < ne; e++)
                        if (edges[e][0] == b && edges[e][1] == a) {
                            edges[e][0] = edges[ne - 1][0];
                            edges[e][1] = edges[ne - 1][1];
                            ne--;
                            found = true;
                            break;
                        }
                    if (!found && ne < 96) { edges[ne][0] = a; edges[ne][1] = b; ne++; }
                }
            }
        }
        for (int e = 0; e < ne; e++) {
            // Reuse a dead slot or append.
            int slot = -1;
            for (int i = 0; i < nf; i++) if (!faces[i].alive) { slot = i; break; }
            if (slot < 0) {
                if (nf >= 128) goto done;
                slot = nf++;
            }
            EFace& f = faces[slot];
            f.v[0] = edges[e][0]; f.v[1] = edges[e][1]; f.v[2] = wi;
            Vec3 nn = cross(vt[f.v[1]].w - vt[f.v[0]].w, vt[f.v[2]].w - vt[f.v[0]].w);
            float l = length(nn);
            if (l < 1e-12f) { f.alive = false; continue; }
            f.n = nn * (1.0f / l);
            f.d = dot(f.n, vt[f.v[0]].w);
            if (f.d < 0) { f.n = -f.n; f.d = -f.d; }
            f.alive = true;
        }
    }
done:
    best = -1;
    {
        float bd = 1e30f;
        for (int i = 0; i < nf; i++)
            if (faces[i].alive && faces[i].d < bd) { bd = faces[i].d; best = i; }
    }
    if (best < 0) return false;
    const EFace& f = faces[best];
    *nrm = f.n;
    *depth = f.d;
    // Witness points: the origin's projection on the face.
    const SP& a = vt[f.v[0]];
    const SP& b = vt[f.v[1]];
    const SP& c = vt[f.v[2]];
    Vec3 p = f.n * f.d;
    Vec3 v0 = b.w - a.w, v1 = c.w - a.w, v2 = p - a.w;
    float d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1), d20 = dot(v2, v0), d21 = dot(v2, v1);
    float den = d00 * d11 - d01 * d01;
    float u = 1, vb = 0, wb = 0;
    if (absf(den) > 1e-14f) {
        vb = (d11 * d20 - d01 * d21) / den;
        wb = (d00 * d21 - d01 * d20) / den;
        u = 1 - vb - wb;
    }
    *pa = a.a * u + b.a * vb + c.a * wb;
    *pb = a.b * u + b.b * vb + c.b * wb;
    return true;
}

// ------------------------------------------------------------ support faces

constexpr int kFaceMax = 16;

// Surface points of the face of `s` best facing `d` (unit). Returns the count.
int support_face(const Convex& s, const Vec3& d, Vec3* out) {
    switch (s.kind) {
    case ShapeKind::Sphere:
        out[0] = s.c + d * s.r;
        return 1;
    case ShapeKind::Box: {
        float dl[3] = { dot(d, s.ax[0]), dot(d, s.ax[1]), dot(d, s.ax[2]) };
        float h[3] = { s.he.x, s.he.y, s.he.z };
        int i = 0;
        if (absf(dl[1]) > absf(dl[i])) i = 1;
        if (absf(dl[2]) > absf(dl[i])) i = 2;
        int j = (i + 1) % 3, k = (i + 2) % 3;
        Vec3 fc = s.c + s.ax[i] * (sgn(dl[i]) * h[i]);
        Vec3 aj = s.ax[j] * h[j], ak = s.ax[k] * h[k];
        out[0] = fc + aj + ak;
        out[1] = fc - aj + ak;
        out[2] = fc - aj - ak;
        out[3] = fc + aj - ak;
        return 4;
    }
    case ShapeKind::Capsule: {
        float dy = dot(d, s.ax[1]);
        Vec3 top = s.c + s.ax[1] * s.he.y, bot = s.c - s.ax[1] * s.he.y;
        if (absf(dy) < 0.4f) {
            out[0] = top + d * s.r;
            out[1] = bot + d * s.r;
            return 2;
        }
        out[0] = (dy > 0 ? top : bot) + d * s.r;
        return 1;
    }
    case ShapeKind::Cylinder: {
        ring_init();
        float dx = dot(d, s.ax[0]), dy = dot(d, s.ax[1]), dz = dot(d, s.ax[2]);
        if (absf(dy) >= 0.7071f) {
            Vec3 fc = s.c + s.ax[1] * (sgn(dy) * s.he.y);
            for (int i = 0; i < kRing; i++) out[i] = fc + s.ax[0] * (g_cs[i][0] * s.he.x) + s.ax[2] * (g_cs[i][1] * s.he.x);
            return kRing;
        }
        float l = fsqrt(dx * dx + dz * dz);
        Vec3 rim = l > 1e-6f ? s.ax[0] * (dx / l * s.he.x) + s.ax[2] * (dz / l * s.he.x) : s.ax[0] * s.he.x;
        out[0] = s.c + rim + s.ax[1] * s.he.y;
        out[1] = s.c + rim - s.ax[1] * s.he.y;
        return 2;
    }
    case ShapeKind::Hull: {
        const Hull* h = s.hull;
        Vec3 dl = dir_in_frame(s, d);
        int bi = hull_best_vertex(s, dl);
        // The adjacent triangle that faces d most.
        int bt = -1;
        float bs = -1e30f;
        Vec3 bn;
        for (int t = 0; t < h->nt; t++) {
            if (h->tri[t * 3] != bi && h->tri[t * 3 + 1] != bi && h->tri[t * 3 + 2] != bi) continue;
            Vec3 nl = normalize(Vec3(h->tn[t].x / s.scale.x, h->tn[t].y / s.scale.y, h->tn[t].z / s.scale.z));
            float sc = dot(nl, dl);
            if (sc > bs) { bs = sc; bt = t; bn = nl; }
        }
        Vec3 sv0(h->v[bi].x * s.scale.x, h->v[bi].y * s.scale.y, h->v[bi].z * s.scale.z);
        if (bt < 0) { out[0] = from_frame(s, sv0); return 1; }
        float tol = 2e-4f * length(Vec3(h->half.x * s.scale.x, h->half.y * s.scale.y, h->half.z * s.scale.z)) + 1e-6f;
        Vec3 pts[64];
        int n = 0;
        for (int i = 0; i < h->nv && n < 64; i++) {
            Vec3 sv(h->v[i].x * s.scale.x, h->v[i].y * s.scale.y, h->v[i].z * s.scale.z);
            if (dot(bn, sv - sv0) >= -tol) pts[n++] = sv;
        }
        if (n < 3) { out[0] = from_frame(s, sv0); return 1; }
        Vec3 cen;
        for (int i = 0; i < n; i++) cen += pts[i];
        cen = cen * (1.0f / (float)n);
        Vec3 u = normalize(pts[0] - cen);
        if (dot(u, u) < 0.5f) u = normalize(pts[1] - cen);
        Vec3 w = cross(bn, u);
        float ang[64];
        for (int i = 0; i < n; i++) ang[i] = fatan2(dot(pts[i] - cen, w), dot(pts[i] - cen, u));
        for (int i = 1; i < n; i++) {                   // insertion sort by angle
            float a = ang[i];
            Vec3 p = pts[i];
            int j = i - 1;
            while (j >= 0 && ang[j] > a) { ang[j + 1] = ang[j]; pts[j + 1] = pts[j]; j--; }
            ang[j + 1] = a;
            pts[j + 1] = p;
        }
        int m = n > kFaceMax ? kFaceMax : n;
        for (int i = 0; i < m; i++) out[i] = from_frame(s, pts[m == n ? i : i * n / m]);
        return m;
    }
    default:
        out[0] = s.c;
        return 1;
    }
}

// Unit normal of a polygon (Newell), oriented so that dot(normal, toward) > 0.
Vec3 poly_normal(const Vec3* p, int n, const Vec3& toward) {
    Vec3 nn;
    for (int i = 0; i < n; i++) {
        const Vec3& a = p[i];
        const Vec3& b = p[(i + 1) % n];
        nn.x += (a.y - b.y) * (a.z + b.z);
        nn.y += (a.z - b.z) * (a.x + b.x);
        nn.z += (a.x - b.x) * (a.y + b.y);
    }
    nn = normalize(nn);
    return dot(nn, toward) < 0 ? -nn : nn;
}

// Keeps the 4 points that span the largest area (deepest first).
int reduce4(ContactPoint* p, int n) {
    if (n <= 4) return n;
    int idx[4];
    int bi = 0;
    for (int i = 1; i < n; i++) if (p[i].sep < p[bi].sep) bi = i;
    idx[0] = bi;
    int far = -1;
    float fd = -1;
    for (int i = 0; i < n; i++) {
        if (i == bi) continue;
        Vec3 e = p[i].pos - p[bi].pos;
        float d = dot(e, e);
        if (d > fd) { fd = d; far = i; }
    }
    idx[1] = far;
    int third = -1;
    float ta = -1;
    for (int i = 0; i < n; i++) {
        if (i == idx[0] || i == idx[1]) continue;
        float a = length(cross(p[idx[1]].pos - p[idx[0]].pos, p[i].pos - p[idx[0]].pos));
        if (a > ta) { ta = a; third = i; }
    }
    idx[2] = third;
    int fourth = -1;
    float fa = -1;
    for (int i = 0; i < n; i++) {
        if (i == idx[0] || i == idx[1] || i == idx[2]) continue;
        float a = 0;
        for (int k = 0; k < 3; k++) {
            const Vec3& e0 = p[idx[k]].pos;
            const Vec3& e1 = p[idx[(k + 1) % 3]].pos;
            a += length(cross(e1 - e0, p[i].pos - e0));
        }
        if (a > fa) { fa = a; fourth = i; }
    }
    idx[3] = fourth;
    ContactPoint out[4];
    for (int i = 0; i < 4; i++) out[i] = p[idx[i]];
    for (int i = 0; i < 4; i++) p[i] = out[i];
    return 4;
}

// Clips `in` (1, 2 or a polygon of n points) against the side planes of the
// reference polygon `ref`. Output up to 40 points.
int clip_to_polygon(const Vec3* in, int ni, const Vec3* ref, int nr, const Vec3& nref, Vec3* out) {
    Vec3 cen;
    for (int i = 0; i < nr; i++) cen += ref[i];
    cen = cen * (1.0f / (float)nr);
    Vec3 bufA[40], bufB[40];
    int na = ni;
    for (int i = 0; i < ni; i++) bufA[i] = in[i];
    Vec3* cur = bufA;
    Vec3* nxt = bufB;
    for (int e = 0; e < nr && na > 0; e++) {
        const Vec3& p0 = ref[e];
        Vec3 edge = ref[(e + 1) % nr] - p0;
        Vec3 m = cross(edge, nref);
        if (dot(m, cen - p0) < 0) m = -m;
        float ml = length(m);
        if (ml < 1e-12f) continue;
        m = m * (1.0f / ml);
        int nn = 0;
        if (na == 1) {
            if (dot(cur[0] - p0, m) >= 0) nxt[nn++] = cur[0];
        } else if (na == 2) {
            float d0 = dot(cur[0] - p0, m), d1 = dot(cur[1] - p0, m);
            Vec3 a = cur[0], b = cur[1];
            if (d0 < 0 && d1 < 0) nn = 0;
            else {
                if (d0 < 0) a = a + (b - a) * (d0 / (d0 - d1));
                else if (d1 < 0) b = a + (b - a) * (d0 / (d0 - d1));
                nxt[nn++] = a;
                nxt[nn++] = b;
            }
        } else {
            for (int i = 0; i < na && nn < 38; i++) {
                const Vec3& a = cur[i];
                const Vec3& b = cur[(i + 1) % na];
                float da = dot(a - p0, m), db = dot(b - p0, m);
                if (da >= 0) nxt[nn++] = a;
                if ((da >= 0) != (db >= 0)) nxt[nn++] = a + (b - a) * (da / (da - db));
            }
        }
        Vec3* t = cur; cur = nxt; nxt = t;
        na = nn;
    }
    for (int i = 0; i < na; i++) out[i] = cur[i];
    return na;
}

}  // namespace

bool convex_collide(const Convex& A, const Convex& B, float margin, ContactManifold* out) {
    float rA = rounding(A), rB = rounding(B);
    Gjk g;
    gjk(A, B, &g);
    Vec3 n, pa, pb;
    float sep;
    if (g.intersect) {
        float depth;
        if (epa(A, B, g, &n, &depth, &pa, &pb)) {
            sep = -depth - rA - rB;
        } else {
            n = normalize(B.c - A.c);
            if (dot(n, n) < 0.5f) n = Vec3(0, 1, 0);
            pa = A.c; pb = B.c;
            sep = -rA - rB - 1e-3f;
        }
    } else {
        sep = g.dist - rA - rB;
        n = g.n; pa = g.pa; pb = g.pb;
    }
    if (sep >= margin) return false;
    Vec3 pas = pa + n * rA, pbs = pb - n * rB;
    Vec3 single = (pas + pbs) * 0.5f;
    out->n = n;
    out->count = 0;

    Vec3 fa[kFaceMax], fb[kFaceMax];
    int na = support_face(A, n, fa), nb = support_face(B, -n, fb);
    ContactPoint pts[40];
    int np = 0;

    bool poly_a = na >= 3, poly_b = nb >= 3;
    if (poly_a || poly_b) {
        Vec3 nA = poly_a ? poly_normal(fa, na, n) : Vec3();
        Vec3 nB = poly_b ? poly_normal(fb, nb, -n) : Vec3();
        float sa = poly_a ? dot(nA, n) : -1.0f, sb = poly_b ? dot(nB, -n) : -1.0f;
        bool a_ref = sa >= sb;
        float score = a_ref ? sa : sb;
        if (score > 0.3f) {
            const Vec3* ref = a_ref ? fa : fb;
            int nref = a_ref ? na : nb;
            const Vec3* inc = a_ref ? fb : fa;
            int ninc = a_ref ? nb : na;
            Vec3 nr = a_ref ? nA : nB;
            Vec3 clipped[40];
            int nc = clip_to_polygon(inc, ninc, ref, nref, nr, clipped);
            for (int i = 0; i < nc && np < 40; i++) {
                float sp = dot(clipped[i] - ref[0], nr);
                if (sp >= margin) continue;
                pts[np].pos = clipped[i] - nr * (sp * 0.5f);
                pts[np].sep = sp;
                np++;
            }
        }
    } else if (na == 2 && nb == 2) {
        Vec3 da = fa[1] - fa[0], db = fb[1] - fb[0];
        float la = length(da), lb = length(db);
        if (la > 1e-6f && lb > 1e-6f) {
            Vec3 u = da * (1.0f / la);
            Vec3 cr = cross(da, db);
            if (dot(cr, cr) < 0.01f * la * la * lb * lb) {          // parallel: overlap of the two segments
                float ta0 = dot(fa[0], u), ta1 = dot(fa[1], u);
                float tb0 = dot(fb[0], u), tb1 = dot(fb[1], u);
                float alo = ta0 < ta1 ? ta0 : ta1, ahi = ta0 < ta1 ? ta1 : ta0;
                float blo = tb0 < tb1 ? tb0 : tb1, bhi = tb0 < tb1 ? tb1 : tb0;
                float lo = alo > blo ? alo : blo, hi = ahi < bhi ? ahi : bhi;
                if (hi > lo && absf(tb1 - tb0) > 1e-6f) {
                    for (int k = 0; k < 2; k++) {
                        float t = k ? hi : lo;
                        Vec3 qa = fa[0] + u * (t - ta0);
                        Vec3 qb = fb[0] + (fb[1] - fb[0]) * ((t - tb0) / (tb1 - tb0));
                        pts[np].pos = (qa + qb) * 0.5f;
                        pts[np].sep = dot(qb - qa, n);
                        np++;
                    }
                }
            }
        }
    }
    if (np == 0) {
        pts[0].pos = single;
        pts[0].sep = sep;
        np = 1;
    }
    np = reduce4(pts, np);
    out->count = np;
    for (int i = 0; i < np; i++) out->p[i] = pts[i];
    return true;
}

bool convex_overlap(const Convex& A, const Convex& B) {
    Gjk g;
    gjk(A, B, &g);
    if (g.intersect) return true;
    return g.dist < rounding(A) + rounding(B);
}

// --------------------------------------------------------------------- hull

namespace {

struct HF { int v[3]; Vec3 n; float d; bool alive; };

}  // namespace

void hull_free(Hull* h) {
    if (!h) return;
    free(h->v);
    free(h->tri);
    free(h->tn);
    free(h);
}


Hull* hull_build(const Vec3* in, int n) {
    if (n < 1 || !in) return nullptr;
    // Sample very large sets, then drop duplicates.
    int stride = n > 1024 ? (n + 1023) / 1024 : 1;
    Vec3* P = (Vec3*)malloc(sizeof(Vec3) * (size_t)(n / stride + 8));
    int np = 0;
    if (!P) return nullptr;
    Vec3 lo = in[0], hi = in[0];
    for (int i = 0; i < n; i++) { lo = vmin(lo, in[i]); hi = vmax(hi, in[i]); }
    float ext = length(hi - lo);
    float eps = 1e-6f * (ext > 0 ? ext : 1.0f);
    for (int i = 0; i < n; i += stride) {
        bool dup = false;
        for (int j = 0; j < np && !dup; j++) {
            Vec3 e = P[j] - in[i];
            dup = absf(e.x) < eps && absf(e.y) < eps && absf(e.z) < eps;
        }
        if (!dup) P[np++] = in[i];
    }
    Hull* H = nullptr;
    HF* F = nullptr;
    int nf = 0, cap = 0;
    bool ok = np >= 4;
    int i0 = 0, i1 = 0, i2 = 0, i3 = 0;
    if (ok) {
        for (int i = 1; i < np; i++) if (P[i].x < P[i0].x) i0 = i;
        float bd = -1;
        for (int i = 0; i < np; i++) { Vec3 e = P[i] - P[i0]; float d = dot(e, e); if (d > bd) { bd = d; i1 = i; } }
        bd = -1;
        Vec3 e01 = P[i1] - P[i0];
        for (int i = 0; i < np; i++) { Vec3 c = cross(P[i] - P[i0], e01); float d = dot(c, c); if (d > bd) { bd = d; i2 = i; } }
        if (bd <= 1e-12f * ext * ext * ext * ext + 1e-30f) ok = false;
        if (ok) {
            Vec3 nn = normalize(cross(P[i1] - P[i0], P[i2] - P[i0]));
            bd = -1;
            for (int i = 0; i < np; i++) { float d = absf(dot(P[i] - P[i0], nn)); if (d > bd) { bd = d; i3 = i; } }
            if (bd <= 1e-5f * ext) ok = false;
        }
    }
    if (!ok) {
        free(P);
        if (np >= 4 || ext >= 0) {
            // Flat or tiny: the thickened bounding box.
            Vec3 pad(0, 0, 0);
            float m = 0.0005f + 1e-4f * ext;
            Vec3 l = lo - Vec3(m, m, m), h = hi + Vec3(m, m, m);
            (void)pad;
            // Avoid recursion into the degenerate case: this box is not flat.
            Vec3 pts[8];
            for (int i = 0; i < 8; i++) pts[i] = Vec3((i & 1) ? h.x : l.x, (i & 2) ? h.y : l.y, (i & 4) ? h.z : l.z);
            return hull_build(pts, 8);
        }
        return nullptr;
    }
    {
        cap = 64 + np * 4;
        F = (HF*)malloc(sizeof(HF) * (size_t)cap);
        if (!F) { free(P); return nullptr; }
        int ti[4] = { i0, i1, i2, i3 };
        Vec3 cen = (P[i0] + P[i1] + P[i2] + P[i3]) * 0.25f;
        static const int T[4][3] = { { 0, 1, 2 }, { 0, 1, 3 }, { 0, 2, 3 }, { 1, 2, 3 } };
        for (int i = 0; i < 4; i++) {
            HF& f = F[nf++];
            f.v[0] = ti[T[i][0]]; f.v[1] = ti[T[i][1]]; f.v[2] = ti[T[i][2]];
            Vec3 nn = cross(P[f.v[1]] - P[f.v[0]], P[f.v[2]] - P[f.v[0]]);
            if (dot(nn, P[f.v[0]] - cen) < 0) { int t = f.v[1]; f.v[1] = f.v[2]; f.v[2] = t; nn = -nn; }
            f.n = normalize(nn);
            f.d = dot(f.n, P[f.v[0]]);
            f.alive = true;
        }
        int* edges = (int*)malloc(sizeof(int) * 2 * 4096);
        for (int p = 0; p < np && edges; p++) {
            if (p == i0 || p == i1 || p == i2 || p == i3) continue;
            int ne = 0;
            bool any = false;
            for (int i = 0; i < nf; i++) {
                HF& f = F[i];
                if (!f.alive || dot(f.n, P[p]) - f.d <= 1e-6f * ext) continue;
                any = true;
                f.alive = false;
                for (int k = 0; k < 3; k++) {
                    int a = f.v[k], b = f.v[(k + 1) % 3];
                    bool found = false;
                    for (int e = 0; e < ne; e++)
                        if (edges[e * 2] == b && edges[e * 2 + 1] == a) {
                            edges[e * 2] = edges[(ne - 1) * 2];
                            edges[e * 2 + 1] = edges[(ne - 1) * 2 + 1];
                            ne--;
                            found = true;
                            break;
                        }
                    if (!found && ne < 4096) { edges[ne * 2] = a; edges[ne * 2 + 1] = b; ne++; }
                }
            }
            if (!any) continue;
            for (int e = 0; e < ne; e++) {
                int slot = -1;
                for (int i = 0; i < nf; i++) if (!F[i].alive) { slot = i; break; }
                if (slot < 0) {
                    if (nf == cap) {
                        cap *= 2;
                        HF* nf2 = (HF*)realloc(F, sizeof(HF) * (size_t)cap);
                        if (!nf2) break;
                        F = nf2;
                    }
                    slot = nf++;
                }
                HF& f = F[slot];
                f.v[0] = edges[e * 2]; f.v[1] = edges[e * 2 + 1]; f.v[2] = p;
                Vec3 nn = cross(P[f.v[1]] - P[f.v[0]], P[f.v[2]] - P[f.v[0]]);
                float l = length(nn);
                if (l < 1e-20f) { f.alive = false; continue; }
                f.n = nn * (1.0f / l);
                f.d = dot(f.n, P[f.v[0]]);
                f.alive = true;
            }
        }
        free(edges);
    }
    // Collect.
    int* remap = (int*)malloc(sizeof(int) * (size_t)np);
    H = (Hull*)calloc(1, sizeof(Hull));
    if (!H) { free(remap); free(F); free(P); return nullptr; }
    int nt = 0;
    for (int i = 0; i < nf; i++) if (F[i].alive) nt++;
    H->tri = (int*)malloc(sizeof(int) * 3 * (size_t)nt);
    H->tn = (Vec3*)malloc(sizeof(Vec3) * (size_t)nt);
    H->v = (Vec3*)malloc(sizeof(Vec3) * (size_t)np);
    if (!remap || !H->tri || !H->tn || !H->v) { free(remap); free(F); free(P); hull_free(H); return nullptr; }
    for (int i = 0; i < np; i++) remap[i] = -1;
    Vec3 bl(1e30f, 1e30f, 1e30f), bh(-1e30f, -1e30f, -1e30f);
    int nv = 0;
    for (int i = 0; i < nf; i++) {
        if (!F[i].alive) continue;
        for (int k = 0; k < 3; k++) {
            int v = F[i].v[k];
            if (remap[v] < 0) { remap[v] = nv; H->v[nv++] = P[v]; bl = vmin(bl, P[v]); bh = vmax(bh, P[v]); }
            H->tri[H->nt * 3 + k] = remap[v];
        }
        H->tn[H->nt] = F[i].n;
        H->nt++;
    }
    H->nv = nv;
    H->center = (bl + bh) * 0.5f;
    H->half = (bh - bl) * 0.5f;
    for (int i = 0; i < nv; i++) H->v[i] = H->v[i] - H->center;
    free(remap);
    free(F);
    free(P);
    return H;
}

}  // namespace fe
