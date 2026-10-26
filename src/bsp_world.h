#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#define REMIX_ALLOW_X86
#include <remix/remix_c.h>

#include "engine_materials.h"

// The map's static world (world brushes and displacements of the BSP's world
// model), built once from the map file and submitted every frame as static
// instances. The engine's own world draws are then dropped (see
// openremix_proxy_world_lists): the whole map stays in the scene regardless
// of Source's visibility, and the per-frame capture cost goes away.
namespace proxy::bsp {

struct MaterialRef {
  remixapi_MaterialHandle handle = nullptr;
  std::shared_ptr<void> keep; // keeps the material (and its texture) alive
};
// The material the proxy made for an uploaded texture with this dxvk hash
// (null handle: not uploaded yet). emission (may be null): its unlit or self-
// illuminated variant, masked by the first uploaded texture among
// maskCandidates (null handle while none of them is).
using MaterialLookup = MaterialRef (*)(uint64_t textureHash, const engine_materials::Shading *emission,
                                       const std::vector<uint64_t> *maskCandidates, bool nearest);

// dxvk hashes a texture ("models/x/y", as materials name it) can be uploaded
// under: its largest mips, from the VTF file. Empty: no file, or a format the
// proxy does not upload. frame: of an animated texture (Source uploads each
// as its own texture). Render thread.
std::vector<uint64_t> textureCandidates(const std::string &name, uint32_t frame = 0);
// The water the eye is in (the map's leaves), as the runtime's camera medium:
// its colour (linear) and measurement distance. False: in air, or unknown.
bool waterAt(const float eye[3], float color[3], float &distance);
// A texture's mean luminance as stored (0..1, gamma; its smallest mip), or
// -1 when its file is missing or in a format not read. Cached by name.
float textureLuminance(const std::string &name);

// Loads `maps/<mapName>.bsp` from the search paths; an empty name unloads.
// Returns whether the map's world is now provided from the BSP.
bool load(const remixapi_Interface &api, const std::string &mapName);

// The BSP world is loaded: world draws are dropped.
bool active();

// The BSP world draws the map's overlays: engine overlay draws are dropped.
bool overlaysActive();

// Resolves materials, builds pending meshes and draws this frame's instances.
void submit(MaterialLookup lookup);

// Whether a draw's model-to-world rows (3x4, as in vertex shader c58) place
// one of the map's static props, which the BSP world draws itself. eye: the
// view's camera; props the engine draws from inside the 3D skybox are left
// to it from then on.
bool isStaticProp(const float *modelRows, const float eye[3]);

// Whether a triangle (model-space corners) lies on corners of the func_lod
// brushes the BSP world bakes: the engine's draw of one, which goes.
bool isBakedBrushEntity(const float *a, const float *b, const float *c);

// A static light of the map, in the engine's LightDesc_t terms (type 1 point,
// 2 directional, 3 spot; color in Source's linear intensity units).
struct Light {
  int type = 0;
  float color[3] = {}, position[3] = {}, direction[3] = {};
  float attenuation[3] = {}; // constant, linear, quadratic
  float thetaDot = 0, phiDot = 0; // spot inner and outer cone cosines
  uint64_t hash = 0;
  int style = 0; // lightstyle scaling the color each frame (0: none)
};
// The map's static lights, submitted every frame.
const std::vector<Light> &lights();

// Whether a light the engine passes to a model is one of lights() (matched
// by position, or by direction for directional lights).
bool providesLight(int type, const float position[3], const float direction[3]);

// Depth occluders for the game's occlusion queries (light glows, the sun):
// the map's opaque faces and static props as a world-space triangle list.
// version changes with the map.
struct Occluders {
  std::vector<float> positions; // xyz per vertex
  std::vector<uint32_t> indices;
  uint64_t version = 0;
};
const Occluders &occluders();

// Whether the BSP world draws the static prop at this placement (3x4 model-
// to-world rows), so the engine need not. Thread-safe. Not claimed: 3D
// skybox props, models that failed to load, and models with translucent or
// ignored meshes (the engine still draws those).
bool providesProp(const float *rows);

// The map's sky_camera (3D skybox origin and scale), if any.
bool skyCamera(float origin[3], float &scale);

// The 3D skybox is path traced in world space (its engine sky-view draws are
// redundant): the distance its geometry reaches from the playable map's
// centre, or 0 when it is rasterized by the sky camera.
float skyTracedReach();

// The BSP material using a texture (for statistics), or empty.
std::string materialName(uint64_t textureHash);

void dumpStatistics(FILE *out);

} // namespace proxy::bsp
