#include "fx/sprites.h"
#include "fx/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fe {

SpriteBank::~SpriteBank() {
    free(quads_);
    free(batches_);
    free(labels_);
    free(scan_);
    free(dist_);
}

void SpriteBank::collect(World& w, const Vec3& eye) {
    nb_ = nl_ = 0;
    if (!quads_) {
        quads_ = (gfx::Particle*)malloc(sizeof(gfx::Particle) * kMaxSprites);
        batches_ = (gfx::ParticleBatch*)malloc(sizeof(gfx::ParticleBatch) * kMaxSprites);
        labels_ = (gfx::TextLabel*)malloc(sizeof(gfx::TextLabel) * kMaxSprites);
        dist_ = (float*)malloc(sizeof(float) * 2 * kMaxSprites);
        if (!quads_ || !batches_ || !labels_ || !dist_) return;
    }
    int n = fx_scan(w, ModuleKind::Billboard, &scan_, &scan_cap_);
    for (int i = 0; i < n && nb_ < kMaxSprites; i++) {
        const Billboard* b = (const Billboard*)w.get(scan_[i], ModuleKind::Billboard);
        const WorldTransform* wt = w.world_transform(scan_[i]);
        if (!b || !wt || !b->visible || b->alpha <= 0.003f) continue;
        Vec3 t, s;
        Quat r;
        decompose(wt->matrix, &t, &r, &s);
        float sx = fabsf_(s.x), sy = fabsf_(s.y);
        gfx::Particle& q = quads_[nb_];
        q = gfx::Particle();
        q.pos = t;
        q.size = b->width * (sx > 1e-4f ? sx : 1e-4f);
        q.aspect = b->width > 1e-5f ? b->height * sy / (b->width * (sx > 1e-4f ? sx : 1e-4f)) : 1.0f;
        q.r = b->color.x; q.g = b->color.y; q.b = b->color.z; q.a = b->alpha;
        gfx::ParticleBatch& pb = batches_[nb_];
        pb.items = &q;
        pb.count = 1;
        pb.texture = b->texture[0] ? b->texture : nullptr;
        pb.additive = b->blend == ParticleBlend::Additive;
        Vec3 d = t - eye;
        dist_[nb_] = dot(d, d);
        nb_++;
    }
    // Far first (insertion sort of the batches by distance).
    for (int i = 1; i < nb_; i++) {
        gfx::ParticleBatch pb = batches_[i];
        gfx::Particle q = quads_[i];
        float d = dist_[i];
        int j = i;
        while (j > 0 && dist_[j - 1] < d) { batches_[j] = batches_[j - 1]; quads_[j] = quads_[j - 1]; dist_[j] = dist_[j - 1]; j--; }
        batches_[j] = pb;
        quads_[j] = q;
        dist_[j] = d;
    }
    for (int i = 0; i < nb_; i++) batches_[i].items = &quads_[i];     // the items moved with the sort
    n = fx_scan(w, ModuleKind::Text3D, &scan_, &scan_cap_);
    for (int i = 0; i < n && nl_ < kMaxSprites; i++) {
        const Text3D* t3 = (const Text3D*)w.get(scan_[i], ModuleKind::Text3D);
        const WorldTransform* wt = w.world_transform(scan_[i]);
        if (!t3 || !wt || !t3->visible || !t3->text[0]) continue;
        Vec3 t, s;
        Quat r;
        decompose(wt->matrix, &t, &r, &s);
        gfx::TextLabel& l = labels_[nl_++];
        l = gfx::TextLabel();
        l.pos = t;
        l.height = t3->size * (fabsf_(s.y) > 1e-4f ? fabsf_(s.y) : 1e-4f);
        l.r = t3->color.x; l.g = t3->color.y; l.b = t3->color.z; l.a = 1;
        l.align = (uint8_t)t3->align == 0 ? 0 : ((uint8_t)t3->align == 2 ? 2 : 1);
        l.outline = t3->outline;
        snprintf(l.text, sizeof l.text, "%s", t3->text);
    }
}

}  // namespace fe
