#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#define REMIX_ALLOW_X86
#include <remix/remix_c.h>
#include <remix/remix_bake_ext.h>

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
// A texture's mean colour and alpha as stored (0..1; sRGB values for colour
// textures), from its VTF. False for a missing file or a format not read.
bool textureMeanColor(const std::string &name, float rgba[4]);
// A texture's largest mip (first frame) as RGBA8, R in the lowest byte, from
// its VTF. False for a missing file or a format not read.
bool textureRgba(const std::string &name, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba);

// Loads `maps/<mapName>.bsp` from the search paths; an empty name unloads.
// Returns whether the map's world is now provided from the BSP. The host
// enables styled lights only when it can supply current engine lightstyles.
bool load(const remixapi_Interface &api, const std::string &mapName, bool lightStylesAvailable = false,
          bool decodeSkybox = false);
struct SkyboxFace {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  // HDR sky materials are decoded before interpolation; no display transform.
  std::vector<float> linearRgba;
};
struct SkyboxImages {
  std::string name, error;
  // Source world axes: +X, -X, +Y, -Y, +Z, -Z.
  std::array<SkyboxFace, 6> faces;
};
const SkyboxImages &skyboxImages();

// Offline replay uses the same Source search paths and VTF decoder as the game.
// Animated images are refused unless the diagnostic host explicitly requests
// a frozen frame-zero snapshot. This is not engine animation playback.
bool textureImage(const std::string &name, uint32_t &width, uint32_t &height,
                  std::vector<uint8_t> &rgba, std::string &error, bool freezeAnimation = false);
bool materialSource(uint64_t textureHash, std::string &baseTexture);
// Legacy XXH64 replacement key for the same upload bytes/mip as a current
// texture candidate. Offline hosts need this because decoded DDSs have new IDs.
uint64_t textureReplacementHash(const std::string &name, uint64_t sourceHash);
bool playerStart(float origin[3], float angles[3]);

// The BSP world is loaded: world draws are dropped.
bool active();

// The BSP world draws the map's overlays: engine overlay draws are dropped.
bool overlaysActive();

// Resolves materials, builds pending meshes and draws this frame's instances.
void submit(MaterialLookup lookup);

// Publish the complete retained BSP scene for load-time baking. Static map
// lights and the captured sky use ordinary handles. Stationary styled lights
// provide separate fully-on authored handles and live identities; never dlights/flashlights. The native runtime captures inputs once per map/material revision.
// Diagnostic hosts may override the probe domain; ordinary gameplay uses the full map.
void publishBakeScene(const std::vector<remixapi_LightHandle> &lights,
                      const std::vector<remixapi_BakeStyledLightEXT> &styled = {},
                      const std::array<float, 6> *probeBounds = nullptr);

// Whether a draw's model-to-world rows (3x4, as in vertex shader c58) place
// one of the map's static props, which the BSP world draws itself. eye: the
// view's camera; props the engine draws from inside the 3D skybox are left
// to it from then on.
bool isStaticProp(const float *modelRows, const float eye[3]);

// Whether a triangle (model-space corners) lies on corners of the func_lod
// brushes the BSP world bakes: the engine's draw of one, which goes.
bool isBakedBrushEntity(const float *a, const float *b, const float *c);

// A static light of the map, in the engine's LightDesc_t terms (type 1 point,
// 2 directional, 3 spot, internal 4 surface; color in Source intensity units).
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
