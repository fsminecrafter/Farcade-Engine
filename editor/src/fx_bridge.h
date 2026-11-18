// The editor side of the fx modules (engine/fx): Play runs them as systems on
// the play world (FxWorld); in edit mode the selected emitters are simulated
// as a preview. Their pictures (particles, billboards, text) and the gizmos of
// the modules are drawn over the viewport image with ImGui, so the 3D image
// need not be redrawn every frame.
#pragma once

#include "core/math.h"
#include "ecs/ecs.h"
#include "scene.h"

struct ImDrawList;

namespace fe {

class Editor;
class FxWorld;
class ParticleBank;

class FxBridge {
public:
    FxBridge();
    ~FxBridge();
    FxBridge(const FxBridge&) = delete;
    FxBridge& operator=(const FxBridge&) = delete;

    // The fx systems of the running Play session (PlaySession::fx), as of the
    // last frame; null in edit mode.
    FxWorld* world() { return fx_; }

    // Edit mode, once per frame: simulates the selected Particle Emitters.
    void update_preview(Editor& ed, float dt);

    // Over the viewport image: particles, billboards, text, and the selected
    // objects' module gizmos. view/proj as drawn, `pos`/`size` the viewport
    // rectangle on screen, `eye` the camera position.
    void draw_overlay(Editor& ed, ImDrawList* dl, const Mat4& view, const Mat4& proj, float x, float y, float w, float h, const Vec3& eye);

    // Counts of what the last overlay drew (tests).
    int last_particles = 0;
    int last_sprites = 0;
    int preview_emitters() const;

private:
    FxWorld* fx_ = nullptr;
    ParticleBank* preview_ = nullptr;
    double preview_time_ = 0;
};

}  // namespace fe
