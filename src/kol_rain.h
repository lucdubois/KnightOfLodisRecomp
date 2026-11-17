#pragma once

namespace gbarecomp {
struct PresentWarpMesh;
}

namespace kol {

struct ScenePixels;

// Where the warp puts a point of the world (kol_perspective corner_at), for
// our own rain: X - Z and X + Z (`e`, `d`) and its height Y land on the
// scene's column col0 + e / 2 and, in perspective, are scaled about the
// look by s = K / (K + dref - d) and pitched about the reference; `flat`
// (the light without the perspective) keeps s = 1.
struct RainCam {
    float base = 0.0f;     // the scene row of the place (0, 0): row = base + d / 4 - Y / 2
    float col0 = 0.0f;     // the scene column of X - Z = 0 (ox + kx)
    float dref = 0.0f, yref = 0.0f, K = 1.0f, pitch = 1.0f, zoom = 1.0f;
    float cx = 0.0f, cy = 0.0f, cxp = 0.0f;
    int vx = 0, vy = 0, vw = 0, vh = 0;   // the view in the scene
    bool flat = false;
};

// Our own rain in place of the game's (Options > New Rain): at the game's
// intensity (ScenePixels::rain_level), drops that fall in the world, are
// placed through the warp and splash where their landing is seen, into
// `mesh`'s over triangles (gbarecomp PresentWarpMesh::over_xy). The scene's
// buffers are w x h. Clears them when there is no rain to draw. Runs on the
// present worker, once a picture.
void rain_build(const ScenePixels& sp, int w, int h, const RainCam& cam,
                gbarecomp::PresentWarpMesh* mesh);
// Held over a provider's call: a picture it made without rain_build (it
// bailed out) has no rain. The flag changes only at the end, since the game's
// frame is composed while the worker runs.
struct RainFrame {
    RainFrame();
    ~RainFrame();
};
// Whether the last picture drew our rain: the game's drops are then left out
// of the frame (kol_obj_cull). A frame late both ways.
bool rain_drawn();
// KOL_RAIN_MOCKUP=1: the game's drops left out and none of ours drawn, for
// tools/mockup/rain.py's present dumps (with KOL_RAIN_CAM=1 logging the
// camera).
bool rain_mockup();

}  // namespace kol
