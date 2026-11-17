#pragma once

namespace gbarecomp {
struct PresentWarpMesh;
}

namespace kol {

struct ScenePixels;
struct RainCam;

// Our own snow in place of the game's flakes (Options > New Snow): at the
// game's snow level (ScenePixels::snow_level), flakes that fall in the
// world, sway, drift with the wind and melt where their landing is seen,
// placed through the warp as our rain is (RainCam), into `mesh`'s over
// triangles. Leaves them alone when there is no snow to draw (the rain's,
// built before it, may be there). Runs on the present worker, once a
// picture, after rain_build.
void snow_build(const ScenePixels& sp, int w, int h, const RainCam& cam,
                gbarecomp::PresentWarpMesh* mesh);
// As RainFrame: a picture made without snow_build has no snow.
struct SnowFrame {
    SnowFrame();
    ~SnowFrame();
};
// Whether the last picture drew our snow: the game's flakes are then left
// out of the frame (kol_obj_cull). A frame late both ways.
bool snow_drawn();
// KOL_SNOW_MOCKUP=1: the game's flakes left out and none of ours drawn, for
// tools/mockup/snow.py's present dumps (with KOL_RAIN_CAM=1 logging the
// camera).
bool snow_mockup();

}  // namespace kol
