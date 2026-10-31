#include "source_lights.h"
#include "source_rendering.h"
#include <remix/remix_reflection_ext.h>
#include "styled_lights.h"
#include "diag.h"
#include "frame_profile.h"
#include <map>
#include "remix_output.h"
#include "d3d_hooks.h"
#include "remix_ui.h"
#include "sky_faces.h"
#include "bsp_world.h"
#include "bake_sky.h"
#include "device_window.h"
#include "dxvk_hash.h"
#include "engine_lights.h"
#include "engine_nocull.h"
#include "engine_visibility.h"
#include "engine_materials.h"
#include "host_passes.h"
#include "particles.h"
#include "texture_categories.h"
#include "vb_origins.h"
#include <unordered_set>

#include "texture_formats.h"
#define XXH_INLINE_ALL
#include "xxhash.h"

#define REMIX_ALLOW_X86
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <remix/remix_bake_ext.h>
#include <remix/remix_usd_ext.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace proxy::remix {
namespace {

// Source vertex shader registers (common_vs_fxc.h layout).
constexpr unsigned kRegModelViewProj = 4; // c4..c7: model * view * projection (columns)
constexpr unsigned kRegViewProj = 8;  // c8..c11: view * projection
constexpr unsigned kRegModel = 58;    // c58..c60: model (object to world) 3x4 rows
constexpr unsigned kRegEyeIrisU = 50; // c50, c51: Eyes shader iris projection U, V (world position -> iris texture)
constexpr unsigned kRegFlexScale = 3; // c3.x: 1 while a flex mesh is bound to stream 2, else 0 (cFlexScale)
constexpr unsigned kRegAmbientUp = 25; // c21..c26: ambient cube +X -X +Y -Y +Z -Z (linear)
// MaterialMatrixMode_t
constexpr int kMatrixView = 0;
constexpr int kMatrixProjection = 1;
// IShaderAPI (ShaderApi030) slot 12: void GetMatrix(MaterialMatrixMode_t, float *dst)
constexpr unsigned kSlotGetMatrix = 12;
// IShaderAPI slot 34: int GetCurrentNumBones() const
constexpr unsigned kSlotGetCurrentNumBones = 34;
// IShaderDynamicAPI: MaterialFogMode_t GetSceneFogMode(), void
// GetSceneFogColor(unsigned char *rgb); IShaderAPI: void GetFogDistances(float
// *start, float *end, float *fogZ). (tools/sdk_vtables64.py, like the above.)
constexpr unsigned kSlotGetSceneFogMode = 4, kSlotGetSceneFogColor = 5, kSlotGetFogDistances = 213;
constexpr unsigned kMaxBones = 53; // cModel[53] starting at c58

remixapi_Interface g_api = {};
bool g_ready = false;
HWND g_gameWindow = nullptr;
HWND g_hiddenWindow = nullptr;
void *g_shaderApi = nullptr;
uint64_t g_frame = 0;

float g_modulationSample[4] = {}; // the last tinted draw's modulation colour (g_transientSampleLock)

struct Stats {
  std::atomic<uint32_t> draws{0}, accepted{0}, offTarget{0}, noPosition{0}, noData{0}, skinned{0}, badPrim{0},
      badRange{0}, cacheHits{0}, created{0}, transient{0}, evicted{0}, submitted{0}, frames{0}, nonFinite{0}, rejected{0}, flat{0}, uiDraws{0}, uiNoTexture{0}, depthOnly{0}, additive{0}, blended{0}, duplicate{0}, skyFrames{0}, lights{0}, playerBody{0}, textures{0}, textureSkipped{0}, ignored{0}, bspDropped{0}, bspKeptBlended{0}, bspKeptDynamic{0}, bspPropsDropped{0}, bspOverlaysDropped{0}, bspBrushEntitiesDropped{0}, skyViewDemoted{0}, flashlightPasses{0}, worldViewCameras{0}, uiStencil{0}, additiveBlack{0}, haloSources{0}, haloGroups{0}, maskGroups{0}, maskSources{0}, maskOverlays{0}, maskUi{0}, maskCuts{0}, emissiveNotItsTexture{0}, rigidModels{0}, projectedCopies{0}, lightCookies{0}, lightCookieFrameMax{0}, shatteredSolid{0}, shatteredEdges{0}, shatteredTinted{0}, shatteredTextures{0}, opaqueTranslucent{0}, screenDraws{0}, screenInstances{0}, screenViewsFull{0}, flashlightLitPasses{0}, cameraCulledModels{0};
  std::atomic<uint32_t> setLightKept[4] = {};
  // Unlit and self-illuminated draws (engine_materials): by kind, and why some stayed lit.
  std::atomic<uint32_t> luaLitDraws{0}; // unlit Lua meshes lit as surfaces (onDraw)
  std::atomic<uint32_t> unlitDraws{0}, selfIllumDraws{0}, selfIllumMaskDraws{0}, selfIllumMaskAlphaDraws{0},
      emissiveLightDraws{0}, unlitVertexColorDraws{0}, modulatedDraws{0}, wireframeDraws{0}, modulatedBlack{0}, modulatedMasked{0}, modulatedMetal{0}, emissiveNoTexture{0}, emissiveMaskNoHash{0},
      emissiveMaskNotUploaded{0}, emissiveCreated{0}, emissiveFailed{0}, emissiveEvicted{0};
  // PBR conversions (pbr_rules): draws with one, and why some stayed diffuse.
  std::atomic<uint32_t> underwaterFrames{0}, refractOverlaysDropped{0};
  std::atomic<uint32_t> pbrDraws{0}, pbrNormalDraws{0}, pbrMapDraws{0}, pbrMetalDraws{0}, pbrNotBase{0}, pbrMapsMissing{0}, pbrCreated{0};
  // Flexed draws (facial animation, see flexScale): rebuilt this frame, and flex streams whose scale was 0.
  std::atomic<uint32_t> flexed{0}, flexUnscaled{0}, flexRefreshed{0};
  // Eye draws (the Eyes shader), and the iris overlays made for them.
  std::atomic<uint32_t> eyes{0}, eyeIrises{0}, eyeRefracts{0};
  // Rope draws (the Cable shader): with their base texture found by name, and without.
  std::atomic<uint32_t> cableDraws{0}, cableNoBase{0};
  // Decals (onDraw): by kind (DecalKind order), by what marked them, and how
  // they were lifted off their receiver.
  std::atomic<uint32_t> decalKinds[5] = {};
  std::atomic<uint32_t> decalByFlag{0}, decalByShader{0}, decalByBias{0}, decalByCategory{0}, decalBiasOnly{0},
      decalMaterialOnly{0}, decalWorld{0}, decalSkinned{0}, decalVertexNormals{0}, decalFaceNormals{0},
      decalVertexColors{0}, decalModulateFade{0}, decalOtherBlend{0}, decalNotItsBlend{0}, decalNoMesh{0};
  // Winding of the meshes built with vertex normals under D3DCULL_CCW: their
  // first triangle's e1 x e2 along / against its vertex normals (since launch).
  std::atomic<uint32_t> windingAlong{0}, windingAgainst{0};
} g_stats;

// CPU time accounting (microseconds), reset with the statistics.
struct Timer {
  LARGE_INTEGER start;
  std::atomic<uint32_t> &sink; // 32-bit: a 64-bit atomic add is a locked cmpxchg8b loop on x86
  std::atomic<uint32_t> *total; // never reset (wraps): per-frame differences for logFrameSpike
  explicit Timer(std::atomic<uint32_t> &s, std::atomic<uint32_t> *t = nullptr) : sink(s), total(t) {
    QueryPerformanceCounter(&start);
  }
  ~Timer() {
    static const double usPerTick = [] {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return 1e6 / double(f.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const auto us = uint32_t(double(now.QuadPart - start.QuadPart) * usPerTick);
    sink.fetch_add(us, std::memory_order_relaxed);
    if (total)
      total->fetch_add(us, std::memory_order_relaxed);
  }
};
std::atomic<uint64_t> g_transientVerts{0}, g_transientIndices{0};
std::mutex g_transientSampleLock;
std::unordered_map<const void *, uint32_t> g_transientDecls; // declaration -> transient draws
// Microseconds per statistics interval (10 s).
std::atomic<uint32_t> g_usDraw{0}, g_usBuild{0}, g_usSubmit{0}, g_usPresent{0}, g_usFrame{0}, g_usBsp{0};
LARGE_INTEGER g_lastPresent = {};
// For logFrameSpike: the same timers without the statistics reset (draw,
// mesh build, present submit, BSP world, Present call), the last frame's
// length, and when the statistics thread's dump began and ended (ms ticks).
std::atomic<uint32_t> g_totalUs[5] = {};
double g_frameIntervalMs = 0;
std::atomic<uint64_t> g_statisticsBegan{0}, g_statisticsEnded{0};
std::atomic<int64_t> g_frameBegan{0}; // QueryPerformanceCounter at the last onPresent
std::atomic<unsigned long> g_frameThread{0};

// Keys for this session's caches and identities (draw state, views, lights).
// (Formerly FNV-1a, byte by byte: a hot spot at ~1200 draws per frame.)
uint64_t fnv(const void *data, size_t size, uint64_t h = 1469598103934665603ull) {
  return XXH3_64bits_withSeed(data, size, h);
}

struct CachedMesh {
  remixapi_MeshHandle handle = nullptr;
  uint64_t lastFrame = 0;
  uint32_t bones = 0; // skinned: bone matrices the mesh indexes (0 = rigid)
  uint64_t geometry = 0; // dxvk::geometryHash of the draw that built it
};
std::unordered_map<uint64_t, CachedMesh> g_meshes;
// Flexed meshes (faces, see flexScale): one runtime mesh per draw whose
// vertices are refreshed in place when the flex deltas change
// (remixapi_MeshInfoDeformEXT), so the face stays the same mesh and
// the same instance from frame to frame: motion vectors and denoiser history,
// which a new mesh every frame did not have (a talking face smeared).
struct FlexMesh {
  remixapi_MeshHandle handle = nullptr;
  uint64_t lastFrame = 0;
  uint64_t content = 0; // the flex deltas it was built from (0: at rest)
  uint32_t bones = 0;
  uint64_t geometry = 0;
};
std::unordered_map<uint64_t, FlexMesh> g_flexMeshes;
// Diagnostics: cached (static-buffer) meshes built anew, by the material bound
// at the time, with the cache key of the last two builds. A mesh that is
// rebuilt every frame is a new mesh, and a new instance, every frame: it has
// no motion vectors. The key field that differs between the two says why.
struct MeshBuilds {
  uint32_t count = 0;
  uint64_t last[13] = {}, previous[13] = {};
};
std::unordered_map<std::string, MeshBuilds> g_meshBuilds; // under g_transientSampleLock
std::vector<remixapi_MeshHandle> g_transient; // destroyed after Present

// --- Textures and materials ------------------------------------------------
// One Remix texture per uploaded D3D texture (level 0), shared by textures
// that receive it through UpdateTexture. Its material is created on first draw.
struct RemixTexture {
  remixapi_TextureHandle texture = nullptr;
  remixapi_MaterialHandle material = nullptr;
  remixapi_MaterialHandle pointMaterial = nullptr; // nearest filter variant (pixel-art)
  remixapi_MaterialHandle skyMaterial = nullptr; // tinted by rtx.skyBrightness
  remixapi_MaterialHandle clampMaterial = nullptr; // clamp-to-edge variant (an eye's iris)
  remixapi_MaterialHandle pointClampMaterial = nullptr; // nearest clamp variant
  float skyBrightness = 0;
  uint64_t hash = 0;
  // dxvk::legacyTextureHash of the upload (0: none): a second key mods may
  // replace its material by (remixapi_MaterialInfoReplacementHashEXT).
  uint64_t legacyHash = 0;
  // An X8R8G8B8 upload's hash with X made 0xff (0: none), a second key in
  // g_texturesByHash: the engine leaves X unwritten in some of its uploads.
  uint64_t opaqueHash = 0;
  std::vector<uint64_t> emissive; // its unlit / self-illuminated variants (g_emissiveMaterials keys)
  // A render-target texture's contents (onTargetPixels): a dynamic runtime
  // texture of this size, rewritten in place (remixapi_UpdateTextureEXT).
  // `hash` is then an identity, not a content hash, and stays for the life
  // of the record, as do the materials made from it.
  bool dynamic = false;
  uint32_t width = 0, height = 0;
  ~RemixTexture();
};
// Recursive: a texture record can be released, and its destructor run, while
// the lock is held.
std::recursive_mutex g_textureLock;
std::unordered_map<const void *, std::shared_ptr<RemixTexture>> g_textures;
// D3D textures with identical contents share one record: the dxvk hash names
// the runtime texture and material, and mods key material replacements by it.
std::unordered_map<uint64_t, std::weak_ptr<RemixTexture>> g_texturesByHash;
std::wstring g_moduleDir; // directory of the game executable

// CPU copies of small textures (level 0, tightly packed native format) for UI
// drawing, which needs RGBA8 pixels. Font pages are filled through partial
// updates, which are applied here as well.
struct CpuTexture {
  uint32_t width = 0, height = 0;
  D3DFORMAT format = D3DFMT_UNKNOWN;
  std::vector<uint8_t> data;
  uint32_t version = 0;
  uint32_t registeredVersion = 0; // UI texture contents last registered (0 = never)
  // Texels changed since registration (x0, y0, x1, y1; empty: x1 <= x0):
  // forwarded as a region update (Chromium repaints dirty rectangles).
  uint32_t dirty[4] = {};
  void touch(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    x1 = std::min(x1, width);
    y1 = std::min(y1, height);
    if (x1 <= x0 || y1 <= y0)
      return;
    if (dirty[2] <= dirty[0] || dirty[3] <= dirty[1]) {
      dirty[0] = x0, dirty[1] = y0, dirty[2] = x1, dirty[3] = y1;
      return;
    }
    dirty[0] = std::min(dirty[0], x0), dirty[1] = std::min(dirty[1], y0);
    dirty[2] = std::max(dirty[2], x1), dirty[3] = std::max(dirty[3], y1);
  }
};
std::unordered_map<const void *, std::shared_ptr<CpuTexture>> g_cpuTextures;

// Bytes per row (or per 4-row block row) and row count of a level.
bool levelLayout(D3DFORMAT format, uint32_t width, uint32_t height, uint32_t &rowBytes, uint32_t &rows) {
  const uint32_t bw = std::max(1u, (width + 3) / 4), bh = std::max(1u, (height + 3) / 4);
  switch (format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
    rowBytes = width * 4;
    rows = height;
    return true;
  case D3DFMT_DXT1:
    rowBytes = bw * 8;
    rows = bh;
    return true;
  case D3DFMT_DXT5:
    rowBytes = bw * 16;
    rows = bh;
    return true;
  default:
    return false;
  }
}

// CPU copies feed UI textures (RegisterUITexture). GMod's Chromium panels
// (main menu, DHTML) are A8R8G8B8 textures of up to 2048x2048 or more.
bool keepCpuCopy(D3DFORMAT format, uint32_t width, uint32_t height) {
  const bool wide = format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8;
  return uint64_t(width) * height <= (wide ? 4096u * 4096u : 512u * 512u);
}

// dxvk-remix variables the proxy implements itself (l4d2-rtx map configs).
std::atomic<float> g_skyBrightness{1.0f};

// The dxvk-remix hash of a D3D texture's contents (0: none, as for dxvk-remix).
uint64_t textureHash(const void *texture) {
  if (!texture)
    return 0;
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  auto it = g_textures.find(texture);
  return it == g_textures.end() ? 0 : it->second->hash;
}

// Dev aid (OPENREMIX_PROXY_DUMP_KEYS=1): every mesh replacement key drawn is
// appended once to bin/replacement_keys.txt, for matching against mods.
void noteReplacementKey(uint64_t key, char kind = 'M') {
  static const bool dump = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_DUMP_KEYS", v, sizeof(v)) && v[0] == '1';
  }();
  if (!dump || !key)
    return;
  static std::mutex lock;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard<std::mutex> guard(lock);
  if (!seen.insert(key).second)
    return;
  if (FILE *f = _wfopen((g_moduleDir + L"\\replacement_keys.txt").c_str(), L"a")) {
    fprintf(f, "%c %016llX\n", kind, static_cast<unsigned long long>(key));
    fclose(f);
  }
}

// Whether a D3D9 sampler state requests point (nearest) filtering (e.g. for pixel-art).
bool isPointSampler(const DWORD (*samplerStates)[16], uint32_t sampler = 0) {
  if (!samplerStates || sampler >= 16)
    return false;
  const DWORD mag = samplerStates[sampler][D3DSAMP_MAGFILTER];
  const DWORD min = samplerStates[sampler][D3DSAMP_MINFILTER];
  return mag == D3DTEXF_POINT || min == D3DTEXF_POINT;
}

// `skyBrightness` > 0 selects the sky variant, whose tint scales the texture.
// Caller holds g_textureLock.
remixapi_MaterialHandle ensureMaterial(RemixTexture &t, float skyBrightness, bool nearest = false) {
  const bool sky = skyBrightness > 0;
  if (sky && t.skyMaterial && t.skyBrightness != skyBrightness) {
    g_api.DestroyMaterial(t.skyMaterial);
    t.skyMaterial = nullptr;
  }
  remixapi_MaterialHandle &slot = sky ? t.skyMaterial : (nearest ? t.pointMaterial : t.material);
  if (!slot) {
    wchar_t path[32];
    swprintf(path, 32, L"0x%llX", static_cast<unsigned long long>(t.hash));
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.roughnessConstant = 1; // A plain Source surface has no authored glossy lobe.
    // The runtime multiplies texture gamma values by the tint (then linearizes),
    // so a linear brightness B is B^(1/2.2).
    const float tint = sky ? std::pow(skyBrightness, 1.0f / 2.2f) : 1.0f;
    opaque.albedoConstant = {tint, tint, tint};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7; // always pass (0 would discard everything)
    opaque.useDrawCallAlphaState = 1; // alpha test/blend come from each draw (InstanceInfoBlendEXT)
    remixapi_MaterialInfoReplacementHashEXT replacement = {};
    replacement.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_REPLACEMENT_HASH_EXT;
    replacement.materialHash = t.legacyHash;
    remixapi_MaterialInfoPbrEXT diffuse = {};
    diffuse.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
    opaque.pNext = &diffuse; // Preserve the path tracer's diffuse-only fallback.
    if (t.legacyHash && !sky)
      diffuse.pNext = &replacement;
    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = sky ? t.hash ^ 0x5ca1ab1e5ca1ab1eull : (nearest ? (t.hash ^ 0x701475a1701475a1ull ? t.hash ^ 0x701475a1701475a1ull : ~t.hash) : t.hash);
    info.albedoTexture = path;
    info.filterMode = nearest ? 0 : 1; // linear (0 is nearest)
    info.wrapModeU = info.wrapModeV = 1;
    if (g_api.CreateMaterial(&info, &slot) != REMIXAPI_ERROR_CODE_SUCCESS)
      slot = nullptr;
    if (sky)
      t.skyBrightness = skyBrightness;
  }
  return slot;
}

// Screen cameras (remixapi_CameraScreensEXT): views the engine draws a world
// into a render-target texture (render.RenderView into an RT camera's target,
// at its "world_lists" marker), by that texture. A draw of the main view that
// samples one (the camera's monitor) gets a screen material showing its slot:
// the runtime traces what the camera sees there, instead of the texture's
// rasterized contents. A view not drawn again for kScreenFrames frames frees
// its slot.
struct ScreenView {
  float view[16], projection[16];
  uint64_t frame = 0;
  uint32_t slot = 0;
};
std::unordered_map<const void *, ScreenView> g_screenViews; // render thread
// During a screen draw (onDraw): its base texture transform, two rows taking
// (u, v, 0, 1) to the coordinates the shader samples with (Source's vertex
// shaders take $basetexturetransform in c48 and c49: a func_monitor's
// "gmod fix" scales its brush coordinates by 2 about the centre). The mesh's
// coordinates are made those (buildMesh), so they name the camera's image.
const float *g_screenUv = nullptr;
// Screen-view instances left out (onPresent), by reason: raster only, blended
// in world space, stencil, placed already; with a texture of each.
uint64_t g_screenSkipped[4] = {};
std::string g_screenSkippedExample[4];
// The last screen draw's coordinates as stored and as made (statistics).
float g_screenUvRange[8] = {1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f, 1e30f, -1e30f}; // raw u, v; made u, v
float g_screenUvRows[8] = {};
std::string g_screenMaterial;
remixapi_MaterialHandle g_screenMaterials[REMIXAPI_SCREEN_CAMERA_SLOTS_EXT] = {};
constexpr uint64_t kScreenFrames = 30;

// At a "world_lists 1" marker drawn into a render-target texture: that view's
// matrices (left-handed, as world views are stored).
void noteScreenView(const void *target, const float view[16], const float projection[16]) {
  auto it = g_screenViews.find(target);
  if (it == g_screenViews.end()) {
    bool used[REMIXAPI_SCREEN_CAMERA_SLOTS_EXT] = {};
    for (const auto &[texture, v] : g_screenViews)
      used[v.slot] = true;
    uint32_t slot = 0;
    while (slot < REMIXAPI_SCREEN_CAMERA_SLOTS_EXT && used[slot])
      ++slot;
    if (slot == REMIXAPI_SCREEN_CAMERA_SLOTS_EXT) {
      ++g_stats.screenViewsFull;
      return;
    }
    it = g_screenViews.emplace(target, ScreenView{}).first;
    it->second.slot = slot;
  }
  std::memcpy(it->second.view, view, sizeof(it->second.view));
  std::memcpy(it->second.projection, projection, sizeof(it->second.projection));
  it->second.frame = g_frame;
}

// The screen slot of a render-target texture a view was drawn into lately (-1: none).
int screenSlot(const void *texture) {
  const auto it = texture ? g_screenViews.find(texture) : g_screenViews.end();
  return it != g_screenViews.end() && g_frame - it->second.frame <= kScreenFrames ? int(it->second.slot) : -1;
}

// The material of a screen showing `slot`: black where the camera is not
// traced (a reflection of the screen, no raster primary).
remixapi_MaterialHandle screenMaterial(int slot) {
  remixapi_MaterialHandle &handle = g_screenMaterials[slot];
  if (!handle) {
    remixapi_MaterialInfoScreenCameraEXT screen = {};
    screen.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_SCREEN_CAMERA_EXT;
    screen.slot = uint32_t(slot);
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = &screen;
    opaque.albedoConstant = {.02f, .02f, .02f};
    opaque.opacityConstant = 1;
    opaque.roughnessConstant = 1;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = 0x5c4ee7ca3e4a0000ull + uint64_t(slot);
    if (g_api.CreateMaterial(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS)
      handle = nullptr;
  }
  return handle;
}

// This frame's screen cameras, for the world camera; forgets old views.
std::vector<remixapi_ScreenCameraEXT> screenCameras() {
  std::vector<remixapi_ScreenCameraEXT> out;
  for (auto it = g_screenViews.begin(); it != g_screenViews.end();) {
    if (g_frame - it->second.frame > kScreenFrames) {
      it = g_screenViews.erase(it);
      continue;
    }
    const float *v = it->second.view, *p = it->second.projection;
    remixapi_ScreenCameraEXT c = {};
    c.slot = it->second.slot;
    float eye[3];
    for (int i = 0; i < 3; ++i)
      eye[i] = -(v[12] * v[i * 4] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
    c.position = {eye[0], eye[1], eye[2]};
    c.right = {v[0], v[4], v[8]};
    c.up = {v[1], v[5], v[9]};
    c.forward = {v[2], v[6], v[10]};
    c.fovYDegrees = p[5] > 0 ? 2 * std::atan(1 / p[5]) * 180 / 3.14159265f : 0;
    c.aspect = p[0] > 0 ? p[5] / p[0] : 0;
    if (c.fovYDegrees > 0 && c.fovYDegrees < 180 && c.aspect > 0)
      out.push_back(c);
    ++it;
  }
  return out;
}

remixapi_MaterialHandle materialFor(const void *texture, float skyBrightness = 0, bool nearest = false) {
  if (const int slot = screenSlot(texture); slot >= 0)
    return screenMaterial(slot);
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  auto it = g_textures.find(texture);
  return it == g_textures.end() ? nullptr : ensureMaterial(*it->second, skyBrightness, nearest);
}

float envScale(const char *name, float fallback);

// --- Unlit and self-illuminated materials -------------------------------------
// Source's unlit shaders output texture x colour, and $selfillum replaces the
// lighting of the masked part of a surface by tint x texture: the value a
// surface of that colour has under a lighting of 1 (a full-bright lightmap).
// The proxy's lights and sky keep Source's lighting values, so the same holds
// here with an emission of 1 x colour (remixapi_MaterialInfoSelfIlluminationEXT:
// measured equal to the same texture lit by a uniform radiance of 1, and
// metered by the auto exposure like it). OPENREMIX_PROXY_EMISSIVE_SCALE
// scales it.
// Such a material is a variant of its base texture's: the same texture is lit
// in one material and glows in another, and tints change at runtime (material
// proxies), so variants are keyed by everything that defines them and dropped
// when unused. The plain lit material keeps the texture's hash (mod
// replacements are keyed by it).
struct EmissiveMaterial {
  remixapi_MaterialHandle handle = nullptr;
  uint64_t reflectionSceneId = 0, reflectionVolumeId = 0;
  uint64_t lastFrame = 0;
  uint32_t pins = 0; // BSP materials holding it
};
std::unordered_map<uint64_t, EmissiveMaterial> g_emissiveMaterials; // by material hash; under g_textureLock

} // namespace
RemixTexture::~RemixTexture() {
  // Its variants go with it (a render-target texture is a new record per
  // image: variants left behind would keep every old image alive). One the
  // BSP world holds keeps this record alive, so none is pinned here.
  {
    std::lock_guard<std::recursive_mutex> lock(g_textureLock);
    for (uint64_t key : emissive)
      if (auto found = g_emissiveMaterials.find(key); found != g_emissiveMaterials.end() && !found->second.pins) {
        if (g_ready && found->second.handle)
          g_api.DestroyMaterial(found->second.handle);
        g_emissiveMaterials.erase(found);
      }
  }
  if (!g_ready)
    return;
  if (material)
    g_api.DestroyMaterial(material);
  if (pointMaterial)
    g_api.DestroyMaterial(pointMaterial);
  if (skyMaterial)
    g_api.DestroyMaterial(skyMaterial);
  if (clampMaterial)
    g_api.DestroyMaterial(clampMaterial);
  if (pointClampMaterial)
    g_api.DestroyMaterial(pointClampMaterial);
  if (texture)
    g_api.DestroyTexture(texture);
}
namespace {

// OPENREMIX_PROXY_EMISSIVE_LIGHTS: which of these surfaces light the scene
// (the runtime's emissive triangle lights; all of them glow either way).
//   0  none.
//   1  the BSP world's: self-illuminated parts of brushes and static props,
//      and unlit materials whose VMT says $selfillum (light panels, monitors).
//      A fixed set of emitters.
//   2  (default) also the self-illuminated parts of what the engine draws
//      (models, the view model). Their emitters come and go with the prop;
//      the runtime keeps them in the dynamic part of its light hierarchy.
//   3  also every unlit surface (foliage cards, skybox scenery, 3D2D panels,
//      sprites: many emitters, changing every frame).
bool emissiveLights(const engine_materials::Shading &s, bool bspWorld) {
  static const int mode = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_EMISSIVE_LIGHTS", v, sizeof(v)) ? std::atoi(v) : 2;
  }();
  if (mode <= 0)
    return false;
  if (s.mode == engine_materials::Shading::Unlit)
    return mode >= 3 || (bspWorld && s.luminous);
  return bspWorld || mode >= 2;
}

// The uploaded texture a mask name stands for (its dxvk hash), or 0.
// Render thread. Candidate hashes come from the VTF file (as the BSP world's).
// The one returned is the record's own, the runtime texture's: a candidate may
// be an X8R8G8B8 upload's opaque alias (BGR888 files, such as most props'
// normal maps), which no runtime texture is named by.
struct NamedTexture {
  std::vector<uint64_t> candidates;
  uint64_t resolved = 0;
};
// The runtime texture's hash of the live upload known by `hash` (its own or
// its opaque alias), or 0. Caller holds g_textureLock.
uint64_t uploadedTextureHash(uint64_t hash) {
  auto found = g_texturesByHash.find(hash);
  if (found == g_texturesByHash.end())
    return 0;
  const auto texture = found->second.lock();
  return texture ? texture->hash : 0;
}

// frame: of an animated texture, each frame its own upload.
NamedTexture &namedTexture(const std::string &name, uint32_t frame = 0) {
  static std::unordered_map<std::string, NamedTexture> masks;
  if (masks.size() > 4096)
    masks.clear();
  auto [it, added] = masks.try_emplace(frame ? name + '#' + std::to_string(frame) : name);
  if (added)
    it->second.candidates = bsp::textureCandidates(name, frame);
  return it->second;
}

uint64_t maskTextureHash(const std::string &name, bool count = true, uint32_t frame = 0) {
  NamedTexture &mask = namedTexture(name, frame);
  if (mask.candidates.empty()) {
    g_stats.emissiveMaskNoHash += count;
    return 0;
  }
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  if (mask.resolved)
    if (const uint64_t hash = uploadedTextureHash(mask.resolved))
      return hash;
  mask.resolved = 0;
  for (uint64_t candidate : mask.candidates)
    if (const uint64_t hash = uploadedTextureHash(candidate)) {
      mask.resolved = candidate;
      return hash;
    }
  g_stats.emissiveMaskNotUploaded += count;
  return 0;
}

// The texture's material sampled clamp-to-edge: an iris is projected onto the
// whole eye and must not repeat beside itself (its border texels are
// transparent). Otherwise as ensureMaterial.
remixapi_MaterialHandle clampedMaterialFor(const void *texture, bool nearest = false) {
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  auto it = g_textures.find(texture);
  if (it == g_textures.end())
    return nullptr;
  RemixTexture &t = *it->second;
  remixapi_MaterialHandle &slot = nearest ? t.pointClampMaterial : t.clampMaterial;
  if (!slot) {
    wchar_t path[32];
    swprintf(path, 32, L"0x%llX", static_cast<unsigned long long>(t.hash));
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.roughnessConstant = 1;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    opaque.useDrawCallAlphaState = 1;
    remixapi_MaterialInfoPbrEXT diffuse = {};
    diffuse.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
    opaque.pNext = &diffuse;
    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = nearest ? (t.hash ^ 0xc1a3b0c1a3b0c1a3ull ^ 0x701475a1701475a1ull) : (t.hash ^ 0xc1a3b0c1a3b0c1a3ull);
    info.albedoTexture = path;
    info.filterMode = nearest ? 0 : 1;
    info.wrapModeU = info.wrapModeV = 3; // clamp to edge (1: repeat)
    if (g_api.CreateMaterial(&info, &slot) != REMIXAPI_ERROR_CODE_SUCCESS)
      slot = nullptr;
  }
  return slot;
}

// The lit material of the uploaded texture a name stands for (nullptr: its
// file gave no hashes, or none of them is uploaded now). For draws whose base
// texture is not on sampler 0 (onDraw: the Cable shader). The record is held
// by the engine's D3D texture (g_textures), as a draw's own texture's is.
remixapi_MaterialHandle materialForName(const std::string &name, bool nearest = false) {
  const uint64_t hash = name.empty() ? 0 : maskTextureHash(name, false);
  if (!hash)
    return nullptr;
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  const auto found = g_texturesByHash.find(hash);
  const auto texture = found != g_texturesByHash.end() ? found->second.lock() : nullptr;
  return texture ? ensureMaterial(*texture, 0, nearest) : nullptr;
}

// A draw belongs to the material bound when it reaches D3D, usually. Not
// always (seen: a self-illuminated prop bound every frame whose draw arrived
// under another material from some view directions), so the materials that
// are not plainly lit are also known by their base texture: a draw of that
// texture takes the latest bind of a material with it, lit or not.
struct BoundShading {
  engine_materials::Shading shading;
  std::string material;
  uint64_t frame = 0;
};
std::unordered_map<std::string, BoundShading> g_shadingByBase;  // base texture name; render thread
std::unordered_map<uint64_t, std::string> g_emissiveBaseNames;  // uploaded texture hash -> that name
struct Rebound {
  std::string bound;
  uint32_t draws = 0;
};
std::map<std::string, Rebound> g_rebound; // statistics, under g_transientSampleLock

// The shading of a draw of `texture`, given the bound material's.
engine_materials::Shading shadingFor(const void *texture, const engine_materials::Shading &bound) {
  using engine_materials::Shading;
  const uint64_t hash = textureHash(texture);
  const auto name = hash ? g_emissiveBaseNames.find(hash) : g_emissiveBaseNames.end();
  if (name != g_emissiveBaseNames.end()) {
    const auto latest = g_shadingByBase.find(name->second);
    if (latest != g_shadingByBase.end() && latest->second.frame + 1 >= g_frame) {
      if (bound.base != name->second) {
        std::lock_guard<std::mutex> lock(g_transientSampleLock);
        if (g_rebound.size() < 32 || g_rebound.count(latest->second.material)) {
          Rebound &r = g_rebound[latest->second.material];
          r.bound = engine_materials::currentName();
          ++r.draws;
        }
      }
      return latest->second.shading;
    }
  }
  // Counted, not acted on: a bound unlit or self-illuminated material whose
  // file's hashes do not include this draw's texture (which may still be its
  // own: other frames of an animated texture are not among them).
  if (bound.mode != Shading::Lit && !bound.base.empty() && hash) {
    const auto &candidates = namedTexture(bound.base).candidates;
    if (maskTextureHash(bound.base, false) &&
        std::find(candidates.begin(), candidates.end(), hash) == candidates.end())
      ++g_stats.emissiveNotItsTexture;
  }
  return bound;
}

// Statistics: the materials whose conversion was dropped because the draw's
// texture is not their base texture's file (under g_transientSampleLock).
struct PbrNotBase {
  std::string material;
  uint64_t texture = 0;
  size_t candidates = 0;
  uint32_t draws = 0;
};
std::map<std::string, PbrNotBase> g_pbrNotBase; // by base texture name

// A PBR conversion's maps as uploaded textures (0: none).
struct PbrMaps {
  bool use = false; // the conversion applies
  uint64_t normal = 0, maps[2] = {}; // roughness, metallic
};

// The maps of `s`'s conversion for a draw of `t`: none unless `t` is the
// material's base texture (another texture on sampler 0 is not what its maps
// belong to). complete: every map it names is uploaded. Caller holds
// g_textureLock.
PbrMaps pbrMapsFor(const RemixTexture &t, const engine_materials::Shading &s, bool knownBase, bool *complete = nullptr) {
  PbrMaps maps;
  if (complete)
    *complete = true;
  if (!s.pbr.active)
    return maps;
  if (!knownBase) {
    const auto &candidates = namedTexture(s.base).candidates;
    if (s.base.empty() || (maskTextureHash(s.base, false) != t.hash &&
                           std::find(candidates.begin(), candidates.end(), t.hash) == candidates.end())) {
      ++g_stats.pbrNotBase;
      std::lock_guard<std::mutex> lock(g_transientSampleLock);
      if (g_pbrNotBase.size() < 32 || g_pbrNotBase.count(s.base)) {
        PbrNotBase &n = g_pbrNotBase[s.base];
        n.material = engine_materials::currentName();
        n.texture = t.hash;
        n.candidates = candidates.size();
        ++n.draws;
      }
      return maps;
    }
  }
  maps.use = true;
  auto resolve = [&](const std::string &name) -> uint64_t {
    if (name.empty())
      return 0;
    if (name == s.base)
      return t.hash;
    const uint64_t hash = maskTextureHash(name, false);
    if (!hash && complete)
      *complete = false;
    return hash;
  };
  maps.normal = resolve(s.pbr.normal);
  bool missing = !s.pbr.normal.empty() && !maps.normal;
  for (int i = 0; i < 2; ++i) {
    maps.maps[i] = resolve(s.pbr.maps[i].texture);
    missing |= !s.pbr.maps[i].texture.empty() && !maps.maps[i];
  }
  g_stats.pbrMapsMissing += missing;
  return maps;
}

// The variant of a texture's material that shows it as `s` says, with the
// mask texture `maskHash` (0: none) and the PBR maps `pbr` (unused: diffuse).
// Caller holds g_textureLock.
EmissiveMaterial *ensureEmissive(RemixTexture &t, const engine_materials::Shading &s, uint64_t maskHash, bool lights,
                                 uint64_t *materialHash = nullptr, const PbrMaps &pbr = {}, bool nearest = false) {
  using engine_materials::Shading;
  const bool emits = s.mode != Shading::Lit;
  static const float scale = envScale("OPENREMIX_PROXY_EMISSIVE_SCALE", 1.0f);
  // Tints are continuous (proxies animate them): 1/64 steps bound the variants.
  struct Key {
    uint64_t mask;
    float maskScale;
    uint16_t tint[3];
    uint8_t mode, lights;
    // The PBR conversion: its maps; roughness, metallic, then per map
    // exponent, bias, scale; in use | ssbump << 1, then per map invert | curve
    // << 1 | channel << 2.
    uint64_t normal, maps[2];
    float pbr[8];
    uint8_t pbrBits[3];
    uint8_t nearest;
    uint16_t albedo[3]; // Shading::albedo, 1/1024 steps
  } key;
  std::memset(&key, 0, sizeof(key)); // hashed as bytes
  key.mask = maskHash;
  key.maskScale = s.maskScale;
  for (int c = 0; c < 3; ++c)
    key.tint[c] = uint16_t(std::lround(std::clamp(std::isfinite(s.tint[c]) ? s.tint[c] : 0.0f, 0.0f, 64.0f) * 64));
  key.mode = uint8_t(s.mode);
  key.lights = lights;
  key.nearest = nearest ? 1 : 0;
  for (int c = 0; c < 3; ++c)
    key.albedo[c] = uint16_t(std::lround(std::clamp(std::isfinite(s.albedo[c]) ? s.albedo[c] : 1.0f, 0.0f, 1.0f) * 1024));
  if (pbr.use) {
    const auto &p = s.pbr;
    key.normal = pbr.normal;
    key.pbr[0] = p.roughness;
    key.pbr[1] = p.metallic;
    key.pbrBits[0] = uint8_t(1 | p.ssbump << 1);
    for (int i = 0; i < 2; ++i) {
      const auto &m = p.maps[i];
      key.maps[i] = pbr.maps[i];
      key.pbr[2 + 3 * i] = m.exponent;
      key.pbr[3 + 3 * i] = m.bias;
      key.pbr[4 + 3 * i] = m.scale;
      key.pbrBits[1 + i] = uint8_t(m.invert | (m.curve & 1) << 1 | (m.channel & 3) << 2);
    }
  }
  uint64_t hash = XXH3_64bits_withSeed(&key, sizeof(key), t.hash);
  if (s.reflectionReceiverKey)
    hash = XXH3_64bits_withSeed(&s.reflectionReceiverKey, sizeof(s.reflectionReceiverKey), hash);
  if (!hash || hash == t.hash)
    hash = ~t.hash;
  if (materialHash)
    *materialHash = hash;
  EmissiveMaterial &variant = g_emissiveMaterials[hash];
  variant.lastFrame = g_frame;
  const bool existed = variant.handle != nullptr;
  if (existed && variant.reflectionSceneId == s.reflectionSceneId && variant.reflectionVolumeId == s.reflectionVolumeId)
    return &variant;
  wchar_t path[32], maskPath[32], normalPath[32], mapPaths[2][32];
  swprintf(path, 32, L"0x%llX", static_cast<unsigned long long>(t.hash));
  swprintf(maskPath, 32, L"0x%llX", static_cast<unsigned long long>(maskHash));
  swprintf(normalPath, 32, L"0x%llX", static_cast<unsigned long long>(pbr.normal));
  for (int i = 0; i < 2; ++i)
    swprintf(mapPaths[i], 32, L"0x%llX", static_cast<unsigned long long>(pbr.maps[i]));
  // PBR: Source's normal maps are tangent space with DirectX's green.
  remixapi_MaterialNormalEncodingEXT encoding = {};
  encoding.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT;
  encoding.encoding = s.pbr.ssbump ? 4 : 2;
  remixapi_MaterialPbrRemapEXT remap = {};
  remap.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_REMAP_EXT;
  remixapi_MaterialPbrChannelEXT *remaps[2] = {&remap.roughness, &remap.metallic};
  for (int i = 0; i < 2; ++i) {
    const auto &m = s.pbr.maps[i];
    *remaps[i] = {m.channel, m.invert, m.curve, m.exponent, m.bias, m.scale};
  }
  remixapi_MaterialInfoPbrEXT enable = {};
  enable.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
  enable.enabled = 1;
  remixapi_MaterialInfoReplacementHashEXT replacement = {};
  replacement.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_REPLACEMENT_HASH_EXT;
  replacement.materialHash = t.legacyHash;
  remixapi_MaterialInfoEmissionEXT emission = {};
  emission.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT;
  emission.suppressLightGeneration = !lights;
  remixapi_MaterialInfoSelfIlluminationEXT self = {};
  self.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
  self.pNext = &emission;
  self.mask = s.mode == Shading::SelfIllumBaseAlpha   ? REMIXAPI_SELF_ILLUMINATION_MASK_ALBEDO_ALPHA_EXT
              : s.mode == Shading::SelfIllumMaskAlpha ? REMIXAPI_SELF_ILLUMINATION_MASK_EMISSIVE_ALPHA_EXT
                                                      : REMIXAPI_SELF_ILLUMINATION_MASK_NONE_EXT;
  self.maskScale = std::isfinite(s.maskScale) && s.maskScale > 0 ? s.maskScale : 1;
  // As the lit material (ensureMaterial), plus the emission and the PBR
  // conversion. A lit one keeps the texture's legacy key, as that material.
  void *chain = emits ? static_cast<void *>(&self) : t.legacyHash ? &replacement : nullptr;
  remixapi_MaterialInfoOpaqueEXT opaque = {};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.roughnessConstant = 1; // PBR conversion below overrides this diffuse fallback.
  if (pbr.use) {
    remap.pNext = chain;
    enable.pNext = &remap;
    chain = &enable;
    if (pbr.normal) {
      encoding.pNext = chain;
      chain = &encoding;
    }
    opaque.roughnessConstant = s.pbr.roughness;
    opaque.metallicConstant = s.pbr.metallic;
    if (pbr.maps[0])
      opaque.roughnessTexture = mapPaths[0];
    if (pbr.maps[1])
      opaque.metallicTexture = mapPaths[1];
  } else {
    enable.enabled = 0; // The rough Forward+ fallback must not opt path tracing into GGX.
    enable.pNext = chain;
    chain = &enable;
  }
  remixapi_MaterialReflectionBindingEXT reflection{
      REMIXAPI_STRUCT_TYPE_MATERIAL_REFLECTION_BINDING_EXT, chain, s.reflectionSceneId, s.reflectionVolumeId};
  opaque.pNext = s.reflectionReceiverKey ? static_cast<void *>(&reflection) : chain;
  opaque.albedoConstant = {key.albedo[0] / 1024.0f, key.albedo[1] / 1024.0f, key.albedo[2] / 1024.0f};
  opaque.opacityConstant = 1;
  opaque.alphaTestType = 7;
  opaque.useDrawCallAlphaState = 1;
  remixapi_MaterialInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &opaque;
  info.hash = hash;
  info.albedoTexture = path;
  if (pbr.use && pbr.normal)
    info.normalTexture = normalPath;
  if (maskHash)
    info.emissiveTexture = maskPath;
  if (emits) {
    info.emissiveIntensity = scale;
    info.emissiveColorConstant = {key.tint[0] / 64.0f, key.tint[1] / 64.0f, key.tint[2] / 64.0f};
  }
  info.filterMode = nearest ? 0 : 1;
  info.wrapModeU = info.wrapModeV = 1;
  if (g_api.CreateMaterial(&info, &variant.handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
    if (!existed) g_emissiveMaterials.erase(hash);
    ++g_stats.emissiveFailed;
    return nullptr;
  }
  variant.reflectionSceneId = s.reflectionSceneId;
  variant.reflectionVolumeId = s.reflectionVolumeId;
  if (existed) return &variant;
  if (emits) ++g_stats.emissiveCreated;
  else if (pbr.use) ++g_stats.pbrCreated;
  // Evicted variants (animated tints) leave their keys behind.
  if (t.emissive.size() >= 256)
    t.emissive.erase(std::remove_if(t.emissive.begin(), t.emissive.end(),
                                    [](uint64_t k) { return !g_emissiveMaterials.count(k); }),
                     t.emissive.end());
  t.emissive.push_back(hash);
  return &variant;
}

// The draw counts of a PBR conversion.
void countPbr(const PbrMaps &maps) {
  if (!maps.use)
    return;
  ++g_stats.pbrDraws;
  g_stats.pbrNormalDraws += maps.normal != 0;
  g_stats.pbrMapDraws += maps.maps[0] != 0;
  g_stats.pbrMetalDraws += maps.maps[1] != 0;
}

// A draw's material: the texture's lit one, or its unlit / self-illuminated
// variant (falling back to the lit one when its mask texture is not known),
// with the material's PBR conversion.
remixapi_MaterialHandle materialFor(const void *texture, const engine_materials::Shading &s, bool nearest = false) {
  using engine_materials::Shading;
  if (const int slot = screenSlot(texture); slot >= 0)
    return screenMaterial(slot);
  const bool tinted = s.albedo[0] != 1 || s.albedo[1] != 1 || s.albedo[2] != 1;
  if (s.mode == Shading::Lit && !s.pbr.active && !tinted)
    return materialFor(texture, 0, nearest);
  const bool masked = s.mode == Shading::SelfIllumMask || s.mode == Shading::SelfIllumMaskAlpha;
  const uint64_t maskHash = masked ? maskTextureHash(s.mask) : 0;
  if (masked && !maskHash && !s.pbr.active)
    return materialFor(texture, 0, nearest);
  const bool lights = emissiveLights(s, false);
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  auto it = g_textures.find(texture);
  if (it == g_textures.end())
    return nullptr;
  const PbrMaps maps = pbrMapsFor(*it->second, s, false);
  if (masked && !maskHash) {
    // Lit, with the conversion.
    Shading lit = s;
    lit.mode = Shading::Lit;
    if (!maps.use)
      return ensureMaterial(*it->second, 0, nearest);
    if (EmissiveMaterial *variant = ensureEmissive(*it->second, lit, 0, false, nullptr, maps, nearest)) {
      countPbr(maps);
      return variant->handle;
    }
    return ensureMaterial(*it->second, 0, nearest);
  }
  if (s.mode == Shading::Lit) {
    if (!maps.use && !tinted)
      return ensureMaterial(*it->second, 0, nearest);
    if (EmissiveMaterial *variant = ensureEmissive(*it->second, s, 0, false, nullptr, maps, nearest)) {
      countPbr(maps);
      return variant->handle;
    }
    return ensureMaterial(*it->second, 0, nearest);
  }
  if (EmissiveMaterial *variant = ensureEmissive(*it->second, s, maskHash, lights, nullptr, maps, nearest)) {
    countPbr(maps);
    switch (s.mode) {
    case Shading::Unlit: ++g_stats.unlitDraws; break;
    case Shading::SelfIllumBaseAlpha: ++g_stats.selfIllumDraws; break;
    case Shading::SelfIllumMask: ++g_stats.selfIllumMaskDraws; break;
    default: ++g_stats.selfIllumMaskAlphaDraws; break;
    }
    g_stats.emissiveLightDraws += lights;
    return variant->handle;
  }
  return ensureMaterial(*it->second, 0, nearest);
}

// Variants nothing drew for a while (a tint that changed) go; pinned ones
// belong to the BSP world, which does not ask again.
void evictEmissiveMaterials() {
  if (g_frame % 64)
    return;
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  for (auto it = g_emissiveMaterials.begin(); it != g_emissiveMaterials.end();) {
    if (!it->second.pins && g_frame - it->second.lastFrame > 300) {
      if (it->second.handle)
        g_api.DestroyMaterial(it->second.handle);
      ++g_stats.emissiveEvicted;
      it = g_emissiveMaterials.erase(it);
    } else {
      ++it;
    }
  }
}

// The material of an uploaded texture by its dxvk hash, for the BSP world;
// `emission`: its unlit / self-illuminated variant, with the first uploaded
// of `maskCandidates` as mask (no handle until that is uploaded).
bsp::MaterialRef materialForHash(uint64_t hash, const engine_materials::Shading *emission,
                                 const std::vector<uint64_t> *maskCandidates, bool nearest = false) {
  using engine_materials::Shading;
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  auto it = g_texturesByHash.find(hash);
  if (it == g_texturesByHash.end())
    return {};
  auto texture = it->second.lock();
  if (!texture)
    return {};
  bsp::MaterialRef ref;
  // Its PBR conversion waits for the maps it names.
  bool complete = true;
  const PbrMaps maps = emission ? pbrMapsFor(*texture, *emission, true, &complete) : PbrMaps{};
  if (!complete)
    return {};
  if (!emission || (emission->mode == Shading::Lit && !maps.use && !emission->reflectionReceiverKey)) {
    ref.handle = ensureMaterial(*texture, 0, nearest);
    ref.keep = std::move(texture);
    return ref;
  }
  uint64_t maskHash = 0;
  if (emission->mode == Shading::SelfIllumMask || emission->mode == Shading::SelfIllumMaskAlpha) {
    for (uint64_t candidate : maskCandidates ? *maskCandidates : std::vector<uint64_t>())
      if ((maskHash = uploadedTextureHash(candidate)))
        break;
    if (!maskHash)
      return {};
  }
  uint64_t materialHash = 0;
  EmissiveMaterial *variant =
      ensureEmissive(*texture, *emission, maskHash, emissiveLights(*emission, true), &materialHash, maps, nearest);
  if (!variant)
    return {};
  ++variant->pins;
  ref.handle = variant->handle;
  // Keeps the base texture, and the variant until the BSP material goes.
  struct Pin {
    std::shared_ptr<RemixTexture> texture;
    uint64_t materialHash;
  };
  ref.keep = std::shared_ptr<void>(new Pin{std::move(texture), materialHash}, [](void *p) {
    auto *pin = static_cast<Pin *>(p);
    {
      std::lock_guard<std::recursive_mutex> lock(g_textureLock);
      if (auto found = g_emissiveMaterials.find(pin->materialHash); found != g_emissiveMaterials.end() && found->second.pins) {
        --found->second.pins;
        found->second.lastFrame = g_frame;
      }
    }
    delete pin; // may release the texture: outside the lock
  });
  return ref;
}

// Depth of IVRenderView::DrawWorldLists calls (openremix_proxy_world_lists).
int g_worldLists = 0;
// Dynamic world-list draws kept for the engine, by base texture (statistics).
std::unordered_map<uint64_t, uint32_t> g_keptWorldTextures;
// Depth of COverlayMgr::RenderOverlays calls (openremix_proxy_overlays).
int g_overlays = 0;
// Categories l4d2-rtx applies to the draws that follow (onMarker "body").
uint32_t g_markerCategories = 0;

// --- Lights ------------------------------------------------------------------
// LightDesc_t (public/mathlib/lightdesc.h), 88 bytes.
using SourceLight = lighting::SourceLight;
constexpr size_t kLightDescSize = 88, kLightDescPart = offsetof(SourceLight, cookieUp);

std::mutex g_lightLock;
std::unordered_map<uint64_t, SourceLight> g_frameLights, g_lastLights;
struct CachedLight {
  remixapi_LightHandle handle = nullptr;
  uint64_t lastFrame = 0;
  uint64_t definition = 0; // see g_lightDefinitions
};
std::unordered_map<uint64_t, CachedLight> g_lights;
// This frame's lights whose hash is an identity, not their contents (styled
// map lights, dlights, projected textures): the contents' hash. When it
// changes the Remix light is redefined under the same hash.
std::unordered_map<uint64_t, uint64_t> g_lightDefinitions;

float envScale(const char *name, float fallback) {
  char v[32] = {};
  const float value = GetEnvironmentVariableA(name, v, sizeof(v)) ? float(atof(v)) : 0;
  return value > 0 ? value : fallback;
}

remixapi_LightHandle createLight(const SourceLight &light, uint64_t hash) {
  static const float lightScale = envScale("OPENREMIX_PROXY_LIGHT_SCALE", 1.0f);
  static const float sunScale = envScale("OPENREMIX_PROXY_SUN_SCALE", 1.0f);
  return lighting::createLight(g_api, light, hash, lightScale, sunScale);

}

} // namespace
// This frame's 2D draws (HUD, VGUI), submitted as one UI draw list.
std::vector<remixapi_UIVertex> g_uiVertices;
std::vector<uint32_t> g_uiIndices;
std::vector<remixapi_UIDrawCommand> g_uiCommands;
uint32_t g_displayWidth = 0, g_displayHeight = 0;

// In-process UI contributors (l4d2-rtx's ImGui menu): called while the frame's
// UI list is being built, they add draws with openremix_ui_append.
using UiCallback = void(__cdecl *)();
UiCallback g_uiCallback = nullptr;
namespace {

struct Instance {
  remixapi_MeshHandle mesh;
  remixapi_MaterialHandle material;
  const void *texture0; // sky variant material
  remixapi_InstanceInfoBlendEXT blend;
  uint32_t categories;
  uint64_t replacementHash; // dxvk-remix mesh replacement key (0: none)
  bool persistent;          // mesh from static buffers (cached across frames)
  bool singleSided = false; // blended draw D3D culls: its back faces are not seen
  bool cameraCullBack = false; // D3D culls its back faces: hidden from the camera only
  std::vector<remixapi_Transform> bones; // skinned instances
  float transform[3][4];
  uint64_t view;
  bool skinned = false;     // a skinned model's draw (bones may still be dropped: single-bone models are rigid)
  bool modelLayer = false;  // a layer the proxy adds to a model (an eye's iris): of the model whatever its transform
  uint32_t primitives = 0, firstIndex = 0; // the draw's primitive count and first index (a screen view's dedupe)
  bool offTarget = false; // drawn into a render-target texture: a screen camera's view (role Screen) or nothing
  // Stencil emulation: the draw's group (g_stencilGroups index) and part in it.
  int stencilGroup = -1;
  uint8_t stencilRole = 0;
  bool depthTest = true, depthWrite = true;
  // A particle draw (submitParticles): rasterized over the traced image with
  // its blend state, whatever that is (rasterOnly), unless particles are
  // traced; its depth state is the draw's.
  bool particle = false, rasterOnly = false;
  // Drawn as its triangles' edges (D3DFILL_WIREFRAME: the Wireframe shader,
  // $wireframe): a raster-only overlay of lines (remixapi_InstanceInfoWireframeEXT).
  bool wireframe = false;
  // What its pixel shader does beyond texture x colour, for the runtime's
  // sprite overlays (remixapi_InstanceInfoSpriteEXT flags; 0: nothing).
  uint32_t spriteFlags = 0;
  float spriteSoft[3] = {}; // soft distance, fade start, fade end (view-space units)
};
std::vector<Instance> g_instances;

// The stencil buffer no longer exists (D3D rasterizes nothing), so stencil
// use is emulated from the draws' state. Draws that write a value mark a
// group; draws tested against that value are shown only inside (EQUAL; LESS
// for the value above) or outside (NOTEQUAL) the group's silhouette, through
// the runtime's masks (remixapi_InstanceInfoMaskEXT, masked UI commands).
// A group whose value is tested and then written again starts over (the game
// cleared the stencil in between: Clear is not seen).
//   Writers: ALWAYS + pass REPLACE (also drawn normally: a sight's housing);
//   NEVER + fail REPLACE (invisible: a scope's lens opening); EQUAL + pass
//   INCR (invisible marks inside another group: thermal bodies in the lens).
//   halo.lua: a full-screen fill in the halo colour where EQUAL, then the
//   blurred image where NOTEQUAL: such a group is a halo
//   (remixapi_InstanceInfoHaloEXT), its sources are re-draws, not scene
//   instances. Blur sizes are not observable: halo.Add's default is used.
struct StencilGroup {
  uint32_t value = 0;
  bool tested = false;
  uint32_t sources = 0, consumers = 0;
  int within = -1;
  bool fill = false, halo = false, additive = true, ignoreZ = false;
  // A sight's reticle floats ~100 units ahead, depth-tested: in the game only
  // against the view model (its own depth range), where it shows through the
  // sight's glass and not its housing. Here the view model shares the world's
  // depth and a nearby wall hid the reticle. With `throughGlass`, only the
  // marks that write no depth (the glass) make the mask and the reticle is
  // not depth-tested.
  uint32_t glassSources = 0;
  bool throughGlass = false;
  float color[3] = {};
  uint32_t passes = 0;
  uint32_t mask = 0; // runtime mask group (1..32), assigned at Present
};
enum : uint8_t { kStencilNone, kStencilSource, kStencilSourceOnly, kStencilInside, kStencilCut };
std::vector<StencilGroup> g_stencilGroups;           // this frame's
std::unordered_map<uint32_t, int> g_stencilCurrent;  // stencil value -> its current group
std::vector<std::pair<int, bool>> g_uiCommandMasks;  // per g_uiCommands entry: group (-1 none), outside

// Halo groups need their fill (the colour); the other groups that are both
// marked and tested get a runtime mask. Once per frame, before submission.
bool g_stencilMasksAssigned = false;
void assignStencilMasks() {
  if (g_stencilMasksAssigned)
    return;
  g_stencilMasksAssigned = true;
  uint32_t next = 0;
  for (auto &group : g_stencilGroups) {
    if (group.halo && group.fill)
      continue;
    if (group.sources && group.consumers && next < REMIXAPI_INSTANCE_MASK_MAX_GROUP_EXT)
      group.mask = ++next;
  }
}

int stencilWriter(uint32_t value) {
  auto it = g_stencilCurrent.find(value);
  if (it == g_stencilCurrent.end() || g_stencilGroups[it->second].tested) {
    g_stencilGroups.emplace_back().value = value;
    it = g_stencilCurrent.insert_or_assign(value, int(g_stencilGroups.size()) - 1).first;
  }
  ++g_stencilGroups[it->second].sources;
  return it->second;
}

// The group a test against `value` reads (-1: nothing wrote it).
int stencilReader(uint32_t value) {
  auto it = g_stencilCurrent.find(value);
  if (it == g_stencilCurrent.end())
    return -1;
  g_stencilGroups[it->second].tested = true;
  ++g_stencilGroups[it->second].consumers;
  return it->second;
}

struct View {
  std::vector<std::array<float, 3>> ambientUp; // +Z ambient cube of lit models in this view
  uint32_t draws = 0;
  uint32_t order = 0; // first use within the frame
  bool perspective = false;
  // Perspective with an identity view matrix: the draw's positions are in
  // camera space (see CameraSpace). Never a camera of its own.
  bool cameraSpace = false;
  float view[16], projection[16];
  float cameraToWorld[3][4] = {}; // a view with a camera: its camera space (as the engine has it) to world
  // Drawn into a screen camera's render-target texture (screenSlot): the
  // entities that camera sees, which the main view may not draw.
  const void *screenTarget = nullptr;
};
// Source's client-side particle manager (CParticleMgr: the emitters of FX_
// effects, env_smokestack, Lua's ParticleEmitter, ...) transforms its
// particles to camera space on the CPU and draws them with the view (and
// model) matrix set to identity (CParticleEffectBinding::RenderStart, unless
// the effect opts out). Such a draw arrives with a view-projection that is
// the projection alone: as a view of its own it matched no camera and
// everything in it was skipped. Its geometry belongs to the view the engine
// was rendering, the last one drawn with a camera: it is placed in the world
// by that view's inverse and submitted with it.
struct CameraSpace {
  uint64_t key = 0; // g_views key of the view last drawn with a camera this frame
  bool valid = false;
} g_cameraSpace; // render thread
std::atomic<uint32_t> g_cameraSpaceParticles{0}, g_cameraSpaceOther{0}, g_cameraSpaceLeft{0}, g_cameraSpaceNoView{0};
uint32_t g_viewOrder = 0;
// The views the engine drew a world for this frame (the matrices in effect at
// each outermost "world_lists 1" marker, in order): the main view and, before
// it, the 3D skybox view. The view model and overlays never draw a world.
struct WorldView {
  float view[16], projection[16];
};
std::vector<WorldView> g_worldViews;

// The scene fog of the last world view of the frame (the main view; the 3D
// skybox's, drawn before it, has its own), read from the shader API at the
// view's world_lists marker, for remixapi_SetFogState: OpenRemix's
// volumetrics take their medium from it. `maxDensity`: $fogmaxdensity, which
// the shader API has no getter for, from the fog register of the view's
// draws (c16.z; kept from the last frame that had one).
struct SceneFog {
  int mode = 0; // MaterialFogMode_t: 0 none, 1 linear, 2 linear below a height
  uint8_t color[3] = {};
  float start = 0, end = 0, z = 0;
  bool read = false;
} g_sceneFog;
float g_fogMaxDensity = 1;
// The main camera is in water (bsp::waterAt): set with the cameras each frame.
bool g_cameraInWater = false;
std::atomic<uint32_t> g_fogSent{0}, g_fogFrames{0};
float g_fogSample[8] = {}; // mode, start, end, max density, colour, sent end (statistics)

void readSceneFog() {
  auto *vtbl = *static_cast<void ***>(g_shaderApi);
  SceneFog fog;
  fog.mode = reinterpret_cast<int (*)(void *)>(vtbl[kSlotGetSceneFogMode])(g_shaderApi);
  reinterpret_cast<void (*)(void *, unsigned char *)>(vtbl[kSlotGetSceneFogColor])(g_shaderApi, fog.color);
  reinterpret_cast<void (*)(void *, float *, float *, float *)>(vtbl[kSlotGetFogDistances])(g_shaderApi, &fog.start,
                                                                                            &fog.end, &fog.z);
  fog.read = true;
  g_sceneFog = fog;
}

// Once a frame, before Present. Source's fog is linear from `start` to `end`,
// where it covers $fogmaxdensity of what is behind. OpenRemix makes of linear
// fog the exponential medium that leaves 5% at the `end` it is given: the end
// sent is the distance at which the game's fog, continued at its mean rate,
// would leave 5%. (The clear stretch before `start` is not kept.)
// OPENREMIX_PROXY_FOG=0: no fog is sent.
void sendSceneFog() {
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_FOG", v, sizeof(v)) && v[0] == '0');
  }();
  static auto set = reinterpret_cast<PFN_remixapi_SetFogState>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_SetFogState"));
  if (!set)
    return;
  const SceneFog fog = g_sceneFog;
  g_sceneFog.read = false;
  // Under water the game's fog is the water's, which the camera medium shows
  // already: the runtime keeps the fog it had above.
  if (g_cameraInWater)
    return;
  remixapi_FogInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_FOG_INFO;
  const float density = std::clamp(g_fogMaxDensity, 0.0f, 1.0f);
  const bool valid = enabled && fog.read && (fog.mode == 1 || fog.mode == 2) && std::isfinite(fog.start) &&
                     std::isfinite(fog.end) && fog.end > 1 && fog.end > fog.start && density > 0.005f;
  float sentEnd = 0;
  if (valid) {
    const float left = std::max(1 - density, 0.05f);
    sentEnd = fog.end * std::log(0.05f) / std::log(left);
    info.mode = 3; // D3DFOG_LINEAR
    info.color = {fog.color[0] / 255.0f, fog.color[1] / 255.0f, fog.color[2] / 255.0f};
    info.end = sentEnd;
    info.scale = 1 / sentEnd;
    ++g_fogSent;
  }
  ++g_fogFrames;
  set(&info);
  std::lock_guard<std::mutex> lock(g_transientSampleLock);
  const float sample[8] = {float(fog.read ? fog.mode : -1), fog.start, fog.end, density, float(fog.color[0]),
                           float(fog.color[1]), float(fog.color[2]), sentEnd};
  std::copy_n(sample, 8, g_fogSample);
}

// D3DBLEND / D3DBLENDOP / D3DCMP to the Vulkan enums InstanceInfoBlendEXT uses.
uint32_t vkBlendFactor(uint32_t d3d) {
  switch (d3d) {
  case D3DBLEND_ZERO: return 0;
  case D3DBLEND_ONE: return 1;
  case D3DBLEND_SRCCOLOR: return 2;
  case D3DBLEND_INVSRCCOLOR: return 3;
  case D3DBLEND_DESTCOLOR: return 4;
  case D3DBLEND_INVDESTCOLOR: return 5;
  case D3DBLEND_SRCALPHA: return 6;
  case D3DBLEND_INVSRCALPHA: return 7;
  case D3DBLEND_DESTALPHA: return 8;
  case D3DBLEND_INVDESTALPHA: return 9;
  case D3DBLEND_BLENDFACTOR: return 10;
  case D3DBLEND_INVBLENDFACTOR: return 11;
  case D3DBLEND_SRCALPHASAT: return 14;
  default: return 1;
  }
}
uint32_t vkBlendOp(uint32_t d3d) { return d3d >= D3DBLENDOP_ADD && d3d <= D3DBLENDOP_MAX ? d3d - 1 : 0; }
uint32_t vkCompareOp(uint32_t d3d) { return d3d >= D3DCMP_NEVER && d3d <= D3DCMP_ALWAYS ? d3d - 1 : 7; }

remixapi_InstanceInfoBlendEXT blendState(const Draw &d) {
  remixapi_InstanceInfoBlendEXT b = {};
  b.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  b.alphaTestEnabled = d.alphaTest;
  b.alphaTestReferenceValue = uint8_t(d.alphaRef);
  b.alphaTestCompareOp = d.alphaTest ? vkCompareOp(d.alphaFunc) : 7;
  b.alphaBlendEnabled = d.blend;
  if (d.blend) {
    b.srcColorBlendFactor = vkBlendFactor(d.srcBlend);
    b.dstColorBlendFactor = vkBlendFactor(d.dstBlend);
    b.colorBlendOp = vkBlendOp(d.blendOp);
    b.srcAlphaBlendFactor = vkBlendFactor(d.separateAlpha ? d.srcBlendAlpha : d.srcBlend);
    b.dstAlphaBlendFactor = vkBlendFactor(d.separateAlpha ? d.dstBlendAlpha : d.dstBlend);
    b.alphaBlendOp = vkBlendOp(d.separateAlpha ? d.blendOpAlpha : d.blendOp);
    b.writeMask = d.colorWrite & 15;
  }
  return b;
}

// dxvk-remix fork InstanceCategories (bit order of l4d2-rtx's enum) to
// remixapi_InstanceCategoryBit; the orders diverge from bit 8.
uint32_t remixCategories(uint32_t fork) {
  static const int8_t map[25] = {0,  1,  2,  3,  4,  5,  6,  7,  21, 8,  9,  10, 11,
                                 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 23, -1};
  uint32_t out = 0;
  for (int bit = 0; bit < 25; ++bit)
    if ((fork >> bit & 1) && map[bit] >= 0)
      out |= 1u << map[bit];
  return out;
}

// World-space transform that makes geometry seen through `main` project like
// it does through a camera at the same place with a narrower field of view:
// view-space x and y scale by the projections' ratios (world' = V^-1 S V world).
// View matrices are row-vector (p_view = p * V) and rigid.
void viewScaleTransform(const float *view, float sx, float sy, float out[3][4]) {
  float r[3][3], t[3];
  for (int j = 0; j < 3; ++j) {
    for (int i = 0; i < 3; ++i)
      r[j][i] = view[i * 4 + j]; // column-vector rotation: v = r p + t
    t[j] = view[12 + j];
  }
  const float s[3] = {sx, sy, 1.0f};
  for (int a = 0; a < 3; ++a) {
    for (int b = 0; b < 3; ++b) {
      float sum = 0;
      for (int k = 0; k < 3; ++k)
        sum += r[k][a] * s[k] * r[k][b]; // (r^T S r)[a][b]
      out[a][b] = sum;
    }
    float sum = 0;
    for (int k = 0; k < 3; ++k)
      sum += r[k][a] * (s[k] - 1.0f) * t[k]; // r^T (S - I) t
    out[a][3] = sum;
  }
}

// a = t * a for 3x4 affine (column-vector) transforms.
void premultiply(const float t[3][4], float a[3][4]) {
  float out[3][4];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) {
      float sum = c == 3 ? t[r][3] : 0.0f;
      for (int k = 0; k < 3; ++k)
        sum += t[r][k] * a[k][c];
      out[r][c] = sum;
    }
  }
  std::memcpy(a, out, sizeof(out));
}

bool sameRotation(const float *a, const float *b) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      if (std::fabs(a[r * 4 + c] - b[r * 4 + c]) > 1e-4f)
        return false;
  return true;
}

// Same rotation and translation: the same camera (e.g. another near/far plane).
bool samePlacement(const float *a, const float *b) {
  if (!sameRotation(a, b))
    return false;
  for (int i = 12; i < 15; ++i)
    if (std::fabs(a[i] - b[i]) > 1e-2f)
      return false;
  return true;
}
std::unordered_map<uint64_t, View> g_views;
View g_lastCamera; // for statistics
float g_lastSky[3] = {};
std::string g_viewSummary; // last frame's views, for statistics

int currentNumBones() {
  using Fn = int(__thiscall *)(void *);
  auto *vtbl = *static_cast<void ***>(g_shaderApi);
  return reinterpret_cast<Fn>(vtbl[kSlotGetCurrentNumBones])(g_shaderApi);
}

void getMatrix(int mode, float *out) {
  using Fn = void(__thiscall *)(void *, int, float *);
  auto *vtbl = *static_cast<void ***>(g_shaderApi);
  reinterpret_cast<Fn>(vtbl[kSlotGetMatrix])(g_shaderApi, mode, out);
}

// Source's packed normal (UBYTE4, bytes 0-1; bytes 2-3 hold the tangent),
// ported from UnpackNormal_UBYTE4 in public/mathlib/mathlib.h.
void unpackNormalUByte4(const uint8_t *packed, float out[3]) {
  float x = packed[0] - 128.0f, y = packed[1] - 128.0f;
  const float zSignBit = x < 0 ? 1.0f : 0.0f, tSignBit = y < 0 ? 1.0f : 0.0f;
  const float zSign = -(2 * zSignBit - 1), tSign = -(2 * tSignBit - 1);
  x = x * zSign - zSignBit - 64;
  y = y * tSign - tSignBit - 64;
  const float xSignBit = x < 0 ? 1.0f : 0.0f, ySignBit = y < 0 ? 1.0f : 0.0f;
  const float xSign = -(2 * xSignBit - 1), ySign = -(2 * ySignBit - 1);
  x = (x * xSign - xSignBit) / 63.0f;
  y = (y * ySign - ySignBit) / 63.0f;
  const float z = 1.0f - x - y;
  const float oolen = 1.0f / std::sqrt(x * x + y * y + z * z);
  out[0] = x * oolen * xSign;
  out[1] = y * oolen * ySign;
  out[2] = z * oolen * zSign;
}

const D3DVERTEXELEMENT9 *findElement(const D3DVERTEXELEMENT9 *e, BYTE usage, BYTE index) {
  for (; e->Stream != 0xff; ++e)
    if (e->Usage == usage && e->UsageIndex == index)
      return e;
  return nullptr;
}

// UI texture (re)registrations: count, bytes and time since the last dump.
std::atomic<uint32_t> g_usUiRegister{0}, g_usUiRegisterApi{0};
std::atomic<uint64_t> g_uiRegistrations{0}, g_uiRegionUpdates{0}, g_uiRegisteredBytes{0};

// Registers (or refreshes) the UI texture for a D3D texture; 0 when the
// texture has no CPU copy (e.g. render targets).
remixapi_UITextureHandle uiTextureFor(const void *texture) {
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  auto it = g_cpuTextures.find(texture);
  if (it == g_cpuTextures.end())
    return 0;
  auto &t = *it->second;
  const auto id = static_cast<remixapi_UITextureHandle>(reinterpret_cast<uintptr_t>(it->second.get()));
  // A registered 32-bit texture changes in place: only its dirty region goes
  // to the runtime (remixapi_UpdateUITextureRegionEXT), without a copy when
  // its alpha is real.
  static const auto updateRegion = reinterpret_cast<PFN_remixapi_UpdateUITextureRegionEXT>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_UpdateUITextureRegionEXT"));
  const bool wide = t.format == D3DFMT_A8R8G8B8 || t.format == D3DFMT_X8R8G8B8;
  if (t.registeredVersion && t.registeredVersion != t.version && updateRegion && wide &&
      t.data.size() == size_t(t.width) * t.height * 4) {
    const uint32_t x0 = t.dirty[0], y0 = t.dirty[1], x1 = t.dirty[2], y1 = t.dirty[3];
    remixapi_ErrorCode updated = REMIXAPI_ERROR_CODE_SUCCESS;
    if (x1 > x0 && y1 > y0) {
      Timer timer(g_usUiRegister);
      ++g_uiRegionUpdates;
      g_uiRegisteredBytes += uint64_t(x1 - x0) * (y1 - y0) * 4;
      const uint8_t *rows = t.data.data() + (size_t(y0) * t.width + x0) * 4;
      uint32_t pitch = t.width * 4;
      std::vector<uint8_t> opaque;
      if (t.format == D3DFMT_X8R8G8B8) {
        opaque.resize(size_t(x1 - x0) * (y1 - y0) * 4);
        for (uint32_t y = 0; y < y1 - y0; ++y)
          std::memcpy(opaque.data() + size_t(y) * (x1 - x0) * 4, rows + size_t(y) * pitch, size_t(x1 - x0) * 4);
        for (size_t i = 3; i < opaque.size(); i += 4)
          opaque[i] = 0xff;
        rows = opaque.data();
        pitch = (x1 - x0) * 4;
      }
      Timer apiTimer(g_usUiRegisterApi);
      updated = updateRegion(id, x0, y0, x1 - x0, y1 - y0, REMIXAPI_FORMAT_B8G8R8A8_UNORM, rows, pitch);
    }
    if (updated == REMIXAPI_ERROR_CODE_SUCCESS) {
      std::fill_n(t.dirty, 4, 0u);
      t.registeredVersion = t.version;
      return id;
    }
  }
  if (t.registeredVersion != t.version) {
    Timer timer(g_usUiRegister);
    ++g_uiRegistrations;
    g_uiRegisteredBytes += uint64_t(t.width) * t.height * 4;
    std::fill_n(t.dirty, 4, 0u);
    std::vector<uint8_t> rgba;
    remixapi_Format format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    if (t.format == D3DFMT_DXT1) {
      rgba = formats::decodeBC1(t.data.data(), t.width, t.height);
    } else if (t.format == D3DFMT_DXT5) {
      rgba = formats::decodeBC3(t.data.data(), t.width, t.height);
    } else {
      rgba = t.data; // BGRA in memory
      format = REMIXAPI_FORMAT_B8G8R8A8_UNORM;
      if (t.format == D3DFMT_X8R8G8B8)
        for (size_t i = 3; i < rgba.size(); i += 4)
          rgba[i] = 0xff;
    }
    remixapi_ErrorCode registered;
    {
      Timer apiTimer(g_usUiRegisterApi);
      registered = g_api.RegisterUITexture(id, t.width, t.height, format, rgba.data(), rgba.size());
    }
    if (registered != REMIXAPI_ERROR_CODE_SUCCESS)
      return 0;
    t.registeredVersion = t.version;
  }
  return id;
}

// Appends a 2D draw to this frame's UI draw list: positions go through the
// draw's model-view-projection registers to screen pixels.
// The draw's D3D blend as the runtime's UI blend flags: the default alpha
// blend as is; modulate overlays (DrawMaterialOverlay, env_screenoverlay:
// DESTCOLOR/SRCCOLOR), additive HUD elements (kill icons, additive fonts:
// SRCALPHA/ONE) and opaque fills keep their own.
uint32_t uiBlendFlags(const Draw &d) {
  auto factor = [](uint32_t blend) -> uint32_t { // D3DBLEND -> VkBlendFactor
    switch (blend) {
    case D3DBLEND_ZERO: return 0;
    case D3DBLEND_ONE: return 1;
    case D3DBLEND_SRCCOLOR: return 2;
    case D3DBLEND_INVSRCCOLOR: return 3;
    case D3DBLEND_DESTCOLOR: return 4;
    case D3DBLEND_INVDESTCOLOR: return 5;
    case D3DBLEND_SRCALPHA: return 6;
    case D3DBLEND_INVSRCALPHA: return 7;
    case D3DBLEND_DESTALPHA: return 8;
    case D3DBLEND_INVDESTALPHA: return 9;
    case D3DBLEND_SRCALPHASAT: return 14;
    default: return 6;
    }
  };
  const uint32_t src = d.blend ? d.srcBlend : D3DBLEND_ONE, dst = d.blend ? d.dstBlend : D3DBLEND_ZERO;
  if (src == D3DBLEND_SRCALPHA && dst == D3DBLEND_INVSRCALPHA)
    return 0; // the default
  return REMIXAPI_UI_NATIVE_CUSTOM_BLEND | factor(src) << REMIXAPI_UI_NATIVE_SRC_BLEND_SHIFT |
         factor(dst) << REMIXAPI_UI_NATIVE_DST_BLEND_SHIFT;
}

void captureUi(const Draw &d) {
  const auto *pos = findElement(d.elements, D3DDECLUSAGE_POSITION, 0);
  const auto *tex = findElement(d.elements, D3DDECLUSAGE_TEXCOORD, 0);
  const auto *col = findElement(d.elements, D3DDECLUSAGE_COLOR, 0);
  if (!pos || !d.indices.data || !g_displayWidth || !g_displayHeight)
    return;
  // Stencil (see StencilGroup): NEVER only writes and shows nothing; a test
  // names the group this draw is limited to.
  const bool stencilTested = d.stencil && d.stencilFunc != D3DCMP_ALWAYS;
  // 2D marks (round avatars' circles) cannot be mask sources: what tests the
  // value afterwards has no known group.
  if (d.stencil && ((d.stencilFunc == D3DCMP_ALWAYS && d.stencilPass == D3DSTENCILOP_REPLACE) ||
                    (d.stencilFunc == D3DCMP_NEVER && d.stencilFail == D3DSTENCILOP_REPLACE)))
    g_stencilCurrent.erase(d.stencilRef & d.stencilWriteMask & 0xff);
  if (d.stencil && d.stencilFunc == D3DCMP_NEVER) {
    ++g_stats.uiStencil;
    return;
  }
  int maskGroup = -1;
  const bool maskOutside = d.stencilFunc == D3DCMP_NOTEQUAL;
  if (stencilTested) {
    const uint32_t value = d.stencilRef & d.stencilMask & 0xff;
    if (d.stencilFunc == D3DCMP_EQUAL || d.stencilFunc == D3DCMP_NOTEQUAL)
      maskGroup = stencilReader(value);
    else if (d.stencilFunc == D3DCMP_LESS) // ref < stencil: the value above (INCR marks)
      maskGroup = stencilReader(value + 1);
  }
  remixapi_UITextureHandle texture = 0;
  if (d.texture0 && !(texture = uiTextureFor(d.texture0))) {
    ++g_stats.uiNoTexture; // render-target sampling (post-processing): not UI
    // A halo's blurred image, composited once per pass outside the sources.
    if (maskGroup >= 0 && maskOutside) {
      StencilGroup &group = g_stencilGroups[maskGroup];
      group.halo = true;
      ++group.passes;
      // pp/add adds (destination ONE); pp/sub is a Modulate: the runtime's
      // non-additive halo multiplies likewise.
      if (d.dstBlend != D3DBLEND_ONE)
        group.additive = false;
    }
    return;
  }
  // Indices first (the declared vertex range may cover a whole buffer).
  std::vector<uint32_t> indices;
  auto index = [&](uint32_t i, uint32_t &out) {
    const uint64_t at = uint64_t(d.startIndex + i) * (d.indices.is32 ? 4 : 2);
    if (at + (d.indices.is32 ? 4 : 2) > d.indices.size)
      return false;
    uint32_t value = 0;
    std::memcpy(&value, d.indices.data + at, d.indices.is32 ? 4 : 2);
    out = value;
    return value >= d.minIndex;
  };
  if (d.type == D3DPT_TRIANGLELIST) {
    indices.resize(size_t(d.primCount) * 3);
    for (uint32_t i = 0; i < indices.size(); ++i)
      if (!index(i, indices[i]))
        return;
  } else if (d.type == D3DPT_TRIANGLESTRIP) {
    for (uint32_t i = 0; i < d.primCount; ++i) {
      uint32_t a, b, c;
      if (!index(i, a) || !index(i + 1, b) || !index(i + 2, c))
        return;
      if (a == b || b == c || a == c)
        continue;
      if (i & 1)
        std::swap(a, b);
      indices.insert(indices.end(), {a, b, c});
    }
  } else {
    return;
  }
  if (indices.empty())
    return;
  const auto [lo, hi] = std::minmax_element(indices.begin(), indices.end());
  const uint32_t low = *lo, count = *hi - *lo + 1;

  auto fetch = [&](const D3DVERTEXELEMENT9 *e, uint32_t v, uint32_t bytes) -> const uint8_t * {
    const auto &s = d.streams[e->Stream];
    if (!s.data)
      return nullptr;
    const uint64_t at = uint64_t(s.offset) + uint64_t(int64_t(d.baseVertex) + v) * s.stride + e->Offset;
    return at + bytes <= s.size ? s.data + at : nullptr;
  };
  const float *mvp = d.vsConstants + kRegModelViewProj * 4; // register k = column k
  // The material's ColorModulate / AlphaModulate ($color, $alpha), which its
  // shader multiplies in: a screen fade (env_fade, ViewDrawFade) is a white
  // quad with the fade's colour and opacity there, and was drawn white.
  float modulation[4] = {1, 1, 1, 1};
  engine_materials::currentVector("$color", modulation);
  engine_materials::currentFloat("$alpha", modulation[3]);
  uint8_t modulate[4];
  for (int c = 0; c < 4; ++c)
    modulate[c] = uint8_t(std::lround(std::clamp(modulation[c], 0.0f, 1.0f) * 255));
  const bool modulated = modulate[0] != 255 || modulate[1] != 255 || modulate[2] != 255 || modulate[3] != 255;
  const uint32_t base = uint32_t(g_uiVertices.size());
  for (uint32_t v = low; v < low + count; ++v) {
    remixapi_UIVertex out = {};
    float p[3] = {};
    if (const uint8_t *src = fetch(pos, v, 12))
      std::memcpy(p, src, 12);
    float clip[4];
    for (int k = 0; k < 4; ++k)
      clip[k] = p[0] * mvp[k * 4] + p[1] * mvp[k * 4 + 1] + p[2] * mvp[k * 4 + 2] + mvp[k * 4 + 3];
    const float w = std::fabs(clip[3]) > 1e-6f ? clip[3] : 1.0f;
    out.x = (clip[0] / w * 0.5f + 0.5f) * g_displayWidth + 0.5f;
    out.y = (0.5f - clip[1] / w * 0.5f) * g_displayHeight + 0.5f;
    if (tex && tex->Type >= D3DDECLTYPE_FLOAT2 && tex->Type <= D3DDECLTYPE_FLOAT4)
      if (const uint8_t *t = fetch(tex, v, 8)) {
        std::memcpy(&out.u, t, 4);
        std::memcpy(&out.v, t + 4, 4);
      }
    out.color = 0xffffffff;
    if (col && col->Type == D3DDECLTYPE_D3DCOLOR)
      if (const uint8_t *c = fetch(col, v, 4)) // memory B, G, R, A -> R in the low byte
        out.color = uint32_t(c[2]) | uint32_t(c[1]) << 8 | uint32_t(c[0]) << 16 | uint32_t(c[3]) << 24;
    if (modulated) {
      uint32_t color = 0;
      for (int c = 0; c < 4; ++c)
        color |= ((out.color >> (8 * c) & 255) * modulate[c] + 127) / 255 << (8 * c);
      out.color = color;
    }
    g_uiVertices.push_back(out);
  }
  // A stencil-tested draw over the whole screen is a masked fill or composite
  // (halo.lua: surface.DrawRect(0, 0, ScrW(), ScrH()) in the halo colour where
  // stencil == 1; a thermal sight's tint in its lens). With its group known
  // it is masked; otherwise it would cover everything and is dropped. Smaller
  // draws of unknown groups (addons' round avatars) stay, unmasked.
  if (stencilTested) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (size_t i = base; i < g_uiVertices.size(); ++i) {
      x0 = std::min(x0, g_uiVertices[i].x), x1 = std::max(x1, g_uiVertices[i].x);
      y0 = std::min(y0, g_uiVertices[i].y), y1 = std::max(y1, g_uiVertices[i].y);
    }
    const float w = std::min(x1, float(g_displayWidth)) - std::max(x0, 0.0f);
    const float h = std::min(y1, float(g_displayHeight)) - std::max(y0, 0.0f);
    if (w > 0 && h > 0 && w * h >= 0.9f * float(g_displayWidth) * float(g_displayHeight)) {
      if (maskGroup >= 0 && d.stencilFunc == D3DCMP_EQUAL) {
        StencilGroup &group = g_stencilGroups[maskGroup];
        const uint32_t c = g_uiVertices[base].color; // R in the low byte
        group.color[0] = float(c & 255) / 255, group.color[1] = float((c >> 8) & 255) / 255,
        group.color[2] = float((c >> 16) & 255) / 255;
        group.fill = true;
      }
      if (maskGroup < 0) {
        g_uiVertices.resize(base);
        ++g_stats.uiStencil;
        return;
      }
    }
  }
  remixapi_UIDrawCommand command = {};
  command.textureId = texture;
  command.flags = uiBlendFlags(d);
  command.indexCount = uint32_t(indices.size());
  command.indexOffset = uint32_t(g_uiIndices.size());
  for (uint32_t i : indices)
    g_uiIndices.push_back(base + i - low);
  g_uiCommands.push_back(command);
  g_uiCommandMasks.push_back({maskGroup, maskOutside});
  ++g_stats.uiDraws;
}

// Flexes (facial animation, common_vs_fxc.h ApplyMorph). Every model
// declaration ends with POSITION1 (float3, or float4 with the wrinkle weight)
// and NORMAL1 on stream 2. Without flexes that stream is a buffer of zeros
// with stride 0; for a flexed strip group studiorender writes this frame's
// deltas into a dynamic "flex mesh" and binds it there, and the vertex shader
// adds delta * cFlexScale.x to the position and the normal before skinning.
// Returns that scale (0: nothing to add). A flex mesh whose deltas are all
// zero (a face at rest) counts as none: the draw keeps its cached mesh.
float flexScale(const Draw &d) {
  const auto *delta = findElement(d.elements, D3DDECLUSAGE_POSITION, 1);
  if (!delta || delta->Stream >= 4 || (delta->Type != D3DDECLTYPE_FLOAT3 && delta->Type != D3DDECLTYPE_FLOAT4))
    return 0;
  const auto &s = d.streams[delta->Stream];
  if (!s.data || !s.stride)
    return 0;
  const float scale = d.vsConstants[kRegFlexScale * 4];
  if (!std::isfinite(scale) || scale == 0) {
    ++g_stats.flexUnscaled; // a flex stream with the switch off: not this build's register?
    return 0;
  }
  const int64_t begin = int64_t(s.offset) + (int64_t(d.baseVertex) + d.minIndex) * s.stride;
  const int64_t bytes = int64_t(d.numVertices) * s.stride;
  if (begin < 0 || begin + bytes > int64_t(s.size))
    return 0;
  for (int64_t at = 0; at + 4 <= bytes; at += 4) {
    uint32_t word;
    std::memcpy(&word, s.data + begin + at, 4);
    if (word & 0x7fffffff) // anything but +-0
      return scale;
  }
  return 0;
}

// Converts one indexed draw into a Remix mesh. Returns nullptr when the draw
// cannot be represented yet.
float g_liftVertexStep = 0; // added lift per vertex index (render thread; set by onDraw for decals)
// Forwarded vertex colours keep their hue only: each scaled so its largest
// channel is full (render thread; set by onDraw for lit Lua meshes, whose
// colours carry baked shading the path tracer replaces).
bool g_hueColors = false;
// `normalOffset` > 0 lifts the surface off its receiver (decals, see onDraw):
// along the vertex normals, or without them (DecalModulate's vertices are a
// position and a texture coordinate) along the summed face normals, e1 x e2
// times `faceNormalSign` (see faceNormalSign()). `usedFaceNormals`: which.
// `vertexColors`: forward COLOR0 (additive raster-only draws: Source fades and
// hides sprites and beams through it, e.g. a holstered gravity gun's beams
// stay alive at brightness 0); such a draw that is all black adds nothing and
// yields no mesh, unless `keepBlack` (unlit $vertexcolor surfaces: a black one
// is still a surface). `flex`: this frame's flex deltas times it are added to
// the positions and normals (flexScale). `eyeProjection`: the Eyes shader's
// two iris projection registers; the texture coordinates are then that
// projection of the skinned position instead of the vertex buffer's.
// `refresh`: new vertices for the mesh already registered under `hash`
// (g_flexMeshes), or a plain create when there is none. `eyeScale`: the
// projected coordinates are scaled by it about the texture's centre.
remixapi_MeshHandle buildMesh(const Draw &d, uint64_t hash, uint32_t bonesPerVertex, uint32_t *boneCount,
                              float normalOffset = 0, bool vertexColors = false, bool keepBlack = false,
                              float faceNormalSign = 1, bool *usedFaceNormals = nullptr, float flex = 0,
                              const float *eyeProjection = nullptr, bool refresh = false,
                              float eyeScale = 1) {
  Timer timer(g_usBuild, &g_totalUs[1]);
  const auto *pos = findElement(d.elements, D3DDECLUSAGE_POSITION, 0);
  const auto *boneWeights = findElement(d.elements, D3DDECLUSAGE_BLENDWEIGHT, 0);
  const auto *boneIndices = findElement(d.elements, D3DDECLUSAGE_BLENDINDICES, 0);
  const auto *tex = findElement(d.elements, D3DDECLUSAGE_TEXCOORD, 0);
  const auto *nrm = findElement(d.elements, D3DDECLUSAGE_NORMAL, 0);
  const auto *col = vertexColors ? findElement(d.elements, D3DDECLUSAGE_COLOR, 0) : nullptr;
  if (col && col->Type != D3DDECLTYPE_D3DCOLOR)
    col = nullptr;
  const auto *flexPos = flex != 0 ? findElement(d.elements, D3DDECLUSAGE_POSITION, 1) : nullptr;
  const auto *flexNrm = flex != 0 ? findElement(d.elements, D3DDECLUSAGE_NORMAL, 1) : nullptr;
  uint32_t anyRgb = 0;
  const auto &ps = d.streams[pos->Stream];

  std::vector<uint32_t> indices;
  auto index = [&](uint32_t i, uint32_t &out) {
    const uint64_t at = uint64_t(d.startIndex + i) * (d.indices.is32 ? 4 : 2);
    if (at + (d.indices.is32 ? 4 : 2) > d.indices.size)
      return false;
    uint32_t value = 0;
    std::memcpy(&value, d.indices.data + at, d.indices.is32 ? 4 : 2);
    if (value < d.minIndex || value - d.minIndex >= d.numVertices)
      return false;
    out = value - d.minIndex;
    return true;
  };
  if (d.type == D3DPT_TRIANGLELIST) {
    indices.resize(size_t(d.primCount) * 3);
    for (uint32_t i = 0; i < indices.size(); ++i)
      if (!index(i, indices[i])) {
        ++g_stats.badRange;
        return nullptr;
      }
  } else if (d.type == D3DPT_TRIANGLESTRIP) {
    indices.reserve(size_t(d.primCount) * 3);
    for (uint32_t i = 0; i < d.primCount; ++i) {
      uint32_t a, b, c;
      if (!index(i, a) || !index(i + 1, b) || !index(i + 2, c)) {
        ++g_stats.badRange;
        return nullptr;
      }
      if (a == b || b == c || a == c)
        continue;
      if (i & 1)
        std::swap(a, b);
      indices.insert(indices.end(), {a, b, c});
    }
    if (indices.empty())
      return nullptr;
  } else {
    ++g_stats.badPrim;
    return nullptr;
  }

  // Dynamic draws declare the whole buffer as their vertex range; convert only
  // the span the indices reference and rebase the indices onto it.
  const auto [lo, hi] = std::minmax_element(indices.begin(), indices.end());
  const uint32_t low = *lo, count = *hi - *lo + 1;
  for (auto &i : indices)
    i -= low;
  const uint32_t first = uint32_t(d.baseVertex) + d.minIndex + low;
  if (d.streams[pos->Stream].dynamic)
    g_transientVerts += count;

  std::vector<remixapi_HardcodedVertex> vertices(count);
  auto fetch = [&](const D3DVERTEXELEMENT9 *e, uint32_t v, uint32_t bytes) -> const uint8_t * {
    const auto &s = d.streams[e->Stream];
    if (!s.data)
      return nullptr;
    const uint64_t at = uint64_t(s.offset) + uint64_t(first + v) * s.stride + e->Offset;
    return at + bytes <= s.size ? s.data + at : nullptr;
  };
  for (uint32_t v = 0; v < count; ++v) {
    auto &out = vertices[v];
    const uint8_t *p = fetch(pos, v, 12);
    if (!p) {
      ++g_stats.badRange;
      return nullptr;
    }
    std::memcpy(out.position, p, 12);
    if (flexPos) // this frame's flex delta (see flexScale): the bind pose the bones then move
      if (const uint8_t *f = fetch(flexPos, v, 12)) {
        float delta[3];
        std::memcpy(delta, f, 12);
        for (int j = 0; j < 3; ++j)
          out.position[j] += delta[j] * flex;
      }
    if (!std::isfinite(out.position[0]) || !std::isfinite(out.position[1]) || !std::isfinite(out.position[2])) {
      ++g_stats.nonFinite; // the runtime rejects the whole mesh
      return nullptr;
    }
    if (tex && (tex->Type == D3DDECLTYPE_FLOAT2 || tex->Type == D3DDECLTYPE_FLOAT3 ||
                tex->Type == D3DDECLTYPE_FLOAT4)) {
      if (const uint8_t *t = fetch(tex, v, 8))
        std::memcpy(out.texcoord, t, 8);
      if (!std::isfinite(out.texcoord[0]) || !std::isfinite(out.texcoord[1]))
        out.texcoord[0] = out.texcoord[1] = 0;
      if (g_screenUv) {
        const float u = out.texcoord[0], v = out.texcoord[1];
        out.texcoord[0] = g_screenUv[0] * u + g_screenUv[1] * v + g_screenUv[3];
        out.texcoord[1] = g_screenUv[4] * u + g_screenUv[5] * v + g_screenUv[7];
        const float values[4] = {u, v, out.texcoord[0], out.texcoord[1]};
        for (int k = 0; k < 4; ++k) {
          g_screenUvRange[2 * k] = std::min(g_screenUvRange[2 * k], values[k]);
          g_screenUvRange[2 * k + 1] = std::max(g_screenUvRange[2 * k + 1], values[k]);
        }
      }
    }
    if (nrm && nrm->Type == D3DDECLTYPE_UBYTE4) {
      if (const uint8_t *n = fetch(nrm, v, 4))
        unpackNormalUByte4(n, out.normal);
    } else if (nrm && nrm->Type == D3DDECLTYPE_FLOAT3) {
      if (const uint8_t *n = fetch(nrm, v, 12))
        std::memcpy(out.normal, n, 12);
      if (!std::isfinite(out.normal[0]) || !std::isfinite(out.normal[1]) || !std::isfinite(out.normal[2]))
        out.normal[0] = out.normal[1] = out.normal[2] = 0;
    }
    if (flexNrm && flexNrm->Type == D3DDECLTYPE_FLOAT3)
      if (const uint8_t *f = fetch(flexNrm, v, 12)) {
        float delta[3], n[3];
        std::memcpy(delta, f, 12);
        for (int j = 0; j < 3; ++j)
          n[j] = out.normal[j] + delta[j] * flex;
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (std::isfinite(len) && len > 1e-6f)
          for (int j = 0; j < 3; ++j)
            out.normal[j] = n[j] / len;
      }
    out.color = 0xffffffff;
    if (col)
      if (const uint8_t *c = fetch(col, v, 4)) { // D3DCOLOR in memory, the runtime's layout
        std::memcpy(&out.color, c, 4);
        if (g_hueColors) {
          const uint32_t r = (out.color >> 16) & 255, g = (out.color >> 8) & 255, b = out.color & 255;
          const uint32_t top = std::max({r, g, b});
          out.color = top ? (out.color & 0xff000000u) | (r * 255 / top) << 16 | (g * 255 / top) << 8 | b * 255 / top
                          : out.color | 0xffffff;
        }
        anyRgb |= out.color & 0xffffff;
      }
  }
  (void)ps;
  if (col && !anyRgb && !keepBlack) {
    ++g_stats.additiveBlack;
    return nullptr;
  }

  // Source skinning (common_vs_fxc.h): up to three bone indices in a
  // D3DCOLOR (memory bytes 0..2), two stored weights (float2, or short2 / 32767)
  // with the last weight implied; the shader's bone count selects how many apply.
  std::vector<float> weights;
  std::vector<uint32_t> bones;
  uint32_t maxBone = 0, maxWeightedBone = 0;
  if (bonesPerVertex) {
    weights.resize(size_t(count) * bonesPerVertex);
    bones.resize(weights.size());
    for (uint32_t v = 0; v < count; ++v) {
      const uint8_t *bi = fetch(boneIndices, v, 4);
      float stored[2] = {1, 0};
      if (boneWeights && boneWeights->Type == D3DDECLTYPE_FLOAT2) {
        if (const uint8_t *bw = fetch(boneWeights, v, 8))
          std::memcpy(stored, bw, 8);
      } else if (boneWeights && boneWeights->Type == D3DDECLTYPE_SHORT2) {
        if (const uint8_t *bw = fetch(boneWeights, v, 4)) {
          int16_t packed[2];
          std::memcpy(packed, bw, 4);
          stored[0] = packed[0] / 32767.0f;
          stored[1] = packed[1] / 32767.0f;
        }
      }
      float w[3] = {1, 0, 0};
      if (bonesPerVertex == 2) {
        w[0] = stored[0];
        w[1] = 1 - stored[0];
      } else if (bonesPerVertex == 3) {
        w[0] = stored[0];
        w[1] = stored[1];
        w[2] = 1 - stored[0] - stored[1];
      }
      for (uint32_t k = 0; k < bonesPerVertex; ++k) {
        const uint32_t bone = bi ? bi[k] : 0;
        // The implied weight can round slightly below zero; the runtime rejects negatives.
        weights[size_t(v) * bonesPerVertex + k] = std::isfinite(w[k]) ? std::clamp(w[k], 0.0f, 1.0f) : 0;
        bones[size_t(v) * bonesPerVertex + k] = bone;
        maxBone = std::max(maxBone, bone);
        if (weights[size_t(v) * bonesPerVertex + k] > 0)
          maxWeightedBone = std::max(maxWeightedBone, bone);
      }
    }
    if (maxBone >= kMaxBones) {
      ++g_stats.badRange;
      return nullptr;
    }
  }

  // Eyes shader (eyes_vs20): the iris is a planar projection of the world
  // position, u = dot(c50, (world, 1)), v = dot(c51, (world, 1)), which
  // studiorender aims along the eyeball's gaze every frame. The world position
  // is this bind-pose vertex moved by its bones, as the runtime will skin it.
  if (eyeProjection) {
    const float *model = d.vsConstants + kRegModel * 4;
    for (uint32_t v = 0; v < count; ++v) {
      const float *p = vertices[v].position;
      float world[3] = {0, 0, 0};
      for (uint32_t k = 0; k < std::max(bonesPerVertex, 1u); ++k) {
        const float *m = model + (bonesPerVertex ? bones[size_t(v) * bonesPerVertex + k] : 0) * 12;
        const float w = bonesPerVertex ? weights[size_t(v) * bonesPerVertex + k] : 1.0f;
        for (int r = 0; r < 3; ++r)
          world[r] += w * (m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3]);
      }
      for (int a = 0; a < 2; ++a) {
        const float *e = eyeProjection + a * 4;
        const float t = e[0] * world[0] + e[1] * world[1] + e[2] * world[2] + e[3];
        vertices[v].texcoord[a] = std::isfinite(t) ? (t - 0.5f) * eyeScale + 0.5f : 0;
      }
    }
  }

  // Which way triangles wind (faceNormalSign()): one vote per mesh that has
  // vertex normals, from its first triangle.
  if (nrm && d.cullMode == D3DCULL_CCW && indices.size() >= 3) {
    const auto &a = vertices[indices[0]], &b = vertices[indices[1]], &c = vertices[indices[2]];
    const float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
    const float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
    const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    float along = 0;
    for (int j = 0; j < 3; ++j)
      along += n[j] * (a.normal[j] + b.normal[j] + c.normal[j]);
    if (along > 0)
      ++g_stats.windingAlong;
    else if (along < 0)
      ++g_stats.windingAgainst;
  }
  if (normalOffset > 0) {
    // Vertex normal where present, else the summed face normals.
    bool anyFace = false;
    std::vector<float> faces(vertices.size() * 3, 0.0f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
      const float *a = vertices[indices[i]].position, *b = vertices[indices[i + 1]].position,
                  *c = vertices[indices[i + 2]].position;
      const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
      for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j)
          faces[size_t(indices[i + k]) * 3 + j] += n[j];
    }
    for (size_t v = 0; v < vertices.size(); ++v) {
      const float *vn = vertices[v].normal, *fn = &faces[v * 3];
      const float vlen = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
      const float flen = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
      const float *n = vlen > 1e-6f ? vn : fn;
      const float len = vlen > 1e-6f ? vlen : flen;
      const float sign = vlen > 1e-6f ? 1.0f : faceNormalSign;
      anyFace |= !(vlen > 1e-6f);
      if (len > 1e-12f)
        for (int j = 0; j < 3; ++j)
          vertices[v].position[j] += n[j] / len * (normalOffset + std::min(float(v) * g_liftVertexStep, 0.3f)) * sign;
    }
    if (usedFaceNormals)
      *usedFaceNormals = anyFace;
  }

  remixapi_MeshInfoSurfaceTriangles surface = {};
  surface.vertices_values = vertices.data();
  surface.vertices_count = vertices.size();
  surface.indices_values = indices.data();
  surface.indices_count = indices.size();
  // Every vertex on bone 0 alone (props, most physics models: Source draws
  // them through the skinning shader with one matrix): a rigid mesh, placed
  // by that matrix as its instance transform. One mesh and one acceleration
  // structure for all its instances, rigid motion vectors, and nothing for
  // the runtime to skin or to treat as a deforming surface.
  const bool rigid = bonesPerVertex && maxWeightedBone == 0;
  if (bonesPerVertex && !rigid) {
    surface.skinning_hasvalue = 1;
    surface.skinning_value.bonesPerVertex = bonesPerVertex;
    surface.skinning_value.blendWeights_values = weights.data();
    surface.skinning_value.blendWeights_count = uint32_t(weights.size());
    surface.skinning_value.blendIndices_values = bones.data();
    surface.skinning_value.blendIndices_count = uint32_t(bones.size());
  }
  if (boneCount) // 1: rigid (see above)
    *boneCount = !bonesPerVertex ? 0 : rigid ? 1 : std::max(maxBone + 1, 2u);
  remixapi_MeshInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
  info.hash = hash ? hash : 1;
  info.surfaces_values = &surface;
  info.surfaces_count = 1;
  // The mesh registered under this hash, deformed: it stays the same object.
  remixapi_MeshInfoDeformEXT deform = {};
  deform.sType = REMIXAPI_STRUCT_TYPE_MESH_DEFORM_EXT;
  if (refresh)
    info.pNext = &deform;
  remixapi_MeshHandle handle = nullptr;
  if (g_api.CreateMesh(&info, &handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
    ++g_stats.rejected;
    return nullptr;
  }
  ++g_stats.created;
  return handle;
}

LRESULT CALLBACK hiddenProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

remixapi_ErrorCode g_modsResult = REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;

// Dev aid: `bin/remix_screenshot.request` (its contents: an absolute PNG path,
// or empty for bin/remix_screenshot.png) asks for a screenshot of the next
// presented frame.
// Dev aid: `shaderapi_proxy_flat.request` (next to the DLL) appends every flat
// (non-perspective) draw of the next three frames to shaderapi_proxy_flat.txt:
// material, shader, texture, blend state, colour and screen bounds, and the
// first pixel shader registers (effect parameters).
int g_dumpFlatFrames = 0;   // render thread
FILE *g_dumpFlat = nullptr; // render thread
// Dev aid: bin/shaderapi_proxy_material.request names a texture ("hunter/
// myplastic"); the next draws of it are described in bin/material_probe.txt:
// the material bound, the shading taken and its PBR conversion.
struct MaterialProbe {
  std::vector<uint64_t> hashes;
  std::string name;
  uint32_t left = 0;
} g_materialProbe; // render thread
void serviceMaterialProbeRequest() {
  if (g_frame % 16)
    return;
  const std::wstring request = g_moduleDir + L"\\shaderapi_proxy_material.request";
  FILE *f = _wfopen(request.c_str(), L"r");
  if (!f)
    return;
  char line[256] = {};
  if (!fgets(line, sizeof(line), f))
    line[0] = 0;
  fclose(f);
  DeleteFileW(request.c_str());
  std::string name = line;
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
    name.pop_back();
  g_materialProbe.name = name;
  g_materialProbe.hashes = bsp::textureCandidates(name);
  g_materialProbe.left = 64;
  if (FILE *out = _wfopen((g_moduleDir + L"\\material_probe.txt").c_str(), L"w")) {
    fprintf(out, "== %s: %zu candidate hashes, from frame %llu\n", name.c_str(), g_materialProbe.hashes.size(),
            static_cast<unsigned long long>(g_frame));
    fclose(out);
  }
}
void probeMaterialDraw(const void *texture, const engine_materials::Shading &bound, const engine_materials::Shading &taken,
                       bool additive, bool multiply) {
  if (!g_materialProbe.left || !texture)
    return;
  const uint64_t hash = textureHash(texture);
  const auto &h = g_materialProbe.hashes;
  if (std::find(h.begin(), h.end(), hash) == h.end())
    return;
  --g_materialProbe.left;
  FILE *out = _wfopen((g_moduleDir + L"\\material_probe.txt").c_str(), L"a");
  if (!out)
    return;
  const auto &b = engine_materials::bound();
  fprintf(out, "f%llu texture %016llX | bound %s (%s, flags %08X) base %s pbr %d | taken mode %d base %s pbr %d normal %s rough %s ch %u bias %g scale %g, roughness %g%s%s\n",
          static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(hash), engine_materials::currentName(),
          b.known ? b.shader : "?", unsigned(b.flags), bound.base.c_str(), int(bound.pbr.active), int(taken.mode),
          taken.base.c_str(), int(taken.pbr.active), taken.pbr.normal.c_str(), taken.pbr.maps[0].texture.c_str(),
          taken.pbr.maps[0].channel, taken.pbr.maps[0].bias, taken.pbr.maps[0].scale, taken.pbr.roughness,
          additive ? " additive" : "", multiply ? " multiply" : "");
  fclose(out);
}

// Dev aid: bin/shaderapi_proxy_shader.request names a shader
// ("shatteredglass"); its next 64 draws are described in bin/shader_probe.txt:
// the material, its $basetexture as bound (after material proxies), each
// sampler's texture (hash; whether the runtime has it), blending and the
// shading taken.
struct ShaderProbe {
  std::string shader;
  uint32_t left = 0;
} g_shaderProbe; // render thread
void serviceShaderProbeRequest() {
  if (g_frame % 16)
    return;
  const std::wstring request = g_moduleDir + L"\\shaderapi_proxy_shader.request";
  if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES)
    return;
  std::string name;
  if (FILE *in = _wfopen(request.c_str(), L"r")) {
    char line[256] = {};
    if (std::fgets(line, sizeof(line), in))
      name = line;
    fclose(in);
  }
  DeleteFileW(request.c_str());
  while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
    name.pop_back();
  for (char &c : name)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  g_shaderProbe.shader = name;
  g_shaderProbe.left = name.empty() ? 0 : 64;
  if (FILE *out = _wfopen((g_moduleDir + L"\\shader_probe.txt").c_str(), L"w")) {
    fprintf(out, "== shader %s, from frame %llu\n", name.c_str(), static_cast<unsigned long long>(g_frame));
    fclose(out);
  }
}
void probeShaderDraw(const Draw &d, const engine_materials::Shading &taken) {
  if (!g_shaderProbe.left)
    return;
  const auto &b = engine_materials::bound();
  if (!b.known)
    return;
  std::string shader = b.shader;
  for (char &c : shader)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  if (shader.rfind(g_shaderProbe.shader, 0) != 0)
    return;
  --g_shaderProbe.left;
  FILE *out = _wfopen((g_moduleDir + L"\\shader_probe.txt").c_str(), L"a");
  if (!out)
    return;
  fprintf(out, "f%llu %s (%s, translucent %d alphaTest %d) base \"%s\" | blend %d %u/%u alphaTest %d | taken mode %d base \"%s\" | verts %u prims %u\n",
          static_cast<unsigned long long>(g_frame), engine_materials::currentName(), b.shader, int(b.translucent),
          int(b.alphaTest), engine_materials::current().base.c_str(), int(d.blend), d.srcBlend, d.dstBlend, int(d.alphaTest),
          int(taken.mode), taken.base.c_str(), d.numVertices, d.primCount);
  // The vertex layout, and the first vertex's texture coordinates.
  if (d.elements) {
    fprintf(out, "   elements:");
    for (const D3DVERTEXELEMENT9 *e = d.elements; e->Stream != 0xff; ++e) {
      fprintf(out, " s%u+%u u%u.%u t%u", e->Stream, e->Offset, e->Usage, e->UsageIndex, e->Type);
      const auto &st = d.streams[e->Stream < 4 ? e->Stream : 0];
      const uint64_t at = uint64_t(st.offset) + uint64_t(int64_t(d.baseVertex) + d.minIndex) * st.stride + e->Offset;
      if (e->Usage == D3DDECLUSAGE_TEXCOORD && e->Stream < 4 && st.data && e->Type <= D3DDECLTYPE_FLOAT4 &&
          at + (e->Type + 1) * 4 <= st.size) {
        float v[4] = {};
        std::memcpy(v, st.data + at, (e->Type + 1) * 4);
        fprintf(out, "=(%.3g %.3g %.3g %.3g)", v[0], v[1], v[2], v[3]);
      }
    }
    fprintf(out, "\n");
  }
  for (int s = 0; s < 8; ++s)
    if (d.stageTextures[s]) {
      const uint64_t hash = textureHash(d.stageTextures[s]);
      std::lock_guard<std::recursive_mutex> lock(g_textureLock);
      fprintf(out, "   s%d %p hash %016llX%s\n", s, d.stageTextures[s], static_cast<unsigned long long>(hash),
              hash && uploadedTextureHash(hash) ? " (runtime has it)" : hash ? "" : " (no upload seen)");
    }
  fclose(out);
}

void serviceFlatDumpRequest() {
  if (g_dumpFlatFrames) {
    if (--g_dumpFlatFrames == 0 && g_dumpFlat) {
      fclose(g_dumpFlat);
      g_dumpFlat = nullptr;
    }
    return;
  }
  if (g_frame % 16)
    return;
  const std::wstring request = g_moduleDir + L"\\shaderapi_proxy_flat.request";
  if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES)
    return;
  DeleteFileW(request.c_str());
  g_dumpFlat = _wfopen((g_moduleDir + L"\\shaderapi_proxy_flat.txt").c_str(), L"a");
  if (g_dumpFlat) {
    fprintf(g_dumpFlat, "== frames from %llu\n", (unsigned long long)g_frame);
    g_dumpFlatFrames = 3;
  }
}
void dumpFlat(const Draw &d) {
  if (!g_dumpFlat)
    return;
  const auto *pos = findElement(d.elements, D3DDECLUSAGE_POSITION, 0);
  const auto *col = findElement(d.elements, D3DDECLUSAGE_COLOR, 0);
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  uint32_t color = 0xffffffff;
  if (pos && d.vsConstants) {
    const float *mvp = d.vsConstants + kRegModelViewProj * 4;
    const auto &s = d.streams[pos->Stream];
    for (uint32_t v = d.minIndex; v < d.minIndex + d.numVertices && s.data; ++v) {
      const uint64_t at = uint64_t(s.offset) + uint64_t(int64_t(d.baseVertex) + v) * s.stride;
      if (at + pos->Offset + 12 > s.size)
        break;
      float p[3];
      std::memcpy(p, s.data + at + pos->Offset, 12);
      float clip[4];
      for (int k = 0; k < 4; ++k)
        clip[k] = p[0] * mvp[k * 4] + p[1] * mvp[k * 4 + 1] + p[2] * mvp[k * 4 + 2] + mvp[k * 4 + 3];
      const float w = std::fabs(clip[3]) > 1e-6f ? clip[3] : 1.0f;
      const float x = (clip[0] / w * 0.5f + 0.5f) * g_displayWidth, y = (0.5f - clip[1] / w * 0.5f) * g_displayHeight;
      x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
      if (v == d.minIndex && col && col->Type == D3DDECLTYPE_D3DCOLOR && at + col->Offset + 4 <= s.size &&
          col->Stream == pos->Stream)
        std::memcpy(&color, s.data + at + col->Offset, 4);
    }
  }
  const auto &b = engine_materials::bound();
  fprintf(g_dumpFlat,
          "f%llu %s [%s] tex0=%p%s%s tex1=%p blend=%d %u/%u op%u sep=%d write=%x alphaTest=%d stencil=%d z=%d prim=%u verts=%u "
          "color(argb)=%08x bounds=%.0f,%.0f-%.0f,%.0f target=%ux%u main=%d",
          (unsigned long long)g_frame, engine_materials::currentName(), b.known ? b.shader : "?", d.texture0,
          d.texture0Target ? "(target)" : "", d.texture0 && !uiTextureFor(d.texture0) ? "(no ui)" : "", d.texture1,
          int(d.blend), d.srcBlend, d.dstBlend, d.blendOp, int(d.separateAlpha), d.colorWrite, int(d.alphaTest),
          int(d.stencil), int(d.depthTest), d.primCount, d.numVertices, color, x0, y0, x1, y1, d.targetWidth,
          d.targetHeight, int(d.mainTarget));
  if (d.psConstants) {
    fprintf(g_dumpFlat, " ps:");
    for (int r = 0; r < 8; ++r)
      fprintf(g_dumpFlat, " c%d=(%.3g %.3g %.3g %.3g)", r, d.psConstants[r * 4], d.psConstants[r * 4 + 1],
              d.psConstants[r * 4 + 2], d.psConstants[r * 4 + 3]);
  }
  fprintf(g_dumpFlat, "\n");
}

void serviceScreenshotRequest() {
  // Checked every 16th frame: a file system call per frame shows in profiles.
  if (g_frame % 16)
    return;
  // Dev aid: shaderapi_proxy_ui.request toggles the developer panel (posted to
  // the window, whose thread owns the panel's input).
  const std::wstring ui = g_moduleDir + L"\\shaderapi_proxy_ui.request";
  if (GetFileAttributesW(ui.c_str()) != INVALID_FILE_ATTRIBUTES) {
    DeleteFileW(ui.c_str());
    PostMessageW(g_gameWindow, remix_ui::kToggleMessage, 0, 0);
  }
  const std::wstring request = g_moduleDir + L"\\remix_screenshot.request";
  if (GetFileAttributesW(request.c_str()) == INVALID_FILE_ATTRIBUTES)
    return;
  char path[MAX_PATH] = {};
  if (FILE *f = _wfopen(request.c_str(), L"r")) {
    if (!fgets(path, sizeof(path), f))
      path[0] = 0;
    fclose(f);
  }
  DeleteFileW(request.c_str());
  path[strcspn(path, "\r\n")] = 0;
  if (!path[0]) {
    WideCharToMultiByte(CP_UTF8, 0, (g_moduleDir + L"\\remix_screenshot.png").c_str(), -1, path, sizeof(path),
                        nullptr, nullptr);
  }
  g_api.RequestPresentedScreenshot(path);
}

bool startRemix() {
  wchar_t path[MAX_PATH];
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&startRemix), &self);
  GetModuleFileNameW(self, path, MAX_PATH);
  std::wstring dll = path;
  g_moduleDir = dll.substr(0, dll.find_last_of(L"\\/"));
  // OpenRemix runs in process (GMod is x64): bin/win64/openremix/remix.dll,
  // its dependencies (SDL3, OpenUSD, DLSS) resolved from that directory.
  dll = g_moduleDir + L"\\openremix\\remix.dll";
  diag::stage("OpenRemix: loading openremix\\remix.dll");
  if (!LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
    // 126: the file or one of the DLLs it needs is missing; 193: wrong architecture.
    diag::stage("OpenRemix: FAILED to load remix.dll (error %lu)", GetLastError());
    return false;
  }
  diag::stage("OpenRemix: remix.dll loaded, initializing the API");
  remixapi_HMODULE lib = nullptr;
  if (const auto status = remixapi_lib_loadRemixDllAndInitialize(dll.c_str(), &g_api, &lib);
      status != REMIXAPI_ERROR_CODE_SUCCESS) {
    diag::stage("OpenRemix: API initialization FAILED (remixapi_ErrorCode %d)", int(status));
    return false;
  }
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.hwnd = g_gameWindow;
  // Before SDL wraps the window in Startup: the game's own window procedure.
  const auto gameProc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC));
  diag::stage("OpenRemix: Startup (window %p): Vulkan device and swapchain", static_cast<void *>(g_gameWindow));
  if (const auto status = g_api.Startup(&startup); status != REMIXAPI_ERROR_CODE_SUCCESS) {
    diag::stage("OpenRemix: Startup FAILED (remixapi_ErrorCode %d)", int(status));
    return false;
  }
  diag::stage("OpenRemix: Startup succeeded");
  remix_ui::install(g_gameWindow, &g_api, gameProc);
  // OPENREMIX_PROXY_BACKEND=pathtrace|raster|forwardplus (runtime default: raster).
  char backend[32] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_BACKEND", backend, sizeof(backend)))
    g_api.SetConfigVariable("rtx.native.backend", backend);
  // USD mod replacements from the game's rtx-remix/mods (OPENREMIX_PROXY_MODS:
  // another directory, or 0 to disable). Z up.
  {
    char mods[MAX_PATH * 2] = {};
    if (!GetEnvironmentVariableA("OPENREMIX_PROXY_MODS", mods, sizeof(mods))) {
      GetModuleFileNameA(nullptr, mods, MAX_PATH);
      *strrchr(mods, '\\') = 0;
      strcat(mods, "\\rtx-remix\\mods");
    }
    auto configure = reinterpret_cast<decltype(&remixapi_ConfigureUSDModsEXT)>(
        GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_ConfigureUSDModsEXT"));
    if (configure && std::strcmp(mods, "0") != 0) {
      remixapi_USDModsOptionsEXT options = {};
      options.structSize = sizeof(options);
      options.version = 1;
      options.directory = mods;
      // Remix captures and mods declare metersPerUnit 0.01 but hold game units,
      // which dxvk-remix applies unscaled: match the declaration.
      options.hostMetersPerUnit = 0.01;
      options.hostUpAxis = 2;
      char watch[8] = {};
      options.watch = !(GetEnvironmentVariableA("OPENREMIX_PROXY_MODS_WATCH", watch, sizeof(watch)) && watch[0] == '0');
      g_modsResult = configure(&options);
    }
  }
  {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t *slash = wcsrchr(exe, L'\\'))
      *slash = 0;
    categories::load(exe);
  }
  // The proxy's choices are defaults (remixapi_SetConfigDefaultEXT): a value
  // the user saved in their profile stays (set at every start, they used to
  // undo it: ReSTIR DI came back on). An older runtime without it takes them
  // as plain settings.
  static const auto configDefault = reinterpret_cast<PFN_remixapi_SetConfigDefaultEXT>(
      reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_SetConfigDefaultEXT")));
  auto setDefault = [](const char *key, const char *value) {
    if (!configDefault || configDefault(key, value) != REMIXAPI_ERROR_CODE_SUCCESS)
      g_api.SetConfigVariable(key, value);
  };
  source_rendering::lightingDefaults(setDefault);
  // Static world brushes (onPresent) would otherwise make every ray traverse
  // a static and a dynamic acceleration structure.
  setDefault("rtx.native.pathtrace.combinedTlas", "1");
  // Flexed faces are refreshed in place (g_flexMeshes).
  setDefault("rtx.native.meshes.refreshInPlace", "1");
  // The view model is path-traced in front of the scene (see the cameras in
  // onPresent).
  setDefault("rtx.native.pathtrace.viewModel", "1");
  // OPENREMIX_PROXY_CONFIG="key=value;key=value": runtime settings at startup.
  char config[2048] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_CONFIG", config, sizeof(config))) {
    for (char *entry = strtok(config, ";"); entry; entry = strtok(nullptr, ";")) {
      if (char *eq = strchr(entry, '=')) {
        *eq = 0;
        g_api.SetConfigVariable(entry, eq + 1);
      }
    }
  }
  char gpuStats[8] = {}, frameProfile[2] = {};
  if ((GetEnvironmentVariableA("OPENREMIX_PROXY_GPU_STATS", gpuStats, sizeof(gpuStats)) && gpuStats[0] == '1') ||
      GetEnvironmentVariableA("OPENREMIX_PROXY_FRAME_PROFILE", frameProfile, sizeof(frameProfile))) {
    const auto result = g_api.SetConfigVariable("rtx.native.gpuPassTimers", "1");
    diag::stage("GPU profiling: native pass timer request returned %u", unsigned(result));
  }
  return true;
}

} // namespace

bool enabled() {
  static const bool on = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY", v, sizeof(v)) && v[0] == '0');
  }();
  return on;
}

void setShaderApi(void *real) { g_shaderApi = real; }

bool viewMatrices(float view[16], float projection[16]) {
  if (!g_shaderApi)
    return false;
  getMatrix(kMatrixView, view);
  getMatrix(kMatrixProjection, projection);
  return true;
}

bool isSkyDraw(const Draw &d) {
  return g_ready && g_worldLists > 0 && d.mainTarget && !d.blend && d.streams[0].dynamic && d.colorWrite;
}

// --- Sky dome ----------------------------------------------------------------
// The captured 2D skybox, resampled to the dome's equirectangular layout
// (u = atan2(z, x) / 2pi + 0.5, v = acos(y) / pi in dome space) and lit at
// OPENREMIX_PROXY_SKY_DOME_SCALE. The dome transform swaps y and z, so the
// poles (rows that collapse to a point) are Source's up and down and the
// seam is a meridian; with the identity they lay on the horizon at +-y. The
// swap is its own inverse: either reading of the transform gives the same map.
remixapi_LightHandle g_skyDome = nullptr;
BakeSky g_bakeSky;
lighting::StyledLights g_styledBakeLights;
remixapi_TextureHandle g_skyTexture = nullptr;
uint64_t g_skyTextureHash = 0;
uint32_t g_skyEpoch = 0;
// The dome is resampled from the faces off the game's thread (2M texels) and
// handed to the runtime at a later Present (applySkyDome).
struct SkyJob {
  std::thread thread;
  std::atomic<bool> done{false};
  bool running = false;
  bool slight = false; // a slight difference from the dome makes a new one
  bool update = false; // its verdict: rgba is a new dome
  uint32_t epoch = 0;
  uint64_t hash = 0;
  std::vector<uint8_t> faces[6], rgba;
  std::vector<uint16_t> reference, blocks; // g_skyBlocks then; these faces'
} g_skyJob;
// Per face, the sums of its 8x8 texel blocks (B, G, R) of the faces the dome
// was last made from: what a capture is compared with.
std::vector<uint16_t> g_skyBlocks;
ULONGLONG g_skyDomeTick = 0; // when the dome was last replaced
bool g_skyChanged = false;   // a dome was made since takeSkyChanged

uint32_t skyEpoch() { return g_skyEpoch; }

// A new map: the dome of the previous one must not outlive it (a map whose
// 2D sky is not drawn, or not yet, falls back to the sky light estimate).
static void resetSky(const char *mapName) {
  g_styledBakeLights.reset(g_api);
  g_bakeSky.reset(g_api, mapName && *mapName ? XXH3_64bits(mapName, std::strlen(mapName)) | 1 : 0);
  if (g_skyDome)
    g_api.DestroyLight(g_skyDome);
  if (g_skyTexture)
    g_api.DestroyTexture(g_skyTexture);
  g_skyDome = nullptr;
  g_skyTexture = nullptr;
  g_skyTextureHash = 0;
  g_skyBlocks.clear();
  ++g_skyEpoch;
}

// Both the ordered engine marker and external hosts announce map lifecycle
// events here. Keep the live and baked environment lifetime identical on both
// entry points, including unloads and failed map loads.
bool loadMap(const char *name) {
  if (!g_ready)
    return false;
  resetSky(name);
  diag::stage("Map lifecycle: %s", name && *name ? name : "unload");
  return bsp::load(g_api, name ? name : "", engine_lights::stylesActive());
}

void setSkyFaces(const uint8_t *const faces[6]) {
  if (!g_ready || g_skyJob.running)
    return; // the capture before this one is still being looked at
  if (g_skyJob.thread.joinable())
    g_skyJob.thread.join();
  constexpr uint32_t S = proxy::sky::kFaceSize;
  for (int f = 0; f < 6; ++f)
    g_skyJob.faces[f].assign(faces[f], faces[f] + size_t(S) * S * 4);
  g_skyJob.reference = g_skyBlocks;
  g_skyJob.slight = GetTickCount64() - g_skyDomeTick >= 10000;
  g_skyJob.epoch = g_skyEpoch;
  g_skyJob.update = false;
  g_skyJob.done = false;
  g_skyJob.running = true;
  g_skyJob.thread = std::thread([] {
    constexpr uint32_t kWidth = proxy::sky::kDomeWidth, kHeight = proxy::sky::kDomeHeight, S = proxy::sky::kFaceSize;
    // The faces identify the dome: unchanged ones cost neither the resampling
    // nor a texture upload. A painted sky (g_sky) is dithered anew every
    // frame and its stars drift, so no two captures of it are equal; what
    // counts is how far a block of texels' mean is from the dome's (levels of
    // 255). A large difference (another sky, a face first seen) makes a new
    // dome at once, a slight one (drift, a slow day) only now and then: a new
    // dome costs an upload and restarts the runtime's sampling of it.
    constexpr uint32_t kBlock = 8, kBlocks = S / kBlock;
    constexpr int kSlight = 2 * int(kBlock * kBlock), kLarge = 24 * int(kBlock * kBlock);
    auto &blocks = g_skyJob.blocks;
    blocks.assign(size_t(6) * kBlocks * kBlocks * 3, 0);
    for (int f = 0; f < 6; ++f)
      for (uint32_t y = 0; y < kBlocks * kBlock; ++y) {
        const uint8_t *in = g_skyJob.faces[f].data() + size_t(y) * S * 4;
        uint16_t *out = &blocks[(size_t(f) * kBlocks + y / kBlock) * kBlocks * 3];
        for (uint32_t x = 0; x < kBlocks; ++x, out += 3)
          for (uint32_t k = 0; k < kBlock; ++k, in += 4) {
            out[0] += in[0];
            out[1] += in[1];
            out[2] += in[2];
          }
      }
    int difference = kLarge + 1; // no dome yet
    if (g_skyJob.reference.size() == blocks.size()) {
      difference = 0;
      for (size_t i = 0; i < blocks.size(); ++i)
        difference = std::max(difference, std::abs(int(blocks[i]) - int(g_skyJob.reference[i])));
    }
    if (difference > kLarge || (difference > kSlight && g_skyJob.slight)) {
      uint64_t hash = kWidth;
      for (int f = 0; f < 6; ++f)
        hash = XXH3_64bits_withSeed(g_skyJob.faces[f].data(), g_skyJob.faces[f].size(), hash);
      g_skyJob.hash = hash | 1;
      const auto &faces = g_skyJob.faces;
      auto &rgba = g_skyJob.rgba; // kept (8 MiB)
      float bases[6][3][3];
      for (int f = 0; f < 6; ++f)
        proxy::sky::faceBasis(f, bases[f][0], bases[f][1], bases[f][2]);
      std::vector<float> cosPhi(kWidth), sinPhi(kWidth);
      for (uint32_t u = 0; u < kWidth; ++u) {
        const float phi = ((u + 0.5f) / kWidth - 0.5f) * 6.28318531f;
        cosPhi[u] = std::cos(phi);
        sinPhi[u] = std::sin(phi);
      }
      rgba.resize(size_t(kWidth) * kHeight * 4);
      for (uint32_t v = 0; v < kHeight; ++v) {
        const float theta = (v + 0.5f) / kHeight * 3.14159265f;
        const float sinTheta = std::sin(theta), cosTheta = std::cos(theta);
        uint8_t *out = &rgba[size_t(v) * kWidth * 4];
        for (uint32_t u = 0; u < kWidth; ++u, out += 4) {
          // Dome direction (sin cos, cos, sin sin) with y and z swapped: world.
          const float w[3] = {sinTheta * cosPhi[u], sinTheta * sinPhi[u], cosTheta};
          // The face of the major axis (faceBasis order: +x -x +y -y +z -z).
          const float ax = std::fabs(w[0]), ay = std::fabs(w[1]), az = std::fabs(w[2]);
          const int axis = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
          const int face = axis * 2 + (w[axis] < 0);
          const float inv = 1 / std::max(std::fabs(w[axis]), 1e-6f);
          const float x = (w[0] * bases[face][0][0] + w[1] * bases[face][0][1] + w[2] * bases[face][0][2]) * inv;
          const float y = (w[0] * bases[face][1][0] + w[1] * bases[face][1][1] + w[2] * bases[face][1][2]) * inv;
          // Bilinear, texel centres at half-integers (the capture offsets its
          // projection to match); the last half texel at a face's edge clamps.
          const float px = std::clamp((x * 0.5f + 0.5f) * S - 0.5f, 0.f, S - 1.001f);
          const float py = std::clamp((0.5f - y * 0.5f) * S - 0.5f, 0.f, S - 1.001f);
          const uint32_t x0 = uint32_t(px), y0 = uint32_t(py);
          const float fx = px - x0, fy = py - y0;
          const uint8_t *t0 = faces[face].data() + (size_t(y0) * S + x0) * 4, *t1 = t0 + size_t(S) * 4;
          for (int c = 0; c < 3; ++c) {
            const int k = 2 - c; // BGRA -> RGBA
            const float value = (t0[k] * (1 - fx) + t0[4 + k] * fx) * (1 - fy) + (t1[k] * (1 - fx) + t1[4 + k] * fx) * fy;
            out[c] = uint8_t(value + 0.5f);
          }
          out[3] = 255;
        }
      }
      g_skyJob.update = true;
    }
    g_skyJob.done = true;
  });
}

bool takeSkyChanged() { return std::exchange(g_skyChanged, false); }

// At Present: the dome a capture started, once it has been resampled.
void applySkyDome() {
  if (!g_skyJob.running || !g_skyJob.done)
    return;
  g_skyJob.thread.join();
  g_skyJob.running = false;
  if (g_skyJob.epoch != g_skyEpoch || !g_skyJob.update)
    return; // of the previous map; the dome's sky still
  constexpr uint32_t kWidth = proxy::sky::kDomeWidth, kHeight = proxy::sky::kDomeHeight;
  const uint64_t hash = g_skyJob.hash;
  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.hash = hash;
  info.width = kWidth;
  info.height = kHeight;
  info.depth = 1;
  info.mipLevels = 1;
  info.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  info.data = g_skyJob.rgba.data();
  info.dataSize = g_skyJob.rgba.size();
  remixapi_TextureHandle texture = nullptr;
  if (g_api.CreateTexture(&info, &texture) != REMIXAPI_ERROR_CODE_SUCCESS)
    return; // the next capture tries again
  static const float domeScale = envScale("OPENREMIX_PROXY_SKY_DOME_SCALE", 1.0f);
  wchar_t path[32];
  std::swprintf(path, 32, L"0x%llx", static_cast<unsigned long long>(hash));
  remixapi_LightInfoDomeEXT dome = {};
  dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
  dome.transform = {{{1, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 0, 0}}};
  dome.colorTexture = path;
  remixapi_LightInfo light = {};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &dome;
  light.hash = 0x5b7d0e0000000001ull;
  light.radiance = {domeScale, domeScale, domeScale};
  remixapi_LightHandle handle = nullptr;
  if (g_api.CreateLight(&light, &handle) != REMIXAPI_ERROR_CODE_SUCCESS) {
    g_api.DestroyTexture(texture);
    return;
  }
  if (g_skyDome && g_skyDome != handle)
    g_api.DestroyLight(g_skyDome);
  if (g_skyTexture && g_skyTexture != texture)
    g_api.DestroyTexture(g_skyTexture);
  g_skyDome = handle;
  g_skyTexture = texture;
  g_skyTextureHash = hash;
  g_skyBlocks = g_skyJob.blocks;
  g_skyDomeTick = GetTickCount64();
  g_skyChanged = true;
  if (!g_bakeSky.capture(g_api, info, domeScale))
    diag::stage("Map-load bake sky capture failed; a later changed sky capture will retry");
}

void onCreateDevice(HWND gameWindow, D3DPRESENT_PARAMETERS *pp) {
  if (!enabled() || !pp)
    return;
  g_gameWindow = gameWindowForDevice(g_gameWindow, g_hiddenWindow, gameWindow, pp->hDeviceWindow);
  diag::stage("D3D host window: focus %p, requested %p, game %p, hidden %p", gameWindow,
              pp->hDeviceWindow, g_gameWindow, g_hiddenWindow);
  g_displayWidth = pp->BackBufferWidth;
  g_displayHeight = pp->BackBufferHeight;
  // D3D9 keeps rendering (to a hidden window) so the shader API is unaffected;
  // the game window belongs to the Remix swapchain.
  if (!g_hiddenWindow) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = hiddenProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"shaderapi_proxy_hidden";
    RegisterClassW(&wc);
    g_hiddenWindow = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, pp->BackBufferWidth,
                                     pp->BackBufferHeight, nullptr, nullptr, wc.hInstance, nullptr);
  }
  pp->hDeviceWindow = g_hiddenWindow;
  pp->Windowed = TRUE;
  pp->FullScreen_RefreshRateInHz = 0;
}

void onDeviceCreated(IDirect3DDevice9 *) {
  // The engine has installed its own crash handler by now.
  diag::reinstall();
  if (!enabled() || g_ready) {
    diag::stage("Direct3D device created (OpenRemix %s)", g_ready ? "already running" : "disabled");
    return;
  }
  diag::stage("Direct3D device created");
  g_ready = startRemix();
  diag::stage("OpenRemix: start-up %s", g_ready ? "complete" : "FAILED: the game goes on without it");
}

// Census of the draws that depend on what the proxy does not reproduce: a
// stencil test or write, a render-target texture as input, or a render-target
// texture as output. One row per distinct state, for the statistics file.
struct CensusRow {
  uint32_t draws = 0, primitives = 0;
};
std::map<std::string, CensusRow> g_census; // under g_transientSampleLock

bool drawIsPerspective() {
  if (!g_ready || !g_shaderApi)
    return true;
  float projection[16];
  getMatrix(kMatrixProjection, projection);
  return std::fabs(std::fabs(projection[11]) - 1.0f) < 1e-3f && std::fabs(projection[15]) < 1e-3f;
}

void census(const Draw &d) {
  char key[320];
  if (!d.stencil && !d.texture0Target) {
    // Plain draws into a render-target texture: per target only.
    std::snprintf(key, sizeof(key), "into RT %p %ux%u fmt %u: unmasked draws", d.target, d.targetWidth, d.targetHeight,
                  d.targetFormat);
  } else {
    float projection[16];
    getMatrix(kMatrixProjection, projection);
    const bool perspective = std::fabs(std::fabs(projection[11]) - 1.0f) < 1e-3f && std::fabs(projection[15]) < 1e-3f;
    char target[64] = "back buffer";
    if (d.target)
      std::snprintf(target, sizeof(target), "RT %p %ux%u fmt %u", d.target, d.targetWidth, d.targetHeight,
                    d.targetFormat);
    else if (!d.mainTarget)
      std::snprintf(target, sizeof(target), "other surface");
    char stencil[112] = "stencil off";
    if (d.stencil)
      std::snprintf(stencil, sizeof(stencil), "stencil func %u ref %u mask %X/%X pass %u fail %u zfail %u", d.stencilFunc,
                    d.stencilRef, d.stencilMask & 0xff, d.stencilWriteMask & 0xff, d.stencilPass, d.stencilFail,
                    d.stencilZFail);
    std::snprintf(key, sizeof(key), "%s | %s P5 %.3f | tex0 %s%p | %s | cw %X z %d func %u write %d | blend %d %u/%u atest %d",
                  target, perspective ? "3D" : "2D", projection[5], d.texture0Target ? "RT " : "", d.texture0, stencil,
                  d.colorWrite, int(d.zEnable), d.zFunc, int(d.zWrite), int(d.blend), d.srcBlend, d.dstBlend,
                  int(d.alphaTest));
  }
  std::lock_guard<std::mutex> lock(g_transientSampleLock);
  if (g_census.size() >= 128 && !g_census.count(key))
    return;
  auto &row = g_census[key];
  ++row.draws;
  row.primitives += d.primCount;
}

// Decals (see onDraw): what one paints, and what marked it.
enum DecalKind : uint8_t { kDecalNone, kDecalAlpha, kDecalMod2x, kDecalMultiply, kDecalOpaque };
enum : uint32_t { kDecalByFlag = 1, kDecalByShader = 2, kDecalByBias = 4, kDecalByCategory = 8 };
struct DecalSeen {
  uint32_t draws = 0, how = 0;
  uint8_t kind = 0;
  std::string shader;
};
std::map<std::string, DecalSeen> g_decalsSeen; // by material, this interval; under g_transientSampleLock

// What to multiply the summed face normals (e1 x e2) of a draw by to point
// out of its visible side, for lifting a decal that has no vertex normals.
// D3D culls the triangles that run counter-clockwise on screen (D3DCULL_CCW,
// Source's default) and Source's world space is right-handed, so e1 x e2 of a
// visible triangle points away from the viewer: into the wall (as in the BSP
// file, whose faces wind clockwise seen from their front). The meshes that do
// file, whose faces wind clockwise seen from their front, and in the studio
// models' strips). The meshes that do have vertex normals are the check: if
// their triangles overwhelmingly wind the other way, that is what is used.
float faceNormalSign(uint32_t cullMode) {
  const uint32_t along = g_stats.windingAlong.load(std::memory_order_relaxed),
                 against = g_stats.windingAgainst.load(std::memory_order_relaxed);
  const float ccw = along > 64 && along / 8 > against ? 1.0f : -1.0f;
  return cullMode == D3DCULL_CW ? -ccw : ccw;
}

// A draw's part in the stencil emulation (see StencilGroup), from its stencil
// state. False: the draw shows nothing (it writes nothing the emulation uses,
// or tests a value nothing marked).
bool stencilRole(const Draw &d, Instance &inst) {
  if (d.stencil) { // see StencilGroup
    const uint32_t value = d.stencilRef & d.stencilMask & 0xff, written = d.stencilRef & d.stencilWriteMask & 0xff;
    inst.depthTest = d.depthTest;
    inst.depthWrite = d.zWrite;
    if (d.stencilFunc == D3DCMP_ALWAYS && d.stencilPass == D3DSTENCILOP_REPLACE) {
      inst.stencilGroup = stencilWriter(written);
      g_stencilGroups[inst.stencilGroup].glassSources += !d.zWrite;
      // A render-target texture without contents (not read back, see
      // d3d::requestTargetContents): such a writer only marks.
      inst.stencilRole = d.texture0Target && !inst.material ? kStencilSourceOnly : kStencilSource;
      g_stencilGroups[inst.stencilGroup].ignoreZ |= !d.depthTest;
    } else if (d.stencilFunc == D3DCMP_NEVER) {
      if (d.stencilFail != D3DSTENCILOP_REPLACE)
        return false;
      inst.stencilGroup = stencilWriter(written);
      inst.stencilRole = kStencilSourceOnly;
      inst.depthTest = false; // fails before the depth test: marks through anything
    } else if (d.stencilFunc == D3DCMP_EQUAL &&
               (d.stencilPass == D3DSTENCILOP_INCR || d.stencilPass == D3DSTENCILOP_INCRSAT)) {
      const int parent = stencilReader(value);
      if (parent < 0)
        return false;
      inst.stencilGroup = stencilWriter(value + 1);
      g_stencilGroups[inst.stencilGroup].within = parent;
      inst.stencilRole = kStencilSourceOnly;
    } else if (d.stencilFunc == D3DCMP_EQUAL || d.stencilFunc == D3DCMP_LESS) {
      inst.stencilGroup = stencilReader(d.stencilFunc == D3DCMP_LESS ? value + 1 : value);
      if (inst.stencilGroup < 0)
        return false; // nothing marked: not visible
      inst.stencilRole = kStencilInside;
    }
    else if (d.stencilFunc == D3DCMP_NOTEQUAL) {
      // A scope's body around its lens opening: a scene instance as ever,
      // which the camera does not see inside the group.
      inst.stencilGroup = stencilReader(value);
      inst.stencilRole = inst.stencilGroup >= 0 ? kStencilCut : kStencilNone;
    }
  }
  return true;
}

// --- Particles ---------------------------------------------------------------
// Source's particle systems (render_animated_sprites, render_sprite_trail,
// render_rope, ... with the Spritecard shader) build their billboards in the
// vertex shader: the four vertices of a particle all carry its centre as
// POSITION; the corner (TEXCOORD3), radius, roll and yaw (TEXCOORD2), sheet
// frames (TEXCOORD0/1) and, per orientation mode, a normal come from other
// elements, the camera and size limits from constants. Ropes and trails with
// $splinetype (splinecard_vs20) carry a curve parameter as POSITION and four
// control points as texture coordinates. Taken by POSITION alone such draws
// are degenerate: nothing showed.
//
// The proxy runs the draw's own vertex shader on the CPU instead (vsprog,
// decoded at CreateVertexShader) and takes its clip-space positions back to
// world space, so the geometry is the shader's whatever its mode, and the
// colour and texture coordinates are the ones the shader hands its pixel
// shader (distance fades, $color modulation, texture transforms). That also
// serves the particles Source expands on the CPU (CParticleMgr effects,
// sprites, beams: UnlitGeneric, Sprite), whose vertex shader is a plain
// transform: their colour modulation and scrolling textures come out of it.
//
// A draw is a particle draw when
//   expanded  its vertex shader's position depends on texture coordinates or
//             colours, from dynamic buffers and not skinned: Spritecard,
//             SplineCard, and whatever else builds geometry that way;
//   shader    it is blended, from dynamic buffers, and its material's shader
//             is Spritecard, SplineCard, Sprite or ParticleSphere, or Modulate
//             with a multiplying blend, a material under the particle
//             directories and nothing that makes it a decal;
//   additive  blended with destination ONE, dynamic, an unlit material
//             (glows, beams, tracers: raster overlays before, too);
//   path      blended, dynamic, unlit, and the material lies under
//             particle/, particles/, effects/, sprites/ or trails/ (smoke and
//             dust of the old particle system; OPENREMIX_PROXY_PARTICLE_PATHS
//             adds prefixes, ';'-separated). Other unlit blended surfaces
//             (3D2D panels, world text) stay traced surfaces.
// Particles are unlit in Source (texture x vertex colour): they are
// rasterized over the traced image with their blend state and depth test
// (REMIXAPI_INSTANCE_RASTER_ONLY_EXT), additive or not. Traced, a smoke puff
// would be a stack of lit, shadowed sheets, black in a dark room, or with an
// emissive material thousands of flickering emitters. They take no part in
// the flashlight-pass, decal, emissive or identity logic of onDraw.
// OPENREMIX_PROXY_PARTICLES=0 turns all of this off (draws as before);
// OPENREMIX_PROXY_PARTICLES_TRACED=1 submits the ones that are not additive
// as traced unlit surfaces instead (emitters only with
// OPENREMIX_PROXY_EMISSIVE_LIGHTS=3).
//
// What the pixel shader does beyond texture x colour is taken from the
// material (Spritecard): $overbrightfactor scales the colour (up to 4x, a
// modulate-2x/4x stage); $addself and $addbasetexture2 blend ONE /
// INVSRCALPHA with a colour the shader multiplied by its alpha, drawn here
// as SRCALPHA / INVSRCALPHA with the $addself gain (1 + addself x overbright
// x vertex alpha) in the vertex colour (the second texture of
// $addbasetexture2 is left out). The rest is the runtime's sprite overlay
// (remixapi_InstanceInfoSpriteEXT), fed with the vertex shader's other
// outputs (remixapi_MeshInfoSpriteVerticesEXT):
//   frames     TEXCOORD1 and TEXCOORD2.x are the second sheet frame and its
//              weight: blended ($blendframes, the default), or the brighter
//              one ($maxlumframeblend1), or with $extractgreenalpha four
//              alpha-only images in the green and alpha of the two frames;
//   sequences  $dualsequence: TEXCOORD5/6 and TEXCOORD2.z are a second
//              sequence, averaged with the first, replacing its colour or
//              laid over it ($sequence_blend_mode 0/1/2), the brighter of
//              its frames with $maxlumframeblend2;
//   soft       $depthblend: alpha x saturate(max(smoothstep(0.75, 1, scene /
//              range), |scene - particle| / $depthblendscale)) on projected
//              z, range 192 units (the depth Source keeps in its frame
//              buffer's alpha): the distance and range go over in view-space
//              units;
//   modulate   the Modulate shader (blood: dst x src, or x 2 with $mod2x,
//              on frame-buffer values, the source fading to neutral with its
//              alpha), and other draws that blend that way (without the
//              fade).
// $ramptexture and $addbasetexture2's second texture are not reproduced.
enum ParticleKind : uint8_t { kNotParticle, kParticleExpanded, kParticleShader, kParticleAdditive, kParticlePath };
const char *const kParticleKindNames[] = {"-", "vertex-shader expanded", "particle shader", "additive unlit",
                                          "unlit by material path"};
enum ParticleOutcome : uint8_t {
  kParticleRasterAdditive,
  kParticleRasterBlended,
  kParticleRasterOther,
  kParticleTraced,
  kParticleFaded,
  kParticleStencil,
  kParticleFailed,
  kParticleOutcomes
};
const char *const kParticleOutcomeNames[] = {"raster additive", "raster blended", "raster other blend", "traced",
                                             "faded out",       "stencil only",   "not understood"};

struct ParticleRow {
  std::string shader, declaration, material;
  uint64_t program = 0;
  uint8_t kind = 0;
  uint32_t draws = 0, vertices = 0, triangles = 0, premultiplied = 0, overbright = 0, secondFrames = 0;
  // Sprite overlay features (draws): soft depth (of them: scale confirmed by
  // the pixel shader's constants), frame blend, brighter frame, green/alpha,
  // second sequence, modulate.
  uint32_t soft = 0, softConfirmed = 0, frameBlend = 0, frameMax = 0, greenAlpha = 0, sequence = 0, modulate = 0;
  uint32_t outcomes[kParticleOutcomes] = {};
  const char *failure = "";
  bool dumped = false;
};
std::unordered_map<uint64_t, ParticleRow> g_particleRows; // per interval; under g_transientSampleLock
std::unordered_set<uint64_t> g_particlesDumped;           // under g_transientSampleLock
struct ParticleLeft {
  std::string shader;
  uint32_t draws = 0, srcBlend = 0, dstBlend = 0;
};
// Blended draws from dynamic buffers that are not particles by the rules
// above, by material; per interval, under g_transientSampleLock.
std::map<std::string, ParticleLeft> g_particlesLeft;
std::atomic<uint32_t> g_usParticles{0};

bool nameStarts(const char *name, const char *prefix) { return _strnicmp(name, prefix, std::strlen(prefix)) == 0; }

bool particlesEnabled() {
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_PARTICLES", v, sizeof(v)) && v[0] == '0');
  }();
  return enabled;
}

// The material lies in one of the particle directories.
bool particleMaterialPath(const char *material) {
  static const std::vector<std::string> prefixes = [] {
    std::vector<std::string> list = {"particle/", "particles/", "effects/", "sprites/", "trails/"};
    char extra[1024] = {};
    if (GetEnvironmentVariableA("OPENREMIX_PROXY_PARTICLE_PATHS", extra, sizeof(extra)))
      for (char *token = std::strtok(extra, ";"); token; token = std::strtok(nullptr, ";")) {
        std::string prefix = token;
        for (char &c : prefix)
          c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
        if (!prefix.empty())
          list.push_back(prefix);
      }
    return list;
  }();
  for (const auto &prefix : prefixes)
    if (std::strncmp(material, prefix.c_str(), prefix.size()) == 0)
      return true;
  return false;
}

bool particleShader(const char *shader) {
  return nameStarts(shader, "spritecard") || nameStarts(shader, "splinecard") || nameStarts(shader, "sprite") ||
         nameStarts(shader, "particlesphere");
}

// `multiplies`: blended dst x src (x 2) and not a decal by any sign.
ParticleKind particleKind(const Draw &d, const engine_materials::Bound &bound, bool skinned, bool dynamic,
                          bool multiplies) {
  if (!particlesEnabled() || skinned)
    return kNotParticle;
  if (!dynamic) // particles are rebuilt every frame
    return kNotParticle;
  if (d.vertexProgram && vsprog::info(*d.vertexProgram).expands)
    return kParticleExpanded;
  if (!d.blend)
    return kNotParticle;
  if (bound.known && particleShader(bound.shader))
    return kParticleShader;
  if (bound.known && multiplies && nameStarts(bound.shader, "modulate") &&
      particleMaterialPath(engine_materials::currentName()))
    return kParticleShader;
  if (engine_materials::current().mode != engine_materials::Shading::Unlit)
    return kNotParticle;
  if (d.dstBlend == D3DBLEND_ONE)
    return kParticleAdditive;
  return particleMaterialPath(engine_materials::currentName()) ? kParticlePath : kNotParticle;
}

// "POSITION0 float3 +0, COLOR0 d3dcolor +12, ...".
std::string describeDeclaration(const D3DVERTEXELEMENT9 *elements) {
  static const char *const usages[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL",  "PSIZE",
                                       "TEXCOORD", "TANGENT",     "BINORMAL",     "TESSFACTOR", "POSITIONT",
                                       "COLOR",    "FOG",         "DEPTH",        "SAMPLE"};
  static const char *const types[] = {"float1",  "float2",  "float3",   "float4",   "d3dcolor", "ubyte4",
                                      "short2",  "short4",  "ubyte4n",  "short2n",  "short4n",  "ushort2n",
                                      "ushort4n", "udec3",  "dec3n",    "float16_2", "float16_4"};
  std::string out;
  for (const D3DVERTEXELEMENT9 *e = elements; e->Stream != 0xff; ++e) {
    char item[64];
    std::snprintf(item, sizeof(item), "%s%s%u %s s%u+%u", out.empty() ? "" : ", ", e->Usage < 14 ? usages[e->Usage] : "?",
                  e->UsageIndex, e->Type < 17 ? types[e->Type] : "?", e->Stream, e->Offset);
    out += item;
  }
  return out;
}

// The draw's triangles as a list of vertex indices relative to its lowest
// (`low`, itself relative to minIndex), `count` vertices in all.
bool particleTriangles(const Draw &d, std::vector<uint32_t> &indices, uint32_t &low, uint32_t &count) {
  indices.clear();
  const uint32_t width = d.indices.is32 ? 4 : 2;
  auto index = [&](uint32_t i, uint32_t &out) {
    const uint64_t at = uint64_t(d.startIndex + i) * width;
    if (at + width > d.indices.size)
      return false;
    uint32_t value = 0;
    std::memcpy(&value, d.indices.data + at, width);
    if (value < d.minIndex || value - d.minIndex >= d.numVertices)
      return false;
    out = value - d.minIndex;
    return true;
  };
  if (d.type == D3DPT_TRIANGLELIST) {
    indices.resize(size_t(d.primCount) * 3);
    for (size_t i = 0; i < indices.size(); ++i)
      if (!index(uint32_t(i), indices[i]))
        return false;
  } else if (d.type == D3DPT_TRIANGLESTRIP) {
    for (uint32_t i = 0; i < d.primCount; ++i) {
      uint32_t a, b, c;
      if (!index(i, a) || !index(i + 1, b) || !index(i + 2, c))
        return false;
      if (a == b || b == c || a == c)
        continue;
      if (i & 1)
        std::swap(a, b);
      indices.insert(indices.end(), {a, b, c});
    }
  } else {
    return false;
  }
  if (indices.empty())
    return false;
  const auto [lo, hi] = std::minmax_element(indices.begin(), indices.end());
  low = *lo;
  count = *hi - *lo + 1;
  for (auto &i : indices)
    i -= low;
  return true;
}

// Dev aid (OPENREMIX_PROXY_DUMP_PARTICLES=1): everything about a particle
// draw, once per (declaration, vertex shader, material shader, kind), to
// bin/particles.txt.
void dumpParticleDraw(const Draw &d, ParticleKind kind, const char *outcome, const char *failure, bool cameraSpace,
                      uint32_t spriteFlags, const float *spriteSoft, bool softConfirmed,
                      const particles::SpriteVertex *sprites, const vsprog::Stream *streams, uint32_t first, uint32_t count, const particles::Vertex *expanded,
                      const particles::Result *result) {
  static bool truncate = true;
  FILE *f = _wfopen((g_moduleDir + L"\\particles.txt").c_str(), truncate ? L"w" : L"a");
  truncate = false;
  if (!f)
    return;
  const engine_materials::Bound &bound = engine_materials::bound();
  fprintf(f, "== %s | material %s shader %s flags %08X%s\n", kParticleKindNames[kind], engine_materials::currentName(),
          bound.shader, unsigned(bound.flags), bound.known ? "" : " (not read)");
  fprintf(f, "   outcome: %s%s%s | positions in %s\n", outcome, failure && *failure ? ": " : "", failure ? failure : "",
          cameraSpace ? "camera space (identity view matrix: placed by the view drawn before)" : "world space");
  fprintf(f, "   sprite overlay:%s%s%s%s%s%s%s%s%s", spriteFlags ? "" : " none",
          spriteFlags & REMIXAPI_SPRITE_FRAME_BLEND_EXT ? " frames blended" : "",
          spriteFlags & REMIXAPI_SPRITE_FRAME_MAX_LUMINANCE_EXT ? " brighter frame" : "",
          spriteFlags & REMIXAPI_SPRITE_GREEN_ALPHA_EXT ? " green/alpha" : "",
          (spriteFlags & REMIXAPI_SPRITE_SEQUENCE_OVER_EXT) == REMIXAPI_SPRITE_SEQUENCE_ADD_EXT ? " second sequence added" : "",
          (spriteFlags & REMIXAPI_SPRITE_SEQUENCE_OVER_EXT) == REMIXAPI_SPRITE_SEQUENCE_REPLACE_EXT ? " second sequence replaces colour" : "",
          (spriteFlags & REMIXAPI_SPRITE_SEQUENCE_OVER_EXT) == REMIXAPI_SPRITE_SEQUENCE_OVER_EXT ? " second sequence over" : "",
          spriteFlags & REMIXAPI_SPRITE_SEQUENCE_MAX_LUMINANCE_EXT ? " (its brighter frame)" : "",
          spriteFlags & REMIXAPI_SPRITE_MODULATE_EXT
              ? (spriteFlags & REMIXAPI_SPRITE_MODULATE_FADE_EXT ? " modulate, fading to neutral" : " modulate")
              : "");
  if (spriteFlags & REMIXAPI_SPRITE_SOFT_DEPTH_EXT)
    fprintf(f, " soft depth over %.3f units of view depth, removed between depths %.2f and %.2f (%s)", spriteSoft[0],
            spriteSoft[1], spriteSoft[2],
            softConfirmed ? "from the pixel shader's c2.x and c29.w" : "from $depthblendscale: the registers disagree");
  fprintf(f, "\n");
  if (d.psConstants) {
    fprintf(f, "   pixel shader constants:");
    for (int reg : {0, 1, 2, 3, 29, 30}) {
      const float *c = d.psConstants + reg * 4;
      fprintf(f, " c%d = %.6g %.6g %.6g %.6g;", reg, c[0], c[1], c[2], c[3]);
    }
    fprintf(f, "\n");
  }
  fprintf(f, "   primitive type %u, %u primitives, base vertex %d, min index %u, %u vertices, start index %u; vertices used "
             "%u..%u\n",
          unsigned(d.type), d.primCount, d.baseVertex, d.minIndex, d.numVertices, d.startIndex, first,
          count ? first + count - 1 : first);
  fprintf(f, "   blend %d src %u dst %u op %u sepA %d srcA %u dstA %u | alphaTest %d func %u ref %u | z enable %d func %u "
             "write %d bias %d | cull %u | stencil %d func %u | colour write %X | texture0 %016llX %s%s\n",
          int(d.blend), d.srcBlend, d.dstBlend, d.blendOp, int(d.separateAlpha), d.srcBlendAlpha, d.dstBlendAlpha,
          int(d.alphaTest), d.alphaFunc, d.alphaRef, int(d.zEnable), d.zFunc, int(d.zWrite), int(d.depthBias), d.cullMode,
          int(d.stencil), d.stencilFunc, d.colorWrite, static_cast<unsigned long long>(textureHash(d.texture0)),
          bsp::materialName(textureHash(d.texture0)).c_str(), d.texture0 ? "" : "(none)");
  fprintf(f, "   declaration: %s\n", describeDeclaration(d.elements).c_str());
  for (int i = 0; i < 4; ++i)
    if (d.streams[i].data)
      fprintf(f, "   stream %d: stride %u offset %u size %u dynamic %d\n", i, d.streams[i].stride, d.streams[i].offset,
              d.streams[i].size, int(d.streams[i].dynamic));
  fprintf(f, "   material parameters:\n");
  engine_materials::describeCurrent(f, "     ");
  if (!d.vertexProgram) {
    fprintf(f, "   vertex shader %p: not seen at creation\n", d.vertexShader);
  } else {
    const vsprog::Info &info = vsprog::info(*d.vertexProgram);
    fprintf(f, "   vertex shader %p: vs_%u_%u hash %016llX, %u tokens, %u instructions; runnable %d%s%s; builds geometry "
               "%d; outputs written %04X; colour output %d%s\n",
            d.vertexShader, info.version >> 8, info.version & 0xff, static_cast<unsigned long long>(info.hash),
            info.tokens, info.instructions, int(info.supported), *info.reason ? ": " : "", info.reason,
            int(info.expands), info.written, info.colorSlot,
            info.colorSlot < 0 ? " (none: white)" : info.colorLinear ? " (converted to linear by the shader)" : "");
    fprintf(f, "   alpha output %d%s\n", info.alphaSlot, info.alphaSlot < 0 ? " (none: opaque)" : "");
    fprintf(f, "   inputs:");
    for (const auto &input : info.inputs)
      fprintf(f, " v%u=usage %u index %u", input.reg, input.usage, input.usageIndex);
    fprintf(f, "\n   constants read:\n");
    for (uint16_t reg : info.constants) {
      const float *c = d.vsConstants + size_t(reg) * 4;
      fprintf(f, "     c%u = %.9g %.9g %.9g %.9g\n", reg, c[0], c[1], c[2], c[3]);
    }
    fprintf(f, "   tokens:");
    const auto &tokens = vsprog::tokens(*d.vertexProgram);
    for (size_t i = 0; i < tokens.size(); ++i)
      fprintf(f, "%s%08X", i % 12 ? " " : "\n     ", tokens[i]);
    fprintf(f, "\n");
  }
  // The first vertices: raw elements, the shader's outputs, the result.
  const uint32_t shown = std::min(count, 8u);
  std::vector<float> outputs(size_t(vsprog::kSlots) * shown * 4);
  const bool ran = shown && d.vertexProgram && streams &&
                   vsprog::run(*d.vertexProgram, d.vsConstants, reinterpret_cast<const vsprog::Element *>(d.elements),
                               streams, first, shown, (1u << vsprog::kSlots) - 1, outputs.data());
  for (uint32_t v = 0; v < shown; ++v) {
    fprintf(f, "   vertex %u:\n", first + v);
    for (const D3DVERTEXELEMENT9 *e = d.elements; e->Stream != 0xff; ++e) {
      if (e->Stream >= 4 || !streams || !streams[e->Stream].data)
        continue;
      const vsprog::Stream &stream = streams[e->Stream];
      const uint64_t at = uint64_t(first + v) * stream.stride + e->Offset;
      if (at + 16 > stream.size)
        continue;
      const uint8_t *raw = stream.data + at;
      fprintf(f, "     usage %u index %u:", e->Usage, e->UsageIndex);
      if (e->Type <= D3DDECLTYPE_FLOAT4) {
        float values[4];
        std::memcpy(values, raw, 16);
        for (unsigned k = 0; k <= e->Type; ++k)
          fprintf(f, " %.9g", values[k]);
      } else {
        const unsigned bytes = e->Type == D3DDECLTYPE_SHORT4 || e->Type == D3DDECLTYPE_SHORT4N ||
                                       e->Type == D3DDECLTYPE_USHORT4N || e->Type == D3DDECLTYPE_FLOAT16_4
                                   ? 8
                                   : 4;
        for (unsigned k = 0; k < bytes; ++k)
          fprintf(f, " %02X", raw[k]);
      }
      fprintf(f, "\n");
    }
    if (ran) {
      const uint32_t written = vsprog::info(*d.vertexProgram).written;
      static const char *const names[vsprog::kSlots] = {"oPos", "oFog", "oPts", "oD0", "oD1", "oT0", "oT1",
                                                        "oT2",  "oT3",  "oT4",  "oT5", "oT6", "oT7"};
      for (unsigned slot = 0; slot < vsprog::kSlots; ++slot)
        if (written >> slot & 1) {
          const float *o = &outputs[(size_t(slot) * shown + v) * 4];
          fprintf(f, "     %s = %.9g %.9g %.9g %.9g\n", names[slot], o[0], o[1], o[2], o[3]);
        }
    }
    if (expanded)
      fprintf(f, "     -> %s %.4f %.4f %.4f uv %.5f %.5f colour (A R G B, sRGB bytes%s) %08X\n",
              cameraSpace ? "camera" : "world", expanded[v].position[0], expanded[v].position[1], expanded[v].position[2], expanded[v].uv[0], expanded[v].uv[1],
              result && result->gain > 1 ? (result->gain == 4 ? ", / 4" : ", / 2") : "", expanded[v].color);
    if (expanded && sprites)
      fprintf(f, "     -> second frame uv %.5f %.5f weight %.4f; second sequence uv %.5f %.5f / %.5f %.5f weight %.4f\n",
              sprites[v].frame[0], sprites[v].frame[1], sprites[v].frame[2], sprites[v].sequence[0],
              sprites[v].sequence[1], sprites[v].sequence[2], sprites[v].sequence[3], sprites[v].weight2);
  }
  fprintf(f, "\n");
  fclose(f);
}

// Builds and queues a particle draw (see above). False: the draw is not
// handled here and goes on through onDraw as before.
// `projection`: the draw's (left-handed form: view depth d projects to z =
// d projection[10] + projection[14]).
bool submitParticles(const Draw &d, ParticleKind kind, uint64_t viewKey, const float (*cameraToWorld)[4],
                     uint32_t categories, const float eye[3], const float *projection) {
  Timer timer(g_usParticles);
  using engine_materials::Shading;
  const engine_materials::Bound &bound = engine_materials::bound();
  static const bool dumpAll = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_DUMP_PARTICLES", v, sizeof(v)) && v[0] == '1';
  }();
  static const bool traceBlended = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_PARTICLES_TRACED", v, sizeof(v)) && v[0] == '1';
  }();

  // Statistics row (and dump key): declaration, vertex shader, material shader, kind.
  size_t elementCount = 1;
  while (d.elements[elementCount - 1].Stream != 0xff)
    ++elementCount;
  const uint64_t programHash = d.vertexProgram ? vsprog::info(*d.vertexProgram).hash : 0;
  uint64_t rowKey = fnv(d.elements, elementCount * sizeof(D3DVERTEXELEMENT9), programHash + kind);
  rowKey = fnv(bound.shader, std::strlen(bound.shader), rowKey);

  thread_local std::vector<uint32_t> indices;
  thread_local std::vector<particles::Vertex> expanded;
  thread_local std::vector<remixapi_HardcodedVertex> vertices;
  thread_local std::vector<particles::SpriteVertex> sprites;
  uint32_t spriteFlags = 0;
  float spriteSoft[3] = {};
  bool softConfirmed = false;
  vsprog::Stream streams[4];
  uint32_t low = 0, count = 0, first = 0;
  particles::Result result;
  const char *failure = "";
  ParticleOutcome outcome = kParticleFailed;
  bool premultiplied = false, built = false;
  remixapi_MeshHandle mesh = nullptr;
  const bool additive = d.blend && d.dstBlend == D3DBLEND_ONE;
  const bool traced = traceBlended && d.blend && !additive;
  const bool spritecard = nameStarts(bound.shader, "spritecard");
  // dst x src (x 2): the frame buffer multiplied.
  const bool multiplies = d.blend && d.blendOp == D3DBLENDOP_ADD &&
                          ((d.srcBlend == D3DBLEND_DESTCOLOR && (d.dstBlend == D3DBLEND_SRCCOLOR || d.dstBlend == D3DBLEND_ZERO)) ||
                           (d.srcBlend == D3DBLEND_ZERO && d.dstBlend == D3DBLEND_SRCCOLOR));
  // The runtime's sprite overlay does the per-pixel part; the traced variant
  // has none (it shows the leading frame).
  static const bool spriteOverlays = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_PARTICLE_SPRITES", v, sizeof(v)) && v[0] == '0');
  }();
  const bool overlay = spriteOverlays && !traced && d.blend;

  do {
    if (!d.vertexProgram) {
      failure = "vertex shader not seen at creation";
      break;
    }
    if (!vsprog::info(*d.vertexProgram).supported) {
      failure = vsprog::info(*d.vertexProgram).reason;
      break;
    }
    if (!particleTriangles(d, indices, low, count)) {
      failure = "primitive type or index range";
      break;
    }
    const int64_t firstVertex = int64_t(d.baseVertex) + d.minIndex + low;
    if (firstVertex < 0) {
      failure = "vertex range";
      break;
    }
    first = uint32_t(firstVertex);
    for (int i = 0; i < 4; ++i) {
      const VertexStream &s = d.streams[i];
      if (s.data && s.offset <= s.size)
        streams[i] = {s.data + s.offset, s.size - s.offset, s.stride};
    }
    particles::Request request;
    request.program = d.vertexProgram;
    request.constants = d.vsConstants;
    static_assert(sizeof(vsprog::Element) == sizeof(D3DVERTEXELEMENT9));
    request.elements = reinterpret_cast<const vsprog::Element *>(d.elements);
    request.streams = streams;
    request.first = first;
    request.count = count;
    auto parameter = [](const char *name, float fallback) {
      float value = fallback;
      return engine_materials::currentFloat(name, value) ? value : fallback;
    };
    if (multiplies && overlay) {
      // Modulate's pixel shader: lerp(neutral, texture x colour, alpha).
      spriteFlags |= REMIXAPI_SPRITE_MODULATE_EXT;
      if (nameStarts(bound.shader, "modulate"))
        spriteFlags |= REMIXAPI_SPRITE_MODULATE_FADE_EXT;
    }
    if (spritecard && overlay) {
      const bool green = parameter("$extractgreenalpha", 0) != 0, brighter = parameter("$maxlumframeblend1", 0) != 0;
      spriteFlags |= green      ? REMIXAPI_SPRITE_GREEN_ALPHA_EXT
                     : brighter ? REMIXAPI_SPRITE_FRAME_MAX_LUMINANCE_EXT
                     : parameter("$blendframes", 1) != 0 ? REMIXAPI_SPRITE_FRAME_BLEND_EXT : 0u;
      if (parameter("$dualsequence", 0) != 0) {
        const int mode = int(parameter("$sequence_blend_mode", 0));
        spriteFlags |= mode == 1 ? REMIXAPI_SPRITE_SEQUENCE_REPLACE_EXT
                       : mode == 2 ? REMIXAPI_SPRITE_SEQUENCE_OVER_EXT : REMIXAPI_SPRITE_SEQUENCE_ADD_EXT;
        if (parameter("$maxlumframeblend2", 0) != 0)
          spriteFlags |= REMIXAPI_SPRITE_SEQUENCE_MAX_LUMINANCE_EXT;
      }
      // Soft particles. The pixel shader computes, on projected z over the
      // range it keeps in the frame buffer's alpha (c29.w = 1 / 192),
      //   saturate(max(smoothstep(0.75, 1, scene c29.w), |scene - z| c29.w c2.x))
      // with c2.x = 192 / $depthblendscale. The registers are taken when they
      // agree with the material (they may be another shader's), else the
      // material's scale and the 192 units.
      if (parameter("$depthblend", 0) != 0 && projection && projection[10] > 0) {
        const float scale = parameter("$depthblendscale", 50);
        float distance = scale > 0 ? scale : 50, range = 192;
        if (d.psConstants) {
          const float perUnit = d.psConstants[29 * 4 + 3], feather = d.psConstants[2 * 4];
          if (std::isfinite(perUnit) && std::isfinite(feather) && perUnit > 1e-5f && perUnit < 1 && feather > 0) {
            const float fromShader = 1 / (perUnit * feather);
            if (fromShader > distance * 0.5f && fromShader < distance * 2) {
              distance = fromShader;
              range = 1 / perUnit;
              softConfirmed = true;
            }
          }
        }
        // Projected z to view depth: z = depth a + b.
        const float a = projection[10], b = projection[14];
        spriteFlags |= REMIXAPI_SPRITE_SOFT_DEPTH_EXT;
        spriteSoft[0] = distance / a;
        spriteSoft[1] = (0.75f * range - b) / a;
        spriteSoft[2] = (range - b) / a;
      }
    }
    constexpr uint32_t kSpriteVertexFlags =
        REMIXAPI_SPRITE_FRAME_BLEND_EXT | REMIXAPI_SPRITE_FRAME_MAX_LUMINANCE_EXT | REMIXAPI_SPRITE_GREEN_ALPHA_EXT |
        REMIXAPI_SPRITE_SEQUENCE_OVER_EXT | REMIXAPI_SPRITE_SEQUENCE_MAX_LUMINANCE_EXT;
    if (spriteFlags & kSpriteVertexFlags) {
      if (sprites.size() < count)
        sprites.resize(count);
      request.sprites = sprites.data();
    }
    if (spritecard) {
      float value = 1;
      // $blendframes (default on): the pixel shader blends two sheet frames
      // (the leading one shows where the sprite overlay does not blend them).
      request.frameBlend = !request.sprites && (!engine_materials::currentFloat("$blendframes", value) || value != 0);
      float overbright = 1, addSelf = 0, second = 0;
      if (engine_materials::currentFloat("$overbrightfactor", overbright) && overbright >= 0)
        for (float &c : request.colorScale)
          c = overbright;
      else
        overbright = 1;
      engine_materials::currentFloat("$addself", addSelf);
      engine_materials::currentFloat("$addbasetexture2", second);
      // Both shader variants multiply the colour by its alpha and blend
      // ONE / INVSRCALPHA.
      if (d.blend && d.srcBlend == D3DBLEND_ONE && d.dstBlend == D3DBLEND_INVSRCALPHA && (addSelf > 0 || second > 0)) {
        premultiplied = true;
        if (addSelf > 0)
          request.addSelf = addSelf * overbright;
      }
    } else if (const Shading &shading = engine_materials::current(); shading.mode == Shading::Unlit) {
      // UnlitGeneric, Sprite: the material's modulation ($color x $color2,
      // $alpha: a sprite's render colour, set by the engine per draw) reaches
      // the pixel shader as a constant, unless the vertex shader took it
      // (c47, the modulation register: it is in the colour output then).
      // UnlitGeneric's pixel shader takes the vertex colour's rgb only with
      // $vertexcolor and its alpha only with $vertexalpha (its vertex shader
      // passes the whole colour on, in a texture coordinate, with either).
      if (bound.known && !particleShader(bound.shader) &&
          vsprog::info(*d.vertexProgram).colorSlot >= int(vsprog::kTex0)) {
        request.useColor = bound.vertexColor;
        request.useAlpha = bound.vertexAlpha;
      }
      const auto &constants = vsprog::info(*d.vertexProgram).constants;
      if (!std::binary_search(constants.begin(), constants.end(), uint16_t(47))) {
        for (int c = 0; c < 3; ++c)
          request.colorScale[c] = std::max(shading.tint[c], 0.0f); // linear, as the unlit surfaces take it
        float alpha = 1;
        if (engine_materials::currentFloat("$alpha", alpha))
          request.alphaScale = std::clamp(alpha, 0.0f, 1.0f);
      }
    }
    if (expanded.size() < count)
      expanded.resize(count);
    if (!particles::expand(request, expanded.data(), result, &failure))
      break;
    built = true;
    // What the vertex shader does not write is not there to combine.
    if (!result.frames)
      spriteFlags &= ~(REMIXAPI_SPRITE_FRAME_BLEND_EXT | REMIXAPI_SPRITE_FRAME_MAX_LUMINANCE_EXT |
                       REMIXAPI_SPRITE_GREEN_ALPHA_EXT);
    if (!result.sequences)
      spriteFlags &= ~(REMIXAPI_SPRITE_SEQUENCE_OVER_EXT | REMIXAPI_SPRITE_SEQUENCE_MAX_LUMINANCE_EXT);
    const bool spriteVertices = (spriteFlags & kSpriteVertexFlags) != 0;

    // What the blend state makes of a black or transparent draw: Source
    // fades particles out through their colour and keeps drawing them.
    const bool usesAlpha = d.blend && (d.srcBlend == D3DBLEND_SRCALPHA || premultiplied);
    if (d.blend && ((additive && (!result.anyColor || (usesAlpha && !result.anyAlpha))) ||
                    (!additive && usesAlpha && d.dstBlend == D3DBLEND_INVSRCALPHA && !result.anyAlpha) ||
                    (!additive && !usesAlpha && d.srcBlend == D3DBLEND_ONE && d.dstBlend == D3DBLEND_INVSRCALPHA &&
                     !result.anyColor && !result.anyAlpha) ||
                    ((spriteFlags & REMIXAPI_SPRITE_MODULATE_FADE_EXT) && !result.anyAlpha))) {
      outcome = kParticleFaded;
      break;
    }

    // Facing the eye, for the traced variant (the raster overlays have no lighting).
    float normal[3] = {eye[0] - (result.low[0] + result.high[0]) * 0.5f, eye[1] - (result.low[1] + result.high[1]) * 0.5f,
                       eye[2] - (result.low[2] + result.high[2]) * 0.5f};
    const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    for (float &n : normal)
      n = length > 1e-6f ? n / length : 0.0f;
    if (!(length > 1e-6f))
      normal[2] = 1;
    if (vertices.size() < count)
      vertices.resize(count);
    for (uint32_t v = 0; v < count; ++v) {
      remixapi_HardcodedVertex &out = vertices[v];
      std::memcpy(out.position, expanded[v].position, sizeof(out.position));
      std::memcpy(out.normal, normal, sizeof(out.normal));
      std::memcpy(out.texcoord, expanded[v].uv, sizeof(out.texcoord));
      out.color = expanded[v].color;
      if (spriteVertices) { // remixapi_MeshInfoSpriteVerticesEXT: instead of a normal, and in the padding
        const particles::SpriteVertex &sprite = sprites[v];
        std::memcpy(out.normal, sprite.frame, sizeof(out.normal));
        std::memcpy(&out._pad0, &sprite.sequence[0], 4);
        std::memcpy(&out._pad1, &sprite.sequence[1], 4);
        std::memcpy(&out._pad2, &sprite.sequence[2], 4);
        std::memcpy(&out._pad3, &sprite.sequence[3], 4);
        std::memcpy(&out._pad4, &sprite.weight2, 4);
      }
    }
    remixapi_MeshInfoSpriteVerticesEXT spriteMesh = {};
    spriteMesh.sType = REMIXAPI_STRUCT_TYPE_MESH_SPRITE_VERTICES_EXT;
    spriteMesh.enabled = 1;
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = vertices.data();
    surface.vertices_count = count;
    surface.indices_values = indices.data();
    surface.indices_count = indices.size();
    struct {
      const void *vb, *ib;
      uint32_t vbVersion, ibVersion, offset, startIndex, primCount, minIndex;
      int baseVertex;
      uint32_t sequence;
      uint64_t view;
    } key = {d.streams[0].buffer, d.indices.buffer, d.streams[0].version, d.indices.version, d.streams[0].offset,
             d.startIndex,        d.primCount,      d.minIndex,           d.baseVertex,      uint32_t(g_instances.size()),
             viewKey};
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = fnv(&key, sizeof(key), 0x9a471c1e5ull) | 1;
    if (spriteVertices)
      info.pNext = &spriteMesh;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    if (g_api.CreateMesh(&info, &mesh) != REMIXAPI_ERROR_CODE_SUCCESS) {
      ++g_stats.rejected;
      mesh = nullptr;
      failure = "mesh rejected by the runtime";
      break;
    }
    ++g_stats.created;
    g_transient.push_back(mesh);
    g_transientVerts += count;
    g_transientIndices += indices.size();

    Instance inst;
    inst.mesh = mesh;
    inst.texture0 = d.texture0;
    if (d.texture0Target && screenSlot(d.texture0) < 0)
      d3d::requestTargetContents(d.texture0);
    inst.blend = blendState(d);
    if (premultiplied) { // see above: the alpha factor moves from the shader to the blend
      inst.blend.srcColorBlendFactor = vkBlendFactor(D3DBLEND_SRCALPHA);
      inst.blend.srcAlphaBlendFactor = vkBlendFactor(D3DBLEND_SRCALPHA);
    }
    // Colour: texture x vertex colour (x 2 or 4: overbright); alpha: texture x vertex alpha.
    inst.blend.textureColorOperation = result.gain == 4 ? 5 : result.gain == 2 ? 4 : 3;
    inst.blend.textureColorArg1Source = 1;
    inst.blend.textureColorArg2Source = 2;
    inst.blend.textureAlphaOperation = 3;
    inst.blend.textureAlphaArg1Source = 1;
    inst.blend.textureAlphaArg2Source = 2;
    const bool nearest = isPointSampler(d.samplerStates, 0);
    if (traced) {
      Shading shading;
      shading.mode = Shading::Unlit;
      shading.vertexColor = shading.vertexAlpha = true;
      inst.material = d.texture0 ? materialFor(d.texture0, shading, nearest) : nullptr;
    } else {
      inst.material = d.texture0 ? materialFor(d.texture0, 0, nearest) : nullptr;
      inst.rasterOnly = true;
    }
    inst.particle = true;
    if (inst.rasterOnly) {
      inst.spriteFlags = spriteFlags;
      std::memcpy(inst.spriteSoft, spriteSoft, sizeof(spriteSoft));
    } else {
      spriteFlags = 0;
    }
    inst.categories = categories;
    inst.replacementHash = 0;
    inst.persistent = false;
    // The mesh is in the space of the draw's view-projection: the world, or
    // the camera's (CameraSpace).
    static const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    std::memcpy(inst.transform, cameraToWorld ? cameraToWorld : identity, sizeof(inst.transform));
    inst.view = viewKey;
    inst.offTarget = !d.mainTarget;
    inst.primitives = d.primCount;
    inst.firstIndex = d.startIndex;
    const bool marks = stencilRole(d, inst); // sets the depth state of stencil draws
    inst.depthTest = d.depthTest && inst.depthTest;
    inst.depthWrite = d.zWrite && !d.blend;
    if (!marks) {
      outcome = kParticleStencil;
      break;
    }
    g_instances.push_back(inst);
    ++g_stats.accepted;
    outcome = traced ? kParticleTraced
              : additive ? kParticleRasterAdditive
              : !d.blend || (d.dstBlend == D3DBLEND_INVSRCALPHA && (d.srcBlend == D3DBLEND_SRCALPHA || d.srcBlend == D3DBLEND_ONE))
                  ? kParticleRasterBlended
                  : kParticleRasterOther;
  } while (false);

  bool dump = false;
  {
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    ++g_transientDecls[d.declaration];
    if (g_particleRows.size() < 64 || g_particleRows.count(rowKey)) {
      ParticleRow &row = g_particleRows[rowKey];
      if (!row.draws) {
        row.shader = bound.known ? bound.shader : "(material not read)";
        row.declaration = describeDeclaration(d.elements);
        row.program = programHash;
        row.kind = kind;
      }
      ++row.draws;
      ++row.outcomes[outcome];
      if (outcome == kParticleFailed)
        row.failure = failure;
      else if (outcome != kParticleFaded && outcome != kParticleStencil) {
        row.vertices += count;
        row.triangles += uint32_t(indices.size() / 3);
        row.premultiplied += premultiplied;
        row.overbright += result.gain > 1;
        row.secondFrames += result.secondFrames;
        row.soft += (spriteFlags & REMIXAPI_SPRITE_SOFT_DEPTH_EXT) != 0;
        row.softConfirmed += (spriteFlags & REMIXAPI_SPRITE_SOFT_DEPTH_EXT) && softConfirmed;
        row.frameBlend += (spriteFlags & REMIXAPI_SPRITE_FRAME_BLEND_EXT) != 0;
        row.frameMax += (spriteFlags & REMIXAPI_SPRITE_FRAME_MAX_LUMINANCE_EXT) != 0;
        row.greenAlpha += (spriteFlags & REMIXAPI_SPRITE_GREEN_ALPHA_EXT) != 0;
        row.sequence += (spriteFlags & REMIXAPI_SPRITE_SEQUENCE_OVER_EXT) != 0;
        row.modulate += (spriteFlags & REMIXAPI_SPRITE_MODULATE_EXT) != 0;
      }
      if (row.material.empty() || outcome == kParticleFailed)
        row.material = engine_materials::currentName();
    }
    if (dumpAll && g_particlesDumped.size() < 512)
      dump = g_particlesDumped.insert(rowKey ^ (outcome == kParticleFailed ? 0x5a5a5a5a5a5a5a5aull : 0)).second;
  }
  if (dump)
    dumpParticleDraw(d, kind, kParticleOutcomeNames[outcome], failure, cameraToWorld != nullptr, spriteFlags, spriteSoft,
                     softConfirmed, built && (spriteFlags & 0x3f) ? sprites.data() : nullptr, streams, first, count,
                     built ? expanded.data() : nullptr, built ? &result : nullptr);
  // A draw whose shader builds its geometry has nothing to offer the general
  // path (degenerate quads); the others fall back to it.
  return outcome != kParticleFailed || kind == kParticleExpanded;
}

// ShatteredGlass edge pieces (onDraw): Source's shader multiplies the glass
// ($detail, on the window's coordinates) by the piece's shape mask (on its
// own). A runtime material has one set of coordinates, so each piece quad
// gets a texture of the two composed on the mask's coordinates. Render thread.
struct ShardSource {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
};
const ShardSource *shardSource(const std::string &name) {
  static std::unordered_map<std::string, std::unique_ptr<ShardSource>> images;
  auto [it, added] = images.try_emplace(name);
  if (added) {
    auto image = std::make_unique<ShardSource>();
    if (bsp::textureRgba(name, image->width, image->height, image->rgba) && image->width && image->height)
      it->second = std::move(image);
  }
  return it->second.get();
}
struct ShardTexture {
  std::unique_ptr<uint8_t> identity; // the key standing for a D3D texture in g_textures
  uint64_t lastFrame = 0;
};
std::unordered_map<uint64_t, ShardTexture> g_shardTextures;
bool g_inShardDraw = false; // drawing a piece's quad with its composed texture
// `map`: the glass coordinates over the mask's, glass = (map[0], map[1]) +
// mask.x * (map[2], map[3]) + mask.y * (map[4], map[5]).
const void *shardTexture(const std::string &mask, const std::string &glass, const float map[6]) {
  float key[6];
  for (int i = 0; i < 6; ++i)
    key[i] = std::round(map[i] * 4096) / 4096;
  uint64_t hash = XXH3_64bits_withSeed(key, sizeof(key), XXH3_64bits(mask.data(), mask.size()));
  hash = XXH3_64bits_withSeed(glass.data(), glass.size(), hash) | 1;
  auto &entry = g_shardTextures[hash];
  entry.lastFrame = g_frame;
  if (entry.identity)
    return entry.identity.get();
  const ShardSource *m = shardSource(mask), *g = shardSource(glass);
  if (!m || !g) {
    g_shardTextures.erase(hash);
    return nullptr;
  }
  const uint32_t width = std::min(m->width, 128u), height = std::min(m->height, 128u);
  std::vector<uint8_t> out(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      const float u = (x + 0.5f) / width, v = (y + 0.5f) / height;
      const uint8_t *mt = &m->rgba[(size_t(std::min(uint32_t(v * m->height), m->height - 1)) * m->width +
                                    std::min(uint32_t(u * m->width), m->width - 1)) * 4];
      float gu = map[0] + u * map[2] + v * map[4], gv = map[1] + u * map[3] + v * map[5];
      gu -= std::floor(gu);
      gv -= std::floor(gv);
      if (!std::isfinite(gu) || !std::isfinite(gv))
        gu = gv = 0;
      const uint8_t *gt = &g->rgba[(size_t(std::min(uint32_t(gv * g->height), g->height - 1)) * g->width +
                                    std::min(uint32_t(gu * g->width), g->width - 1)) * 4];
      uint8_t *o = &out[(size_t(y) * width + x) * 4];
      o[0] = gt[0];
      o[1] = gt[1];
      o[2] = gt[2];
      o[3] = uint8_t((unsigned(gt[3]) * mt[3] + 127) / 255);
    }
  auto t = std::make_shared<RemixTexture>();
  t->hash = hash;
  remixapi_TextureInfoGenerateMipsEXT mips = {};
  mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
  mips.generate = true;
  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.pNext = &mips;
  info.hash = hash;
  info.width = width;
  info.height = height;
  info.depth = 1;
  info.mipLevels = 1;
  info.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  info.data = out.data();
  info.dataSize = out.size();
  if (g_api.CreateTexture(&info, &t->texture) != REMIXAPI_ERROR_CODE_SUCCESS) {
    g_shardTextures.erase(hash);
    return nullptr;
  }
  ++g_stats.shatteredTextures;
  entry.identity = std::make_unique<uint8_t>(0);
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  g_texturesByHash[hash] = t;
  g_textures[entry.identity.get()] = std::move(t);
  // Pieces of windows long out of sight go (a few dozen per window).
  if (g_shardTextures.size() > 1024)
    for (auto it = g_shardTextures.begin(); it != g_shardTextures.end();)
      if (g_frame - it->second.lastFrame > 600) {
        g_textures.erase(it->second.identity.get());
        g_texturesByHash.erase(it->first);
        it = g_shardTextures.erase(it);
      } else
        ++it;
  return entry.identity.get();
}

// A ShatteredGlass edge piece's quads, each drawn with its composed texture
// (shardTexture). False when the draw is not quads of the two coordinate sets.
void onDraw(const Draw &d);
bool drawShardQuads(const Draw &d, const std::string &mask, const std::string &glass) {
  const auto *maskCoord = findElement(d.elements, D3DDECLUSAGE_TEXCOORD, 0);
  const auto *glassCoord = findElement(d.elements, D3DDECLUSAGE_TEXCOORD, 2);
  if (d.type != D3DPT_TRIANGLELIST || !d.indices.data || d.primCount % 2 || !maskCoord || !glassCoord ||
      maskCoord->Type != D3DDECLTYPE_FLOAT2 || glassCoord->Type != D3DDECLTYPE_FLOAT2)
    return false;
  const auto fetch = [&](const D3DVERTEXELEMENT9 *e, uint32_t index, float out[2]) {
    if (e->Stream >= 4)
      return false;
    const auto &s = d.streams[e->Stream];
    const int64_t vertex = int64_t(d.baseVertex) + index;
    const uint64_t at = uint64_t(s.offset) + uint64_t(vertex) * s.stride + e->Offset;
    if (!s.data || vertex < 0 || at + 8 > s.size)
      return false;
    std::memcpy(out, s.data + at, 8);
    return std::isfinite(out[0]) && std::isfinite(out[1]);
  };
  // Every quad's map first: a piece is drawn whole or not at all.
  const uint32_t quads = d.primCount / 2;
  std::vector<const void *> textures(quads);
  for (uint32_t q = 0; q < quads; ++q) {
    const uint64_t first = uint64_t(d.startIndex) + q * 6;
    const uint32_t bytes = d.indices.is32 ? 4 : 2;
    if ((first + 3) * bytes > d.indices.size)
      return false;
    float m[3][2], g[3][2];
    for (int k = 0; k < 3; ++k) {
      uint32_t index = 0;
      std::memcpy(&index, d.indices.data + (first + k) * bytes, bytes);
      if (!fetch(maskCoord, index, m[k]) || !fetch(glassCoord, index, g[k]))
        return false;
    }
    // The affine map of the first triangle (the quad is a parallelogram).
    const float a = m[1][0] - m[0][0], b = m[2][0] - m[0][0], c = m[1][1] - m[0][1], e = m[2][1] - m[0][1];
    const float det = a * e - b * c;
    if (!(std::fabs(det) > 1e-8f))
      return false;
    float map[6];
    for (int axis = 0; axis < 2; ++axis) {
      const float p = g[1][axis] - g[0][axis], r = g[2][axis] - g[0][axis];
      const float du = (p * e - r * c) / det, dv = (r * a - p * b) / det; // d glass / d mask.x, mask.y
      map[2 + axis] = du;
      map[4 + axis] = dv;
      map[axis] = g[0][axis] - du * m[0][0] - dv * m[0][1];
    }
    if (!(textures[q] = shardTexture(mask, glass, map)))
      return false;
  }
  g_inShardDraw = true;
  for (uint32_t q = 0; q < quads; ++q) {
    Draw piece = d;
    piece.startIndex = d.startIndex + q * 6;
    piece.primCount = 2;
    piece.texture0 = textures[q];
    onDraw(piece);
  }
  g_inShardDraw = false;
  ++g_stats.shatteredEdges;
  return true;
}

// Whether a draw is a $translucent world material blended as Source blends it
// (src alpha, 1 - src alpha) that shows nothing behind it: its texture's alpha
// at least 250 everywhere, without vertex alpha or an $alpha below one. Drawn
// as opaque it is a surface of the scene, which the raster effects drawn after
// it are hidden behind (gm_area51_rc1's race track sign: the lamps' glows
// showed through it). Texture opacity cached by name.
bool opaqueTranslucent(const Draw &d) {
  if (!d.blend || d.srcBlend != D3DBLEND_SRCALPHA || d.dstBlend != D3DBLEND_INVSRCALPHA ||
      d.blendOp != D3DBLENDOP_ADD)
    return false;
  const auto &m = engine_materials::bound();
  if (!m.known || !m.lightmapped || !m.translucent || m.additive || m.vertexAlpha || m.decal || m.decalShader)
    return false;
  float alpha = 1;
  if (engine_materials::currentFloat("$alpha", alpha) && alpha < 1)
    return false;
  const std::string &base = engine_materials::current().base;
  if (base.empty())
    return false;
  static std::unordered_map<std::string, bool> opaque; // render thread
  auto [it, added] = opaque.emplace(base, false);
  if (added) {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    if (bsp::textureRgba(base, width, height, rgba) && !rgba.empty()) {
      it->second = true;
      for (size_t i = 3; i < rgba.size() && it->second; i += 4)
        it->second = rgba[i] >= 250;
    }
  }
  return it->second;
}

void onDraw(const Draw &d) {
  // A screen camera's monitor (screenSlot): an opaque surface the runtime
  // traces the camera's view on, however the engine blends the texture.
  if (d.texture0Target && d.mainTarget && (d.blend || d.alphaTest) && screenSlot(d.texture0) >= 0) {
    Draw screen = d;
    screen.blend = screen.alphaTest = false;
    ++g_stats.screenDraws;
    float uv[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    const float *rows = d.vsConstants + 48 * 4;
    if (std::all_of(rows, rows + 8, [](float x) { return std::isfinite(x); }) &&
        (rows[0] != 0 || rows[1] != 0) && (rows[4] != 0 || rows[5] != 0))
      std::copy_n(rows, 8, uv);
    g_screenUv = uv;
    std::copy_n(uv, 8, g_screenUvRows);
    g_screenMaterial = engine_materials::currentName();
    onDraw(screen);
    g_screenUv = nullptr;
    return;
  }
  if (opaqueTranslucent(d)) {
    Draw opaque = d;
    opaque.blend = false;
    ++g_stats.opaqueTranslucent;
    return onDraw(opaque);
  }
  Timer timer(g_usDraw, &g_totalUs[0]);
  ++g_stats.draws;
  if (!g_ready || !g_shaderApi)
    return;
  // ShatteredGlass (a broken func_breakable_surf): each piece binds a shape
  // mask as $basetexture (the BreakableSurface proxy's models/brokenglass/
  // glassbroken_*: white, the shape in alpha, on TEXCOORD0) and the glass
  // itself as $detail (sampler 3, on the window's TEXCOORD2); the shader
  // multiplies them. Taken alone, the white mask made the pane white. A whole
  // block (its mask solid) is drawn as the glass; an edge piece keeps its
  // mask (the jagged outline) with the glass's mean colour.
  float shatteredTint[4] = {1, 1, 1, 1};
  bool shatteredEdge = false;
  if (const auto &m = engine_materials::bound();
      !g_inShardDraw && m.known && nameStarts(m.shader, "shatteredglass") && d.stageTextures[3] &&
      d.texture0 != d.stageTextures[3] && d.elements && findElement(d.elements, D3DDECLUSAGE_TEXCOORD, 2)) {
    const std::string &mask = engine_materials::current().base;
    if (mask.size() >= 6 && mask.compare(mask.size() - 6, 6, "_solid") == 0) {
      static D3DVERTEXELEMENT9 remapped[65]; // render thread
      size_t n = 0;
      for (const D3DVERTEXELEMENT9 *e = d.elements; e->Stream != 0xff && n < 64; ++e, ++n) {
        remapped[n] = *e;
        if (e->Usage == D3DDECLUSAGE_TEXCOORD && (e->UsageIndex == 0 || e->UsageIndex == 2))
          remapped[n].UsageIndex = BYTE(2 - e->UsageIndex);
      }
      remapped[n] = D3DVERTEXELEMENT9{0xff, 0, D3DDECLTYPE_UNUSED, 0, 0, 0}; // D3DDECL_END
      Draw glass = d;
      glass.texture0 = d.stageTextures[3];
      glass.elements = remapped;
      ++g_stats.shatteredSolid;
      onDraw(glass);
      return;
    }
    const std::string detail = engine_materials::currentTextureName("$detail");
    if (!detail.empty() && drawShardQuads(d, mask, detail))
      return;
    // Else the mask, in the glass's mean colour.
    if (!detail.empty() && bsp::textureMeanColor(detail, shatteredTint)) {
      shatteredEdge = true;
      ++g_stats.shatteredTinted;
    }
  }
  // $fogmaxdensity of the view being drawn (see SceneFog): cFogParams.z of a
  // world draw whose vertex shader computes fog.
  if (g_worldLists > 0 && d.mainTarget && d.vertexProgram && d.vsConstants) {
    const auto &constants = vsprog::info(*d.vertexProgram).constants;
    const float density = d.vsConstants[16 * 4 + 2];
    if (std::binary_search(constants.begin(), constants.end(), uint16_t(16)) && density >= 0 && density <= 1)
      g_fogMaxDensity = density;
  }
  if (d.stencil || d.texture0Target || d.target)
    census(d);
  // Flat draws into render-target textures or sampling them are the engine's
  // screen-space passes (effects): replayed by the runtime, not drawn here.
  // So are the draws of screenspace_general materials (addons' own shaders)
  // whatever their projection: a sky dome drawn against a depth buffer, say.
  const auto &material = engine_materials::bound();
  if (hostpass::wants(d)) {
    float projection[16];
    getMatrix(kMatrixProjection, projection);
    const bool perspective = std::fabs(std::fabs(projection[11]) - 1.0f) < 1e-3f && std::fabs(projection[15]) < 1e-3f;
    const bool custom = material.known && nameStarts(material.shader, "screenspace_general");
    if ((!perspective || custom) && hostpass::recordDraw(d, projection))
      return;
  }
  // A screen camera's view (render.RenderView, a point_camera): its scene
  // draws are taken like the main view's (role Screen), so what only that
  // camera sees is in the scene.
  const bool screenView = !d.mainTarget && d.target && screenSlot(d.target) >= 0;
  if (!d.mainTarget && !screenView) {
    // The engine's depth pass (addons' depth buffers): the runtime's own depth
    // stands in for the geometry.
    if (d.target && material.known && nameStarts(material.shader, "depthwrite") && hostpass::recordDepth(d))
      return;
    ++g_stats.offTarget;
    return;
  }
  if ((d.colorWrite & 7) == 0) {
    ++g_stats.depthOnly; // depth prepass / shadow depth
    return;
  }
  uint32_t categories = remixCategories(d.forkCategories) | g_markerCategories;
  // rtx.conf texture categories, by the base texture's dxvk hash.
  const uint64_t baseTextureHash = textureHash(d.texture0);
  categories |= categories::lookup(baseTextureHash);
  if (categories & (REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE | REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN)) {
    ++g_stats.ignored;
    return;
  }
  // Decals: paint on the surface below. Source draws them coplanar with it,
  // depth-biased (SHADER_POLYOFFSET_DECAL: every material with $decal, set by
  // CBaseShader, and DecalModulate by itself) and without depth writes; the
  // runtime composes tagged instances into their receiver's material
  // (resolveDecals in pathtrace.comp) and needs them lifted off it, since only
  // D3D applied the bias. A blended draw is one when its material says so
  // (engine_materials: $decal, or a Decal* shader), when D3D's depth bias is
  // set, or when rtx.conf tags its texture. What it paints follows from its
  // blend state:
  //   alpha    SRCALPHA / INVSRCALPHA (LightmappedGeneric, VertexLitGeneric,
  //            UnlitGeneric with $decal and $translucent: scorch marks,
  //            sprays, model decals, signs): covers by texture alpha x vertex
  //            alpha, coloured by texture x vertex colour.
  //   mod2x    DESTCOLOR / SRCCOLOR (DecalModulate: bullet holes, blood): the
  //            game computes dst x src x 2 on gamma values, mid-grey neutral.
  //            The runtime multiplies the receiver's linear albedo by
  //            linear(src) x 4.634 = linear(src) / linear(0.5): the same
  //            product taken to linear light, (2 src)^2.2, to within 8% from
  //            quarter grey up (darker texels are near black either way).
  //   multiply DESTCOLOR / ZERO or ZERO / SRCCOLOR (Modulate, the engine's
  //            shadow decals when kept): dst x src, albedo x linear(src).
  // (Blending on: the factors stay set after a decal, and the opaque draws
  // that followed one, with them still DESTCOLOR/SRCCOLOR, counted as
  // multiplying: their unlit or self-illuminated materials were not applied.)
  const bool multiply = d.blend &&
                        ((d.srcBlend == D3DBLEND_DESTCOLOR && (d.dstBlend == D3DBLEND_SRCCOLOR || d.dstBlend == D3DBLEND_ZERO)) ||
                         (d.srcBlend == D3DBLEND_ZERO && d.dstBlend == D3DBLEND_SRCCOLOR));
  constexpr uint32_t kDecalCategories =
      REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC |
      REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET;
  const engine_materials::Bound &bound = engine_materials::bound();
  DecalKind paint = kDecalNone; // what the blend state would paint
  if (!d.blend)
    paint = kDecalOpaque;
  else if (d.blendOp != D3DBLENDOP_ADD)
    paint = kDecalNone;
  else if (d.srcBlend == D3DBLEND_DESTCOLOR && d.dstBlend == D3DBLEND_SRCCOLOR)
    paint = kDecalMod2x;
  else if (multiply)
    paint = kDecalMultiply;
  else if (d.srcBlend == D3DBLEND_SRCALPHA && d.dstBlend == D3DBLEND_INVSRCALPHA)
    paint = kDecalAlpha;
  // The material's word counts for the draws that are its own: DecalModulate
  // only ever draws mod2x (a draw that reached D3D under it with another
  // blend state belongs to another material, see shadingFor).
  const bool ownDraw = bound.known && (!bound.modulate || paint == kDecalMod2x);
  uint32_t decalHow = 0;
  if (ownDraw && bound.decal)
    decalHow |= kDecalByFlag;
  if (ownDraw && bound.decalShader)
    decalHow |= kDecalByShader;
  if (d.depthBias)
    decalHow |= kDecalByBias;
  if (categories & kDecalCategories)
    decalHow |= kDecalByCategory;
  // Opaque and alpha-tested decals ($decal without $translucent) are not
  // paint to the runtime (a decal category only counts on a blended
  // instance): they are lifted like the others and stay ordinary surfaces.
  // rtx.conf's tag alone does not make one of an opaque draw, as before.
  // (An opaque draw is its bound $decal material's own only without depth
  // writes, which $decal turns off.)
  DecalKind decalKind = decalHow ? paint : kDecalNone;
  if (decalKind == kDecalOpaque && !(d.depthBias || ((decalHow & kDecalByFlag) && !d.zWrite)))
    decalKind = kDecalNone;
  // A decal material's draw with a blend state that paints nothing here
  // (additive: rasterized over the image like other glows; premultiplied).
  if (decalHow && paint == kDecalNone)
    ++g_stats.decalOtherBlend;
  if (bound.known && !ownDraw && (bound.decal || bound.decalShader))
    ++g_stats.decalNotItsBlend;
  const bool decal = decalKind == kDecalAlpha || decalKind == kDecalMod2x || decalKind == kDecalMultiply;
  const bool lifted = decalKind != kDecalNone;
  if (decal && !(categories & kDecalCategories))
    categories |= REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
  // Dev aid (OPENREMIX_PROXY_DUMP_DECALS=1): each blended or depth-biased
  // draw's state, once per texture and blend state, to bin/decals.txt.
  static const bool dumpDecals = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_DUMP_DECALS", v, sizeof(v)) && v[0] == '1';
  }();
  if (dumpDecals && (d.blend || lifted)) {
    static std::unordered_set<uint64_t> seen;
    if (seen.insert(baseTextureHash ^ (uint64_t(d.srcBlend) << 56) ^ (uint64_t(d.dstBlend) << 48) ^
                    (uint64_t(d.blend) << 40) ^ (uint64_t(decalHow) << 32))
            .second)
      if (FILE *f = _wfopen((g_moduleDir + L"\\decals.txt").c_str(), L"a")) {
        static const char *const kinds[] = {"-", "alpha", "mod2x", "multiply", "opaque"};
        fprintf(f, "tex %016llX %s | material %s shader %s flags %08X%s | decal %s by%s%s%s%s | blend %d src %u dst %u op %u "
                   "sepA %d srcA %u dstA %u depthBias %d zwrite %d alphaTest %d func %u ref %u cats %08X write %X cull %u | "
                   "vertex normal %d color %d bones %d dynamic %d world lists %d\n",
                (unsigned long long)baseTextureHash, bsp::materialName(baseTextureHash).c_str(),
                engine_materials::currentName(), bound.shader, unsigned(bound.flags),
                bound.known ? (bound.subRect ? " (sub-rectangle)" : "") : " (not read)", kinds[decalKind],
                decalHow & kDecalByFlag ? " $decal" : "", decalHow & kDecalByShader ? " shader" : "",
                decalHow & kDecalByBias ? " depth-bias" : "", decalHow & kDecalByCategory ? " rtx.conf" : "",
                int(d.blend), d.srcBlend, d.dstBlend, d.blendOp, int(d.separateAlpha), d.srcBlendAlpha, d.dstBlendAlpha,
                int(d.depthBias), int(d.zWrite), int(d.alphaTest), d.alphaFunc, d.alphaRef, categories, d.colorWrite,
                d.cullMode, int(findElement(d.elements, D3DDECLUSAGE_NORMAL, 0) != nullptr),
                int(findElement(d.elements, D3DDECLUSAGE_COLOR, 0) != nullptr),
                int(findElement(d.elements, D3DDECLUSAGE_BLENDINDICES, 0) != nullptr),
                int(d.streams[0].dynamic || d.indices.dynamic), g_worldLists);
        fclose(f);
      }
  }
  if (categories & (REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY |
                    REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL)) {
    // l4d2-rtx's player body for first-person shadows. The runtime has no
    // shadow-only instances yet, so keep it out of view.
    ++g_stats.playerBody;
    return;
  }
  // Blended draws (particles, sprites, glass, overlays) keep their D3D blend
  // state; extra passes over already drawn geometry are dropped as duplicates.
  if (d.blend)
    ++(d.srcBlend == D3DBLEND_ONE && d.dstBlend == D3DBLEND_ONE ? g_stats.additive : g_stats.blended);
  const auto *pos = findElement(d.elements, D3DDECLUSAGE_POSITION, 0);
  const bool skinnedDraw = findElement(d.elements, D3DDECLUSAGE_BLENDINDICES, 0) != nullptr;
  // Particles (see submitParticles). A shader that builds its geometry reads
  // POSITION as it likes (a curve parameter, any type).
  const ParticleKind particle =
      pos && pos->Stream < 4
          ? particleKind(d, bound, skinnedDraw, d.streams[pos->Stream].dynamic || d.indices.dynamic,
                         multiply && !decalHow)
          : kNotParticle;
  if (!pos || pos->Stream >= 4 || (pos->Type != D3DDECLTYPE_FLOAT3 && particle != kParticleExpanded)) {
    ++g_stats.noPosition;
    return;
  }
  uint32_t bonesPerVertex = 0;
  if (skinnedDraw) {
    bonesPerVertex = uint32_t(std::clamp(currentNumBones(), 1, 3));
    ++g_stats.skinned;
  }
  const auto &ps = d.streams[pos->Stream];
  if (!ps.data || !d.indices.data) {
    ++g_stats.noData;
    return;
  }

  const uint64_t viewKey = fnv(d.vsConstants + kRegViewProj * 4, 16 * sizeof(float));
  auto &view = g_views[viewKey];
  if (view.draws++ == 0) {
    view.order = g_viewOrder++;
    getMatrix(kMatrixView, view.view);
    getMatrix(kMatrixProjection, view.projection);
    view.screenTarget = screenView ? d.target : nullptr;
    // Source builds right-handed D3DX matrices (row vectors, camera looking
    // down -z, projection m[2][3] = -1, depth 0..1). Flipping view-space z
    // (negate the view's third column and the projection's third row) keeps
    // view * projection and yields the left-handed form the runtime expects.
    view.perspective = std::fabs(std::fabs(view.projection[11]) - 1.0f) < 1e-3f && std::fabs(view.projection[15]) < 1e-3f;
    static const float identityView[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    view.cameraSpace = view.perspective;
    for (int i = 0; i < 16 && view.cameraSpace; ++i)
      view.cameraSpace = std::fabs(view.view[i] - identityView[i]) < 1e-6f;
    if (view.perspective && !view.cameraSpace) {
      // Row vectors, rigid: camera = world * R + t, world = (camera - t) * R^T.
      const float *v = view.view;
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
          view.cameraToWorld[i][j] = v[i * 4 + j];
        view.cameraToWorld[i][3] = -(v[i * 4] * v[12] + v[i * 4 + 1] * v[13] + v[i * 4 + 2] * v[14]);
      }
    }
    if (view.perspective && view.projection[11] < 0) {
      for (int r = 0; r < 4; ++r)
        view.view[r * 4 + 2] = -view.view[r * 4 + 2];
      for (int c = 0; c < 4; ++c)
        view.projection[8 + c] = -view.projection[8 + c];
    }
  }
  if (!view.perspective && view.screenTarget)
    return; // a screen view's 2D draws: in its image only
  if (!view.perspective) {
    ++g_stats.flat; // HUD / VGUI
    dumpFlat(d);
    // The Refract shader drawn flat is a screen effect refracting a copy of
    // the frame (the water's $underwateroverlay, screen overlays), which the
    // runtime does not have: drawn, it covered the view (white under water).
    if (engine_materials::bound().known && nameStarts(engine_materials::bound().shader, "refract")) {
      ++g_stats.refractOverlaysDropped;
      return;
    }
    captureUi(d);
    return;
  }
  // Camera-space draws (see CameraSpace) belong to the view drawn before them.
  if (!view.cameraSpace) {
    g_cameraSpace.key = viewKey;
    g_cameraSpace.valid = true;
  }
  const bool dynamicDraw = ps.dynamic || d.indices.dynamic;
  uint64_t instanceView = viewKey;
  const float (*cameraToWorld)[4] = nullptr;
  if (view.cameraSpace) {
    // Particles, and whatever else is blended and rebuilt every frame; the
    // rest stays out as before (a view without a camera).
    const bool placed = particle != kNotParticle || (d.blend && dynamicDraw && !skinnedDraw);
    if (placed && g_cameraSpace.valid) {
      instanceView = g_cameraSpace.key;
      cameraToWorld = g_views[instanceView].cameraToWorld;
      ++(particle != kNotParticle ? g_cameraSpaceParticles : g_cameraSpaceOther);
    } else {
      ++(placed ? g_cameraSpaceNoView : g_cameraSpaceLeft);
    }
  }
  // The BSP world replaces the engine's opaque world surfaces (static vertex
  // buffers; their index buffers are rebuilt per frame); its blended draws
  // (translucent brushes, decals, overlays) and dynamic ones (the 2D skybox)
  // still come from the engine.
  // Overlays (COverlayMgr::RenderOverlays, openremix_proxy_overlays).
  if (g_overlays > 0 && bsp::overlaysActive()) {
    ++g_stats.bspOverlaysDropped;
    return;
  }
  // Static props likewise (matched by placement; translucent ones stay).
  float eye[3];
  for (int i = 0; i < 3; ++i)
    eye[i] = -(view.view[12] * view.view[i * 4] + view.view[13] * view.view[i * 4 + 1] +
               view.view[14] * view.view[i * 4 + 2]);
  // Particles leave here: before the flashlight-pass filter (an additive
  // Spritecard or unlit draw is never that pass, whatever texture it holds),
  // and outside the decal, emissive and mesh-cache logic below.
  if (particle != kNotParticle &&
      submitParticles(d, particle, instanceView, cameraToWorld, categories & ~kDecalCategories, eye, view.projection))
    return;
  // What else is blended and rebuilt every frame, for the statistics: the
  // candidates the particle rules above did not take.
  if (particle == kNotParticle && particlesEnabled() && d.blend && !skinnedDraw && dynamicDraw) {
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    const char *name = engine_materials::currentName();
    const std::string key = *name ? name : bound.known ? "(unnamed)" : "(material not read)";
    if (g_particlesLeft.size() < 64 || g_particlesLeft.count(key)) {
      ParticleLeft &left = g_particlesLeft[key];
      if (!left.draws)
        left.shader = bound.shader;
      ++left.draws;
      left.srcBlend = d.srcBlend;
      left.dstBlend = d.dstBlend;
    }
  }
  if (!d.blend && bsp::isStaticProp(d.vsConstants + kRegModel * 4, eye)) {
    // Their ambient cubes still feed the sky light estimate (onPresent).
    if (findElement(d.elements, D3DDECLUSAGE_BLENDINDICES, 0))
      view.ambientUp.push_back({d.vsConstants[kRegAmbientUp * 4], d.vsConstants[kRegAmbientUp * 4 + 1],
                                d.vsConstants[kRegAmbientUp * 4 + 2]});
    ++g_stats.bspPropsDropped;
    return;
  }
  // The engine's flashlight pass over the world: with a projected texture on,
  // it re-draws the lit surfaces additively with the cookie as texture 0
  // (captured, the cookie got pasted over every surface). Those lights are
  // path traced (engine_lights), so the pass goes. The shadow manager's
  // virtual iterator is not what the world pass uses, so it is caught here.
  // Models get the same additive pass outside the world lists, where other
  // additive draws are legitimate (glows, sprites). Two signs of the pass:
  // the cookie as texture 0 (world-style shaders; the world pass teaches
  // which textures are cookies), or, for models, whose pass keeps the base
  // texture there, the same buffers and range as an opaque draw earlier in
  // the frame (effects are drawn once). Additive is ONE/ONE, or
  // SRCALPHA/ONE for translucent materials (decals).
  struct {
    const void *vertices, *indexBuffer;
    uint32_t offset, startIndex, primCount, minIndex;
    int baseVertex;
    uint32_t padding; // hashed as bytes: no indeterminate padding
    uint64_t view;
  } drawRange = {ps.buffer, d.indices.buffer, ps.offset, d.startIndex, d.primCount, d.minIndex, d.baseVertex, 0, viewKey};
  static std::unordered_set<uint64_t> opaqueDrawn; // render thread only
  static uint64_t opaqueFrame = ~0ull;
  if (opaqueFrame != g_frame) {
    opaqueDrawn.clear();
    opaqueFrame = g_frame;
  }
  const uint64_t geometryKey = fnv(&drawRange, sizeof(drawRange));
  if (!d.blend && !ps.dynamic)
    opaqueDrawn.insert(geometryKey);
  if (d.blend && d.dstBlend == D3DBLEND_ONE && (d.srcBlend == D3DBLEND_ONE || d.srcBlend == D3DBLEND_SRCALPHA)) {
    static std::unordered_set<const void *> cookies; // render thread only
    static uint32_t cookieEpoch = ~0u;
    if (cookieEpoch != g_skyEpoch) { // a new map: texture pointers are reused
      cookies.clear();
      cookieEpoch = g_skyEpoch;
    }
    if (const auto lights = engine_lights::current(); lights && !lights->projected.empty()) {
      if (g_worldLists > 0 && d.texture0)
        cookies.insert(d.texture0);
      // A third sign: a lit surface material that is not additive itself.
      // Nothing else draws one additively, and it catches the pass's dynamic
      // batches (world pieces in the beam, re-cut every frame as the
      // flashlight moves: each was a new overlay mesh for the runtime to
      // upload, tens of ms a frame until every piece had been seen).
      const bool litPass = engine_materials::currentIsLitOpaque();
      if (g_worldLists > 0 || cookies.count(d.texture0) || (!ps.dynamic && opaqueDrawn.count(geometryKey)) || litPass) {
        ++g_stats.flashlightPasses;
        g_stats.flashlightLitPasses += litPass;
        return;
      }
    }
  }
  if (g_worldLists > 0 && bsp::active()) {
    if (!d.blend && !ps.dynamic) {
      ++g_stats.bspDropped;
      return;
    }
    ++(d.blend ? g_stats.bspKeptBlended : g_stats.bspKeptDynamic);
    if (!d.blend) {
      std::lock_guard<std::mutex> lock(g_transientSampleLock);
      ++g_keptWorldTextures[baseTextureHash];
    }
  } else if (!d.blend && !ps.dynamic && bsp::active() && d.primCount) {
    // A func_lod brush, which the BSP world bakes (bsp::isBakedBrushEntity):
    // its first triangle's corners are the baked faces' corners. (Static
    // vertices like the world's; the index buffer is rebuilt per frame. A
    // fading one is blended and stays: the baked copy is opaque.)
    const float *corner[3] = {};
    for (uint32_t k = 0; k < 3; ++k) {
      const size_t at = size_t(d.startIndex + k) * (d.indices.is32 ? 4 : 2);
      if (at + (d.indices.is32 ? 4 : 2) > d.indices.size)
        break;
      uint32_t index = 0;
      std::memcpy(&index, d.indices.data + at, d.indices.is32 ? 4 : 2);
      const int64_t vertex = int64_t(d.baseVertex) + index;
      const size_t offset = size_t(ps.offset) + size_t(vertex) * ps.stride + pos->Offset;
      if (vertex < 0 || offset + 12 > ps.size)
        break;
      corner[k] = reinterpret_cast<const float *>(ps.data + offset);
    }
    if (corner[2] && bsp::isBakedBrushEntity(corner[0], corner[1], corner[2])) {
      ++g_stats.bspBrushEntitiesDropped;
      return;
    }
  }

  // What the Source material bound for this draw does with its texture
  // (engine_materials). Additive and multiplying draws stay as they are:
  // rasterized over the image, or paint on what is below.
  const bool additiveDraw = d.blend && d.dstBlend == D3DBLEND_ONE;
  engine_materials::Shading shading;
  if (!additiveDraw && !multiply)
    shading = shadingFor(d.texture0, engine_materials::current());
  if (shatteredEdge)
    std::copy_n(shatteredTint, 3, shading.albedo);
  probeMaterialDraw(d.texture0, engine_materials::current(), shading, additiveDraw, multiply);
  probeShaderDraw(d, shading);
  if (shading.mode == engine_materials::Shading::Unlit)
    vb_origins::countUnlit(d.streams[0].buffer);
  // Lua-built meshes (a Lua Mesh(): InfMap terrain and scripted maps) are
  // static buffers made outside every engine allocation bracket
  // (vb_origins). Their unlit materials are lit as surfaces: such maps bake
  // their shading into vertex colours for want of lighting, which the path
  // tracer provides. Model sprites, screens and 3D2D panels (engine buffers)
  // stay unlit. OPENREMIX_PROXY_LUA_MESHES_LIT=0 keeps them unlit.
  static const bool litLuaMeshes = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_LUA_MESHES_LIT", v, sizeof(v)) && v[0] == '0');
  }();
  bool luaLit = false, luaColors = false;
  if (litLuaMeshes && shading.mode == engine_materials::Shading::Unlit) {
    vb_origins::Origin origin;
    luaLit = vb_origins::find(d.streams[0].buffer, origin) && !origin.allocator && !origin.dynamic;
    if (luaLit) {
      luaColors = shading.vertexColor;
      shading = {};
      shading.base = engine_materials::current().base;
      ++g_stats.luaLitDraws;
    }
  }
  // An unlit material's $vertexcolor: its vertices carry the colour (3D2D
  // panels, sprites). Other COLOR0 streams (baked prop lighting) do not.
  const bool hasColors = findElement(d.elements, D3DDECLUSAGE_COLOR, 0) != nullptr;
  const bool unlitColors = shading.mode == engine_materials::Shading::Unlit && shading.vertexColor && hasColors;
  const bool hueColors = luaColors && hasColors; // a lit Lua mesh's $vertexcolor: its hue
  // An alpha decal's vertices carry its colour and its fade ($vertexcolor,
  // $vertexalpha: scorch marks and sprays fade out through vertex alpha; the
  // world shaders read both with $vertexcolor alone). mod2x and multiply
  // decals ignore vertex colours (DecalModulate fades towards grey by vertex
  // alpha when both flags are set, which a texture stage cannot express: such
  // a decal keeps its full strength until the engine removes it).
  // OPENREMIX_PROXY_DECAL_COLORS=0: decals as their texture alone.
  static const bool decalColorsEnabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_DECAL_COLORS", v, sizeof(v)) && v[0] == '0');
  }();
  const bool decalMaterial = (decalHow & (kDecalByFlag | kDecalByShader)) != 0;
  const bool decalAlphaFade = decalColorsEnabled && decalKind == kDecalAlpha && decalMaterial && hasColors &&
                              (bound.vertexAlpha || (bound.lightmapped && bound.vertexColor));
  const bool decalColors = decalColorsEnabled && !unlitColors && decalKind == kDecalAlpha && decalMaterial && hasColors &&
                           (bound.vertexColor || decalAlphaFade);
  // Vertex colours the mesh carries (texture x vertex colour by default).
  const bool meshColors = unlitColors || decalColors || hueColors;
  struct HueScope {
    explicit HueScope(bool on) { g_hueColors = on; }
    ~HueScope() { g_hueColors = false; }
  } hueScope(hueColors);
  if (decalKind == kDecalMod2x && decalMaterial && bound.vertexColor && bound.vertexAlpha)
    ++g_stats.decalModulateFade;
  // Decals over decals: all at one height they were coplanar, and which one a
  // ray met first changed with the camera angle (flicker). The engine draws
  // them bottom to top, so each draw sits a little above the one before, and
  // within a draw (a batch of one material's decals) each vertex a little
  // above the one before it; the next draw starts above the last. Above the
  // map's overlays (0.1) and its second layers (0.05); in all under the
  // runtime's attach distance (~2 inches): past kDecalRise they share a height.
  constexpr float kDecalOffset = 0.15f, kDecalVertexStep = 0.001f, kDecalDrawGap = 0.004f, kDecalRise = 1.2f; // inches
  static uint64_t decalFrame = ~0ull;
  static float decalRise = 0; // how far this frame's decals have stacked up
  if (decalFrame != g_frame) {
    decalFrame = g_frame;
    decalRise = 0;
  }
  const float lift = lifted ? kDecalOffset + decalRise : 0;
  if (lifted) // the next draw starts above this one's last vertex
    decalRise = std::min(decalRise + std::min(float(d.numVertices) * kDecalVertexStep, 0.3f) + kDecalDrawGap, kDecalRise);
  g_liftVertexStep = lifted ? kDecalVertexStep : 0;
  const float windingSign = lifted ? faceNormalSign(d.cullMode) : 1;

  struct Key {
    const void *vb, *ib, *decl;
    uint32_t vbVersion, ibVersion, offset, stride;
    int baseVertex;
    uint32_t minIndex, numVertices, startIndex, primCount, type, bonesPerVertex;
    // The key is hashed as bytes. Without this member its last four were the
    // structure's tail padding, whatever the stack held: the same draw then
    // hashed to another mesh from one frame to the next (rebuilt, or one of
    // several cached copies in turn), each a new instance to the runtime with
    // no previous frame, so no motion vectors, until the stack happened to
    // settle (a restart).
    uint32_t padding;
  } key = {ps.buffer, d.indices.buffer, d.declaration, ps.version, d.indices.version, ps.offset, ps.stride,
           d.baseVertex, d.minIndex, d.numVertices, d.startIndex, d.primCount, uint32_t(d.type), bonesPerVertex, 0};
  static_assert(sizeof(Key) == 3 * sizeof(void *) + 12 * sizeof(uint32_t), "Key must have no padding: it is hashed as bytes");
  uint64_t hash = fnv(&key, sizeof(key)) ^ (lifted ? 0xdeca1deca1deca1ull : 0) ^
                  (unlitColors ? 0xc0105c0105c0105ull : 0) ^ (decalColors ? 0x0decac0105c0105ull : 0) ^
                  (hueColors ? 0x4e0c0105c0105ull : 0) ^
                  (lifted && windingSign > 0 ? 0x5197000000000000ull : 0);
  if (g_screenUv)
    hash = fnv(g_screenUv, 8 * sizeof(float), hash);
  // A flexed draw (see flexScale) changes every frame although its own
  // buffers are static. `flexContent` names this frame's deltas: the flex
  // mesh's place and version (0: at rest).
  const float flex = particle == kNotParticle ? flexScale(d) : 0;
  uint64_t flexContent = 0;
  if (flex != 0) {
    const auto &fs = d.streams[findElement(d.elements, D3DDECLUSAGE_POSITION, 1)->Stream];
    const struct {
      const void *buffer;
      uint32_t version, offset;
    } flexKey = {fs.buffer, fs.version, fs.offset};
    flexContent = fnv(&flexKey, sizeof(flexKey)) | 1;
    ++g_stats.flexed;
  }
  // It gets a mesh of its own (g_flexMeshes), apart from the cached at-rest
  // mesh of the same buffers, and keeps it while the face is at rest between
  // expressions: one mesh and one instance for as long as it is drawn. Decals
  // on a flexed face and flexed draws from dynamic buffers stay per frame,
  // named by their deltas.
  const bool dynamicBuffers = ps.dynamic || d.indices.dynamic;
  const uint64_t flexHash = hash ^ 0xf1e8f1e8f1e8f1e8ull;
  const bool flexTracked = !dynamicBuffers && !lifted && (flex != 0 || g_flexMeshes.count(flexHash) != 0);
  if (flex != 0 && !flexTracked)
    hash = fnv(&flexContent, sizeof(flexContent), hash);
  const bool transientDraw = dynamicBuffers || (flex != 0 && !flexTracked);

  remixapi_MeshHandle mesh = nullptr;
  uint32_t boneCount = 0;
  uint64_t geometry = 0;
  bool faceNormals = false, meshBuilt = false;
  if (flexTracked) {
    FlexMesh &f = g_flexMeshes[flexHash];
    if (!f.handle || f.content != flexContent) {
      const remixapi_MeshHandle refreshed =
          buildMesh(d, flexHash, bonesPerVertex, &boneCount, 0, meshColors, meshColors, windingSign, &faceNormals, flex,
                    nullptr, true);
      if (refreshed) {
        f.handle = refreshed;
        f.content = flexContent;
        f.bones = boneCount;
        if (!f.geometry)
          f.geometry = dxvk::geometryHash(d);
        ++g_stats.flexRefreshed;
      } else if (!f.handle) {
        g_flexMeshes.erase(flexHash);
        return;
      }
    }
    f.lastFrame = g_frame;
    mesh = f.handle;
    boneCount = f.bones;
    geometry = f.geometry;
  } else if (transientDraw) {
    // Dynamic buffers are rewritten every frame: build, draw once, destroy.
    // (Decals on the world, on displacements and on models all come this
    // way: the engine rebuilds their batches every frame.)
    g_transientIndices += d.primCount * 3;
    {
      std::lock_guard<std::mutex> lock(g_transientSampleLock);
      ++g_transientDecls[d.declaration];
    }
    // Additive as onPresent classifies it (destination factor ONE): raster only.
    mesh = buildMesh(d, hash, bonesPerVertex, &boneCount, lift, additiveDraw || meshColors, meshColors, windingSign,
                     &faceNormals, flex);
    if (!mesh) {
      g_stats.decalNoMesh += lifted;
      return;
    }
    meshBuilt = true;
    g_transient.push_back(mesh);
    // The replacement key only matters to mods.
    if (g_modsResult == REMIXAPI_ERROR_CODE_SUCCESS)
      geometry = dxvk::geometryHash(d);
  } else if (auto it = g_meshes.find(hash); it != g_meshes.end()) {
    mesh = it->second.handle;
    boneCount = it->second.bones;
    geometry = it->second.geometry;
    it->second.lastFrame = g_frame;
    ++g_stats.cacheHits;
  } else {
    mesh = buildMesh(d, hash, bonesPerVertex, &boneCount, lift, meshColors, meshColors, windingSign, &faceNormals);
    if (!mesh) {
      g_stats.decalNoMesh += lifted;
      return;
    }
    meshBuilt = true;
    geometry = dxvk::geometryHash(d);
    g_meshes[hash] = {mesh, g_frame, boneCount, geometry};
    {
      std::lock_guard<std::mutex> lock(g_transientSampleLock);
      const char *name = engine_materials::currentName();
      MeshBuilds &b = g_meshBuilds[*name ? name : "(unnamed)"];
      ++b.count;
      std::memcpy(b.previous, b.last, sizeof(b.last));
      const uint64_t fields[13] = {uint64_t(uintptr_t(key.vb)), uint64_t(uintptr_t(key.ib)),
                                   uint64_t(uintptr_t(key.decl)), key.vbVersion, key.ibVersion, key.offset,
                                   key.stride, uint64_t(int64_t(key.baseVertex)), key.minIndex, key.numVertices,
                                   key.startIndex, key.primCount, key.bonesPerVertex};
      std::memcpy(b.last, fields, sizeof(fields));
    }
  }
  if (lifted) {
    ++g_stats.decalKinds[decalKind];
    g_stats.decalByFlag += (decalHow & kDecalByFlag) != 0;
    g_stats.decalByShader += (decalHow & kDecalByShader) != 0;
    g_stats.decalByBias += (decalHow & kDecalByBias) != 0;
    g_stats.decalByCategory += (decalHow & kDecalByCategory) != 0;
    g_stats.decalBiasOnly += decalHow == kDecalByBias;
    g_stats.decalMaterialOnly += decalMaterial && !(decalHow & kDecalByBias);
    g_stats.decalWorld += g_worldLists > 0;
    g_stats.decalSkinned += bonesPerVertex != 0;
    if (meshBuilt)
      ++(faceNormals ? g_stats.decalFaceNormals : g_stats.decalVertexNormals);
    g_stats.decalVertexColors += decalColors || (unlitColors && decal);
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    const char *name = engine_materials::currentName();
    const std::string key = *name ? name : bound.known ? "(unnamed)" : "(material not read)";
    if (g_decalsSeen.size() < 96 || g_decalsSeen.count(key)) {
      DecalSeen &seen = g_decalsSeen[key];
      ++seen.draws;
      seen.how |= decalHow;
      seen.kind = decalKind;
      seen.shader = bound.shader;
    }
  }

  Instance inst;
  inst.mesh = mesh;
  // Ropes and cables (keyframe_rope, move_rope: the Cable shader) sample their
  // $bumpmap on sampler 0 and their $basetexture on sampler 1: texture 0 is
  // cable/cablenormalmap, which as albedo painted every rope in the normal
  // map's blues and purples. Their material is the bound one's $basetexture,
  // found by name among the uploaded textures (as emissive masks are); until
  // that is uploaded a rope goes untextured rather than normal-mapped. Their
  // COLOR0 is the lighting the engine sampled along the rope on the CPU, so
  // it is not forwarded: the path tracer lights them.
  const bool nearest = isPointSampler(d.samplerStates, 0);
  if (bound.known && nameStarts(bound.shader, "cable")) {
    inst.material = materialForName(engine_materials::current().base, nearest);
    ++(inst.material ? g_stats.cableDraws : g_stats.cableNoBase);
  } else {
    inst.material = d.texture0 ? materialFor(d.texture0, shading, nearest) : nullptr;
  }
  if (!d.texture0 && shading.mode != engine_materials::Shading::Lit)
    ++g_stats.emissiveNoTexture;
  if (d.texture0Target && screenSlot(d.texture0) < 0)
    d3d::requestTargetContents(d.texture0);
  inst.blend = blendState(d);
  // The modulation colour: the entity's render colour and alpha (a tinted
  // volumetric light shaft, a prop painted with the colour tool, a fading
  // effect) times the material's $color and $alpha. VertexLitGeneric,
  // UnlitGeneric and UnlitTwoTexture take it in pixel shader c1, linear
  // (g_DiffuseModulation), and multiply the texture by it: here the texture
  // factor, whose rgb the runtime decodes as sRGB. (Their vertex shaders' c47
  // is not it: 16, 16, 16, 1 under UnlitTwoTexture.) A mesh drawn with its own
  // COLOR0 keeps that (the colour stage has two arguments), and so does a
  // draw whose modulation is white.
  if (d.psConstants && bound.known && !multiply && !hasColors && !decalKind &&
      (nameStarts(bound.shader, "vertexlitgeneric") || nameStarts(bound.shader, "unlitgeneric") ||
       nameStarts(bound.shader, "unlittwotexture") || nameStarts(bound.shader, "wireframe"))) {
    const float *m = d.psConstants + 1 * 4;
    uint32_t factor = 0;
    for (int c = 0; c < 4; ++c) {
      float v = std::isfinite(m[c]) ? std::clamp(m[c], 0.0f, 1.0f) : 1.0f;
      if (c < 3)
        v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
      else if (!d.blend)
        v = 1;
      factor |= uint32_t(v * 255.0f + 0.5f) << (c == 3 ? 24 : 16 - c * 8);
    }
    // $blendtintbybasealpha (VertexLitGeneric: a player model's clothes in
    // the player's colour): the tint counts where the base texture's alpha
    // says so, lerp(base, base x tint, base alpha). The runtime's colour
    // operation 7.
    float value = 0;
    const bool byBaseAlpha = !d.blend && engine_materials::currentFloat("$blendtintbybasealpha", value) && value != 0;
    // The same switch with a near-black $color2 and $phongalbedotint is a
    // metalness idiom (Modern Warfare base weapons: "$color2 [0 0 0]"): the
    // alpha marks metal, whose diffuse colour the tint removes and whose
    // albedo-coloured Phong highlight is then all it shows. There is no such
    // highlight here, and the tint alone left brass and steel black: these
    // keep their texture's colour.
    // (A PBR conversion that reads that alpha as metal: always.)
    const bool metalIdiom = byBaseAlpha && ((std::max({m[0], m[1], m[2]}) < 0.25f &&
                                             engine_materials::currentFloat("$phongalbedotint", value) && value != 0) ||
                                            (shading.pbr.active && shading.pbr.metalByBaseAlpha));
    g_stats.modulatedMetal += metalIdiom;
    if (factor != 0xffffffffu && !metalIdiom) {
      ++g_stats.modulatedDraws;
      {
        std::lock_guard<std::mutex> lock(g_transientSampleLock);
        std::copy_n(m, 4, g_modulationSample);
      }
      // What adds nothing is not drawn (Source keeps faded effects alive).
      if (additiveDraw && (!(factor & 0xffffff) || (d.srcBlend == D3DBLEND_SRCALPHA && !(factor >> 24)))) {
        ++g_stats.modulatedBlack;
        return;
      }
      g_stats.modulatedMasked += byBaseAlpha;
      inst.blend.tFactor = factor;
      inst.blend.textureColorOperation = byBaseAlpha ? 7 : 3;
      inst.blend.textureColorArg1Source = 1;
      inst.blend.textureColorArg2Source = 3;
      if (d.blend) {
        inst.blend.textureAlphaOperation = 3;
        inst.blend.textureAlphaArg1Source = 1;
        inst.blend.textureAlphaArg2Source = 3;
      }
    }
  }
  if (unlitColors) {
    ++g_stats.unlitVertexColorDraws;
    // $vertexalpha: the vertices' alpha fades the texture's (the runtime's
    // default takes the texture's alone). Modulate texture x vertex.
    if (shading.vertexAlpha) {
      inst.blend.textureAlphaOperation = 3;
      inst.blend.textureAlphaArg1Source = 1;
      inst.blend.textureAlphaArg2Source = 2;
    }
  } else if (decalColors) {
    // The runtime's default colour stage is texture x vertex colour, its
    // alpha stage the texture alone.
    if (decalAlphaFade) {
      inst.blend.textureAlphaOperation = 3;
      inst.blend.textureAlphaArg1Source = 1;
      inst.blend.textureAlphaArg2Source = 2;
    }
    if (!bound.vertexColor) { // $vertexalpha alone: the texture's colour
      inst.blend.textureColorOperation = 1;
      inst.blend.textureColorArg1Source = 1;
      inst.blend.textureColorArg2Source = 2;
    }
  }
  // Experiment (OPENREMIX_PROXY_CULL_BLENDED=1): blended surfaces keep D3D's
  // culling (single-sided instances). Off by default: the runtime's front-face
  // winding has not been checked against D3D's, and on openremix-testing it did
  // not change the transparent layer count or cost.
  static const bool cullBlended = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_CULL_BLENDED", v, sizeof(v)) && v[0] == '1';
  }();
  inst.singleSided = cullBlended && d.blend && d.cullMode != D3DCULL_NONE;
  // Opt-in (OPENREMIX_PROXY_CULL_BACKFACES_MODELS=1), not verified in game:
  // engine-drawn models hide from the camera the back faces D3D culls
  // (remixapi_InstanceInfoCameraCullingEXT; shadows and bounces still meet
  // both sides). Only D3DCULL_CCW, Source's default, whose visible triangles
  // wind clockwise in world space like the runtime's front faces
  // (faceNormalSign); a draw with the opposite mode stays two-sided.
  static const bool cullModels = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_CULL_BACKFACES_MODELS", v, sizeof(v)) && v[0] == '1';
  }();
  inst.cameraCullBack = cullModels && !d.blend && !inst.singleSided && d.cullMode == D3DCULL_CCW;
  inst.categories = categories;
  inst.replacementHash = geometry ? geometry ^ baseTextureHash : 0;
  inst.persistent = !transientDraw;
  noteReplacementKey(inst.replacementHash);
  inst.texture0 = d.texture0;
  if (boneCount > 1) {
    // Bone matrices map the bind pose straight to world space.
    inst.bones.resize(boneCount);
    std::memcpy(inst.bones.data(), d.vsConstants + kRegModel * 4, boneCount * sizeof(remixapi_Transform));
    static const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    std::memcpy(inst.transform, identity, sizeof(inst.transform));
  } else {
    // The model matrix, which is also bone 0 (a rigid studio model's only one).
    std::memcpy(inst.transform, d.vsConstants + kRegModel * 4, sizeof(inst.transform));
    g_stats.rigidModels += boneCount == 1;
  }
  inst.view = instanceView;
  inst.offTarget = !d.mainTarget;
  inst.primitives = d.primCount;
  inst.firstIndex = d.startIndex;
  inst.skinned = skinnedDraw;
  if (cameraToWorld) { // see CameraSpace
    if (inst.bones.empty())
      premultiply(cameraToWorld, inst.transform);
    else
      for (auto &bone : inst.bones)
        premultiply(cameraToWorld, bone.matrix);
  }
  // EyeRefract (Episode Two and later models, Black Mesa's): sampler 0 is the
  // cornea's normal map, which as albedo gave the eye every colour of a
  // normal map. The eye's picture, white included, is the iris texture on
  // sampler 1, at coordinates the vertex shader projects from the world
  // position with the same two registers as the Eyes shader (the refraction
  // through the cornea, which shifts them a little, is left out). The pixel
  // shader samples the iris at half the projected coordinates about the centre
  // (its "sphere" coordinates, uv * 0.5 + 0.25: the projection spans the
  // cornea, the texture the whole eyeball); unscaled the iris came out at half
  // its size. The eyeball
  // is submitted with that texture and those coordinates: a mesh of its own,
  // refreshed in place as the gaze and the head move (g_flexMeshes).
  if (!d.blend && d.texture1 && bound.known && nameStarts(bound.shader, "eyerefract")) {
    ++g_stats.eyes;
    uint32_t eyeBones = 0;
    g_liftVertexStep = 0;
    const remixapi_MaterialHandle eyeMaterial = clampedMaterialFor(d.texture1, isPointSampler(d.samplerStates, 1));
    const uint64_t eyeHash = (dynamicBuffers ? hash : flexHash) ^ 0xe7e2ef2ac7e7e2efull;
    remixapi_MeshHandle eyeMesh =
        eyeMaterial ? buildMesh(d, eyeHash, bonesPerVertex, &eyeBones, 0, false, false, 1, nullptr, flex,
                                d.vsConstants + kRegEyeIrisU * 4, !dynamicBuffers, 0.5f)
                    : nullptr;
    if (dynamicBuffers) {
      if (eyeMesh)
        g_transient.push_back(eyeMesh);
    } else if (eyeMaterial) {
      FlexMesh &f = g_flexMeshes[eyeHash];
      if (eyeMesh)
        f.handle = eyeMesh;
      f.lastFrame = g_frame;
      eyeMesh = f.handle; // a failed refresh keeps the last one
      if (!eyeMesh)
        g_flexMeshes.erase(eyeHash);
    }
    if (eyeMesh) {
      inst.mesh = eyeMesh;
      inst.material = eyeMaterial;
      inst.texture0 = d.texture1;
      inst.persistent = !dynamicBuffers;
      inst.replacementHash = 0;
      ++g_stats.eyeRefracts;
    }
  }
  if (!stencilRole(d, inst))
    return;
  // D3DFILL_WIREFRAME (the Wireframe shader; any material with $wireframe):
  // the edges of its triangles, unlit, in its colour (texture x modulation x
  // vertex colour, as the draw's), with the draw's depth state. Not traced: a
  // line has no surface.
  if (d.fillMode == D3DFILL_WIREFRAME) {
    inst.wireframe = inst.rasterOnly = true;
    inst.material = d.texture0 ? materialFor(d.texture0, 0, nearest) : nullptr;
    inst.persistent = false;
    inst.depthTest = d.depthTest && inst.depthTest;
    inst.depthWrite = d.zWrite;
    inst.categories &= ~(REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC |
                         REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET);
    ++g_stats.wireframeDraws;
  }
  g_instances.push_back(inst);
  // Eyes (Source's Eyes shader; not EyeRefract): the draw above is the eyeball
  // with its sclera texture, sampler 0. The iris and pupil are a second
  // texture (sampler 1) the pixel shader lays over it by its alpha, at
  // coordinates the vertex shader projects from the world position. Here:
  // the same triangles again with those coordinates, as an alpha decal on the
  // eyeball (lifted a hair off it; the runtime composes decals into their
  // receiver). The projection follows the gaze and the head, so its texture
  // coordinates change every frame: one mesh per eye, refreshed in place like
  // a flexed face (g_flexMeshes), which the tracer refits instead of building
  // anew and which stays one instance from frame to frame.
  if (!d.blend && d.texture1 && bound.known && nameStarts(bound.shader, "eyes")) {
    constexpr float kIrisLift = 0.03f; // inches; an eye is about one across
    ++g_stats.eyes;
    uint32_t irisBones = 0;
    g_liftVertexStep = 0;
    const remixapi_MaterialHandle irisMaterial = clampedMaterialFor(d.texture1, isPointSampler(d.samplerStates, 1));
    // From buffers the engine rewrites (not seen for eyes) it stays per frame.
    const uint64_t irisHash = (dynamicBuffers ? hash : flexHash) ^ 0x1215e5e5e5e51215ull;
    remixapi_MeshHandle irisMesh =
        // Lifted out of the eyeball: by face normals (an eye has no vertex
        // normals) these wind as the model's do (faceNormalSign; 1 put the
        // iris inside it, where only a decal's attachment still found it).
        irisMaterial ? buildMesh(d, irisHash, bonesPerVertex, &irisBones, kIrisLift, false, false,
                                 faceNormalSign(d.cullMode), nullptr, flex,
                                 d.vsConstants + kRegEyeIrisU * 4, !dynamicBuffers)
                     : nullptr;
    if (dynamicBuffers) {
      if (irisMesh)
        g_transient.push_back(irisMesh);
    } else if (irisMaterial) {
      FlexMesh &f = g_flexMeshes[irisHash];
      if (irisMesh)
        f.handle = irisMesh;
      f.lastFrame = g_frame;
      irisMesh = f.handle; // a failed refresh keeps the last one
      if (!irisMesh)
        g_flexMeshes.erase(irisHash);
    }
    if (irisMesh) {
      Instance iris = inst; // same bones / transform, view and culling
      iris.mesh = irisMesh;
      iris.material = irisMaterial;
      iris.texture0 = d.texture1;
      Draw over = d; // lerp(sclera, iris, iris.a)
      over.blend = true;
      over.srcBlend = D3DBLEND_SRCALPHA;
      over.dstBlend = D3DBLEND_INVSRCALPHA;
      over.blendOp = D3DBLENDOP_ADD;
      over.separateAlpha = over.alphaTest = false;
      iris.blend = blendState(over);
      iris.categories = categories | REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC;
      iris.persistent = !dynamicBuffers;
      iris.replacementHash = 0;
      iris.modelLayer = true; // its eye may be skinned on the CPU: world-space vertices, no transform
      g_instances.push_back(iris);
      ++g_stats.eyeIrises;
    }
  }
  if (inst.stencilRole == kStencilSourceOnly || inst.stencilRole == kStencilInside)
    return; // marks and overlays: no part in lighting estimates
  if (boneCount) // studio models: their ambient cube samples the surrounding (sky) light
    view.ambientUp.push_back({d.vsConstants[kRegAmbientUp * 4], d.vsConstants[kRegAmbientUp * 4 + 1],
                              d.vsConstants[kRegAmbientUp * 4 + 2]});
  ++g_stats.accepted;
}

void onSetLights(int count, const void *lights) {
  if (!g_ready || !lights || count <= 0 || count > 8)
    return;
  std::lock_guard<std::mutex> lock(g_lightLock);
  for (int i = 0; i < count; ++i) {
    SourceLight l = {};
    std::memcpy(&l, static_cast<const uint8_t *>(lights) + i * kLightDescSize, kLightDescPart);
    if (l.type < 1 || l.type > 3 || bsp::providesLight(l.type, l.position, l.direction))
      continue;
    // Identity: type, color, position, direction and falloff (not derived
    // values). Directional lights carry a per-model position; they are the
    // same light for every model, so position and falloff are ignored.
    SourceLight key = l;
    if (l.type == 2) {
      std::memset(key.position, 0, sizeof(key.position));
      key.range = key.falloff = key.attenuation0 = key.attenuation1 = key.attenuation2 = key.theta = key.phi = 0;
    }
    g_frameLights[fnv(&key, offsetof(SourceLight, thetaDot))] = l;
  }
}

// Dev aid (OPENREMIX_PROXY_GPU_STATS=1): GPU pass times averaged over every
// frame between statistics dumps, instead of one frame per dump.
struct GpuAverages {
  std::mutex lock;
  std::map<std::string, double> passes;
  double frame = 0;
  uint64_t count = 0, lastFrame = ~0ull;
} g_gpuAverages;
void sampleGpuPasses() {
  static const bool enabled = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_GPU_STATS", v, sizeof(v)) && v[0] == '1';
  }();
  if (!enabled)
    return;
  static auto get = reinterpret_cast<decltype(&remixapi_GetGpuPassTimingsEXT)>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetGpuPassTimingsEXT"));
  static remixapi_GpuPassTimingsEXT t;
  t = {};
  t.structSize = sizeof(t);
  if (!get || get(&t) != REMIXAPI_ERROR_CODE_SUCCESS || !t.available)
    return;
  std::lock_guard<std::mutex> guard(g_gpuAverages.lock);
  if (t.frame == g_gpuAverages.lastFrame)
    return; // no newer measured frame yet
  g_gpuAverages.lastFrame = t.frame;
  g_gpuAverages.frame += t.frameMilliseconds;
  ++g_gpuAverages.count;
  for (uint32_t i = 0; i < t.passCount && i < REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT; ++i)
    g_gpuAverages.passes[t.passes[i].name] += t.passes[i].milliseconds;
}

void shutdown() {
  if (!g_ready)
    return;
  g_ready = false;
  remix_ui::uninstall();
  g_api.Shutdown();
}

void applyTargetPixels();

// The runtime's slowest frame of the statistics interval (CPU side), with its
// breakdown: the averages hide a stage that is slow on some frames only.
struct SlowFrame {
  std::mutex lock;
  remixapi_FramePerformanceEXT performance = {};
  remixapi_FrameBreakdownEXT breakdown = {};
  uint32_t frames = 0, over8 = 0, over20 = 0;
  double recordSum = 0;
} g_slowFrame;
void sampleFramePerformance() {
  static auto getPerformance = reinterpret_cast<PFN_remixapi_GetFramePerformanceEXT>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetFramePerformanceEXT"));
  static auto getBreakdown = reinterpret_cast<PFN_remixapi_GetFrameBreakdownEXT>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetFrameBreakdownEXT"));
  remixapi_FramePerformanceEXT performance = {};
  performance.structSize = sizeof(performance);
  if (!getPerformance || getPerformance(&performance) != REMIXAPI_ERROR_CODE_SUCCESS)
    return;
  std::lock_guard<std::mutex> lock(g_slowFrame.lock);
  ++g_slowFrame.frames;
  g_slowFrame.recordSum += performance.recordMilliseconds;
  g_slowFrame.over8 += performance.recordMilliseconds > 8;
  g_slowFrame.over20 += performance.recordMilliseconds > 20;
  if (performance.recordMilliseconds > g_slowFrame.performance.recordMilliseconds) {
    g_slowFrame.performance = performance;
    g_slowFrame.breakdown = {};
    g_slowFrame.breakdown.structSize = sizeof(g_slowFrame.breakdown);
    if (getBreakdown)
      getBreakdown(&g_slowFrame.breakdown);
  }
}

bool frameInProgress(unsigned long &thread, double &milliseconds) {
  static const double msPerTick = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1e3 / double(f.QuadPart);
  }();
  const int64_t began = g_frameBegan.load(std::memory_order_relaxed);
  thread = g_frameThread.load(std::memory_order_relaxed);
  if (!began || !thread)
    return false;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  milliseconds = double(now.QuadPart - began) * msPerTick;
  return true;
}

void noteStatisticsDump(bool begin) { (begin ? g_statisticsBegan : g_statisticsEnded) = GetTickCount64(); }

// shaderapi_proxy_spikes.log: one line for every frame much longer than the
// ones around it, with where its time went. The frame is the span between two
// onPresent calls: the previous Present call (whose runtime timings these
// are), the engine's frame and this proxy's draw handling within it.
void logFrameSpike() {
  static auto getPerformance = reinterpret_cast<PFN_remixapi_GetFramePerformanceEXT>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetFramePerformanceEXT"));
  static auto getBreakdown = reinterpret_cast<PFN_remixapi_GetFrameBreakdownEXT>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetFrameBreakdownEXT"));
  static uint32_t last[5] = {}, frames = 0, logged = 0, sinceSpike = 0;
  static double average = 0;
  static const ULONGLONG started = GetTickCount64();
  double part[5];
  for (int i = 0; i < 5; ++i) {
    const uint32_t now = g_totalUs[i].load(std::memory_order_relaxed);
    part[i] = double(uint32_t(now - last[i])) / 1000;
    last[i] = now;
  }
  const double ms = g_frameIntervalMs;
  ++sinceSpike;
  if (++frames < 200) { // loading
    average = ms;
    return;
  }
  const bool spike = ms > average * 1.5 && ms > average + 3;
  if (spike && logged < 5000) {
    remixapi_FramePerformanceEXT p = {};
    p.structSize = sizeof(p);
    remixapi_FrameBreakdownEXT b = {};
    b.structSize = sizeof(b);
    if (getPerformance)
      getPerformance(&p);
    if (getBreakdown)
      getBreakdown(&b);
    const ULONGLONG tick = GetTickCount64(), began = g_statisticsBegan, ended = g_statisticsEnded;
    if (FILE *log = _wfopen((g_moduleDir + L"\\shaderapi_proxy_spikes.log").c_str(), logged ? L"a" : L"w")) {
      fprintf(log,
              "%8.2f s frame %u: %.2f ms (average %.2f, %u frames since the last) | engine %.2f, proxy draws %.2f "
              "(mesh build %.2f), present submit %.2f (BSP world %.2f, Present call %.2f) | statistics dump %s, began "
              "%.2f s ago | runtime: present %.2f = setup %.2f scene %.2f fence %.2f acquire %.2f record %.2f submit "
              "%.2f queuePresent %.2f release %.2f | geometry %.2f upload %.2f accel %.2f alloc %.2f, conversions %llu "
              "buffer allocations %llu memory allocations %llu uploaded %.2f MiB | record: raster %.2f skin %.2f begin "
              "%.2f setup %.2f surfaces %.2f material %.2f temporal %.2f retire %.2f traceAccel %.2f table %.2f "
              "lighting %.2f sharc %.2f signature %.2f descriptors %.2f primary %.2f passes %.2f post %.2f composite "
              "%.2f ui %.2f snapshot %.2f other %.2f\n",
              double(tick - started) / 1000, frames, ms, average, sinceSpike,
              ms - part[0] - part[2], part[0], part[1], part[2], part[3], part[4],
              began > ended ? "running" : "idle", double(tick - began) / 1000, p.presentMilliseconds,
              b.presentSetupMilliseconds, p.sceneMilliseconds, p.fenceMilliseconds, p.acquireMilliseconds,
              p.recordMilliseconds, p.submitMilliseconds, p.queuePresentMilliseconds, b.presentReleaseMilliseconds,
              p.geometryMilliseconds, p.uploadMilliseconds, p.accelerationMilliseconds, p.allocationMilliseconds,
              static_cast<unsigned long long>(p.geometryConversions),
              static_cast<unsigned long long>(p.bufferAllocations),
              static_cast<unsigned long long>(p.memoryAllocations), p.uploadBytes / 1048576.0,
              b.rasterPrepareMilliseconds, b.rasterSkinningMilliseconds, b.traceBeginMilliseconds,
              b.traceSetupMilliseconds, b.traceSurfacesMilliseconds, b.materialMilliseconds, b.temporalMilliseconds,
              b.retireMilliseconds, b.traceAccelerationMilliseconds, b.tableMilliseconds, b.lightingMilliseconds,
              b.sharcMilliseconds, b.signatureMilliseconds, b.descriptorMilliseconds, b.primaryMilliseconds,
              b.passesMilliseconds, b.postMilliseconds, b.compositeMilliseconds, b.uiMilliseconds,
              b.snapshotMilliseconds, b.otherMilliseconds);
      fclose(log);
      ++logged;
    }
    sinceSpike = 0;
  }
  // The usual frame: a spike moves it little.
  average = average * 0.97 + std::min(ms, average * 1.5) * 0.03;
}

// For the diagnostics log: what OpenRemix (SDL) takes the game window's state
// to be beside what Windows reports, and whether frames reach the screen. The
// runtime skips a frame without an error while it holds the window minimized,
// hidden or empty, so a disagreement here is a frozen picture. Written when
// either changes, and every few seconds while no frame is presented.
void logWindowState(uint32_t presents) {
  if (presents % 30 != 0 && presents > 3)
    return;
  static auto framePerformance = reinterpret_cast<PFN_remixapi_GetFramePerformanceEXT>(
      reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetFramePerformanceEXT")));
  remixapi_FramePerformanceEXT performance = {};
  performance.structSize = sizeof(performance);
  if (framePerformance)
    framePerformance(&performance);
  remixapi_WindowState state = {};
  const auto status = g_api.GetWindowState ? g_api.GetWindowState(&state) : REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  RECT client = {}, window = {};
  GetClientRect(g_gameWindow, &client);
  GetWindowRect(g_gameWindow, &window);
  const bool iconic = IsIconic(g_gameWindow), visible = IsWindowVisible(g_gameWindow);
  const bool foreground = GetForegroundWindow() == g_gameWindow;
  static uint64_t lastPresentations = 0;
  static uint32_t stalledSamples = 0;
  // Since the last sample, 30 of the game's frames ago.
  const bool stalled = presents > 3 && performance.presentations == lastPresentations;
  stalledSamples = stalled ? stalledSamples + 1 : 0;
  char text[512];
  std::snprintf(text, sizeof(text),
                "OpenRemix sees: minimized %d, drawable %ux%u, focused %d (status %d) | Windows says: minimized %d, "
                "visible %d, client %ldx%ld, window %ld,%ld %ldx%ld, style %08lx, ex %08lx, foreground %d",
                int(state.minimized), state.drawableWidth, state.drawableHeight, int(state.focused), int(status),
                int(iconic), int(visible), client.right, client.bottom, window.left, window.top,
                window.right - window.left, window.bottom - window.top,
                static_cast<unsigned long>(GetWindowLongPtrW(g_gameWindow, GWL_STYLE)),
                static_cast<unsigned long>(GetWindowLongPtrW(g_gameWindow, GWL_EXSTYLE)), int(foreground));
  static std::string last;
  static bool lastStalled = false;
  // Every ~5 s while stalled (six samples of 30 presents at the game's rate).
  if (text != last || stalled != lastStalled || (stalled && stalledSamples % 6 == 0)) {
    const bool disagree = status == REMIXAPI_ERROR_CODE_SUCCESS &&
                          (bool(state.minimized) != iconic ||
                           (!iconic && visible && client.right > 0 && client.bottom > 0 &&
                            (!state.drawableWidth || !state.drawableHeight)));
    diag::stage("frame %u: %s%s | OpenRemix frames attempted %llu, presented %llu%s", presents, text,
                disagree ? " | DISAGREE" : "", static_cast<unsigned long long>(performance.attempts),
                static_cast<unsigned long long>(performance.presentations),
                stalled ? " | NOT PRESENTING since the last sample" : "");
    last = text;
    lastStalled = stalled;
  }
  lastPresentations = performance.presentations;
}

// For the diagnostics log: what the runtime is drawing with and whether it is
// drawing. A picture that stays on the clear colour or does not change has a
// reason among these: the path tracer unavailable or not the active backend,
// its frames not advancing, or no scene handed over. Written when the state
// changes (the counters are compared between samples, not printed as state).
void logRendererState(uint32_t presents, size_t instances) {
  if (presents % 30 != 0 && presents > 3)
    return;
  auto stat = [](const char *name) { return reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"remix.dll"), name)); };
  static auto getBackend = reinterpret_cast<PFN_remixapi_GetBackendCapabilitiesEXT>(stat("remixapi_GetBackendCapabilitiesEXT"));
  static auto getTracing = reinterpret_cast<PFN_remixapi_GetTracingStatisticsEXT>(stat("remixapi_GetTracingStatisticsEXT"));
  static auto getDenoising = reinterpret_cast<PFN_remixapi_GetDenoisingStatisticsEXT>(stat("remixapi_GetDenoisingStatisticsEXT"));
  remixapi_BackendCapabilitiesEXT backend = {};
  backend.structSize = sizeof(backend);
  if (getBackend)
    getBackend(&backend);
  remixapi_TracingStatisticsEXT tracing = {};
  tracing.structSize = sizeof(tracing);
  if (getTracing)
    getTracing(&tracing);
  remixapi_DenoisingStatisticsEXT denoising = {};
  denoising.structSize = sizeof(denoising);
  if (getDenoising)
    getDenoising(&denoising);
  static uint64_t lastSequence = 0, lastGeneration = 0, lastRendered = 0;
  const bool tracerAdvancing = tracing.sampleSequence != lastSequence;
  const bool sceneChanging = tracing.sceneGeneration != lastGeneration;
  const bool denoiserAdvancing = denoising.renderedFrames != lastRendered;
  lastSequence = tracing.sampleSequence;
  lastGeneration = tracing.sceneGeneration;
  lastRendered = denoising.renderedFrames;
  char text[900];
  std::snprintf(text, sizeof(text),
                "backend active %u, requested %u, supported mask %u (1 raster, 2 path tracing, 4 Forward+)%s%s | tracer frames "
                "advancing %d, scene changing %d, accumulation %u | denoiser active %u of requested %u, frames "
                "advancing %d%s%s | map world loaded %d, instances this frame %s, world cameras %s",
                backend.activeBackend, backend.requestedBackend, backend.supportedBackends,
                backend.unavailableReason[0] ? ", unavailable: " : "", backend.unavailableReason,
                int(tracerAdvancing), int(sceneChanging), tracing.accumulationEnabled, denoising.activeDenoiser,
                denoising.requestedDenoiser, int(denoiserAdvancing), denoising.unavailableReason[0] ? ", unavailable: " : "",
                denoising.unavailableReason, int(bsp::active()), instances ? "some" : "NONE",
                g_stats.worldViewCameras.load() ? "some" : "none yet");
  static std::string last;
  if (text != last) {
    diag::stage("frame %u: renderer: %s (instances %zu, tracer sequence %llu)", presents, text, instances,
                static_cast<unsigned long long>(tracing.sampleSequence));
    last = text;
  }
  static auto getBake = reinterpret_cast<PFN_remixapi_GetBakeStatisticsEXT>(stat("remixapi_GetBakeStatisticsEXT"));
  remixapi_BakeStatisticsEXT bake = {};
  bake.structSize = sizeof(bake);
  if (getBake && getBake(&bake) == REMIXAPI_ERROR_CODE_SUCCESS) {
    std::snprintf(text, sizeof(text), "state %u, active %u, input %llu, progress %u/%u; %s", bake.state,
                  bake.active, static_cast<unsigned long long>(bake.requestedHash), bake.completed,
                  bake.probes, bake.status);
    static std::string lastBake;
    if (text != lastBake) {
      diag::stage("frame %u: bake: %s", presents, text);
      lastBake = text;
    }
  }
}

namespace {
void flushPendingTextures();
}

void onPresent() {
  {
    static auto backend = reinterpret_cast<PFN_remixapi_GetBackendCapabilitiesEXT>(
        GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetBackendCapabilitiesEXT"));
    static auto baking = reinterpret_cast<PFN_remixapi_GetBakeStatisticsEXT>(
        GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetBakeStatisticsEXT"));
    remixapi_BackendCapabilitiesEXT capabilities{};
    capabilities.structSize = sizeof(capabilities);
    remixapi_BakeStatisticsEXT bake{};
    bake.structSize = sizeof(bake);
    const bool known = backend && baking && backend(&capabilities) == REMIXAPI_ERROR_CODE_SUCCESS &&
                       baking(&bake) == REMIXAPI_ERROR_CODE_SUCCESS;
    engine_nocull::retainOffscreen(!known ||
        proxy::retainOffscreenGeometry(capabilities.activeBackend, capabilities.requestedBackend, bake));
  }
  sampleGpuPasses();
  if (!g_ready)
    return;
  flushPendingTextures();
  ++g_stats.frames;
  applySkyDome();
  {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (g_lastPresent.QuadPart) {
      g_usFrame += uint32_t(double(now.QuadPart - g_lastPresent.QuadPart) * 1e6 / double(freq.QuadPart));
      g_frameIntervalMs = double(now.QuadPart - g_lastPresent.QuadPart) * 1e3 / double(freq.QuadPart);
    }
    g_lastPresent = now;
    g_frameBegan.store(now.QuadPart, std::memory_order_relaxed);
    g_frameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
  }
  Timer timer(g_usSubmit, &g_totalUs[2]);
  // Classify this frame's perspective views. The main view is the one with the
  // most draws among those sharing the most common field of view. Views with
  // the main rotation are: the same camera with other depth planes (world),
  // the 3D skybox (same field of view, sky-space position) or the view model
  // (another field of view). Other views (monitors, reflections) are skipped.
  std::unordered_map<int, uint32_t> fovDraws;
  for (auto &[key, v] : g_views)
    if (v.perspective && !v.cameraSpace)
      fovDraws[int(std::lround(v.projection[5] * 1000))] += v.draws;
  int worldFov = 0;
  uint32_t best = 0;
  for (auto &[fov, n] : fovDraws)
    if (n > best) {
      best = n;
      worldFov = fov;
    }
  const View *main = nullptr;
  uint64_t mainKey = 0;
  for (auto &[key, v] : g_views) {
    if (v.perspective && !v.cameraSpace && int(std::lround(v.projection[5] * 1000)) == worldFov &&
        (!main || v.draws > main->draws)) {
      main = &v;
      mainKey = key;
    }
  }
  // Draw counts are a weak signal (with the engine's static world skipped the
  // view model can out-draw the world, and a view may have no draws at all).
  // The engine says which views have a world: the last one drawn is the main
  // view (the 3D skybox comes first), unless sky_camera says otherwise.
  if (!g_worldViews.empty()) {
    auto eyeOf = [](const float *v, float out[3]) {
      for (int i = 0; i < 3; ++i)
        out[i] = -(v[12] * v[i * 4] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
    };
    const WorldView *chosen = &g_worldViews.back();
    float origin[3], scale = 0;
    if (g_worldViews.size() > 1 && bsp::skyCamera(origin, scale) && scale > 0)
      for (const auto &candidate : g_worldViews)
        for (const auto &other : g_worldViews) {
          float eye[3], skyEye[3], d2 = 0;
          eyeOf(candidate.view, eye);
          eyeOf(other.view, skyEye);
          for (int i = 0; i < 3; ++i) {
            const float d = skyEye[i] - (origin[i] + eye[i] / scale);
            d2 += d * d;
          }
          if (&candidate != &other && d2 < 16.0f)
            chosen = &candidate;
        }
    worldFov = int(std::lround(chosen->projection[5] * 1000));
    main = nullptr;
    for (auto &[key, v] : g_views)
      if (v.perspective && int(std::lround(v.projection[5] * 1000)) == worldFov &&
          samePlacement(v.view, chosen->view) && (!main || v.draws > main->draws)) {
        main = &v;
        mainKey = key;
      }
    if (!main) {
      // No engine draw in the main view this frame (the BSP world shows it all).
      constexpr uint64_t kWorldViewKey = 0x776f726c64766965ull;
      View &v = g_views[kWorldViewKey];
      v.perspective = true;
      v.order = g_viewOrder++;
      std::memcpy(v.view, chosen->view, sizeof(v.view));
      std::memcpy(v.projection, chosen->projection, sizeof(v.projection));
      main = &v;
      mainKey = kWorldViewKey;
    }
    ++g_stats.worldViewCameras;
  }
  // Draw counts can favour the 3D skybox: looking down, the engine culls most
  // of the world while the skybox view draws its whole miniature city. The
  // skybox view looks from sky_camera's origin + eye / scale with the main
  // rotation, so a winner that is another candidate's skybox view yields to it.
  float skyOrigin[3], skyScale = 0;
  if (main && bsp::skyCamera(skyOrigin, skyScale) && skyScale > 0) {
    auto eyeOf = [](const View &v, float out[3]) {
      for (int i = 0; i < 3; ++i)
        out[i] = -(v.view[12] * v.view[i * 4] + v.view[13] * v.view[i * 4 + 1] + v.view[14] * v.view[i * 4 + 2]);
    };
    float mainEye[3];
    eyeOf(*main, mainEye);
    for (auto &[key, v] : g_views) {
      if (&v == main || !v.perspective || int(std::lround(v.projection[5] * 1000)) != worldFov ||
          !sameRotation(v.view, main->view))
        continue;
      float eye[3], distance2 = 0;
      eyeOf(v, eye);
      for (int i = 0; i < 3; ++i) {
        const float d = mainEye[i] - (skyOrigin[i] + eye[i] / skyScale);
        distance2 += d * d;
      }
      if (distance2 < 4.0f * 4.0f) {
        main = &v;
        mainKey = key;
        ++g_stats.skyViewDemoted;
        break;
      }
    }
  }
  if (main) {
    g_lastCamera = *main;
    // The 3D skybox keeps its own camera: the runtime draws SKY instances with
    // the sky camera behind the world (raster layer 0, path-traced
    // background), as Source does.
    enum class Role { Skip, Main, Sky, ViewModel, Screen };
    std::unordered_map<uint64_t, Role> roles;
    const View *viewModel = nullptr, *sky = nullptr;
    for (auto &[key, v] : g_views) {
      Role role = Role::Skip;
      if (v.screenTarget && key != mainKey && !samePlacement(v.view, main->view)) {
        // Its camera's own view (not the skybox view drawn into it first, nor
        // the main camera's scene drawn again into a texture by an effect).
        const auto it = g_screenViews.find(v.screenTarget);
        if (it != g_screenViews.end() && v.perspective &&
            std::equal(v.view, v.view + 16, it->second.view, [](float a, float b) { return std::fabs(a - b) < 1e-3f; }))
          role = Role::Screen;
      } else if (key == mainKey) {
        role = Role::Main;
      } else if (v.perspective && samePlacement(v.view, main->view) &&
                 int(std::lround(v.projection[5] * 1000)) == worldFov) {
        role = Role::Main; // drawn with the main camera
      } else if (v.perspective && sameRotation(v.view, main->view)) {
        const bool worldFovView = int(std::lround(v.projection[5] * 1000)) == worldFov;
        role = worldFovView ? Role::Sky : Role::ViewModel;
        const View *&chosen = worldFovView ? sky : viewModel;
        if (!chosen || v.draws > chosen->draws)
          chosen = &v;
      }
      roles[key] = role;
    }
    if (sky)
      ++g_stats.skyFrames;
    g_viewSummary.clear();
    for (auto &[key, v] : g_views) {
      char line[160];
      static const char *const names[] = {"skip", "main", "sky", "viewmodel", "screen"};
      snprintf(line, sizeof(line), "    view order %u draws %u persp %d P0 %.3f P5 %.3f P14 %.3f T %.0f %.0f %.0f -> %s\n",
               v.order, v.draws, v.perspective, v.projection[0], v.projection[5], v.projection[14], v.view[12],
               v.view[13], v.view[14], names[int(roles[key])]);
      g_viewSummary += line;
    }

    // Sky light: the path tracer's environment radiance follows the upward
    // face of the ambient cubes Source computes for lit models in the main
    // view (median per frame, smoothed over time).
    static float skyRadiance[3] = {};
    static bool skyKnown = false;
    static const float skyScale = envScale("OPENREMIX_PROXY_SKY_SCALE", 1.0f);
    if (!main->ambientUp.empty()) {
      auto samples = main->ambientUp;
      std::sort(samples.begin(), samples.end(),
                [](const auto &a, const auto &b) { return a[0] + a[1] + a[2] < b[0] + b[1] + b[2]; });
      const auto &median = samples[samples.size() / 2];
      for (int i = 0; i < 3; ++i) {
        const float target = std::isfinite(median[i]) ? std::max(median[i], 0.0f) * skyScale : 0;
        skyRadiance[i] = skyKnown ? skyRadiance[i] + (target - skyRadiance[i]) * 0.05f : target;
      }
      skyKnown = true;
    }
    g_lastSky[0] = skyRadiance[0];
    g_lastSky[1] = skyRadiance[1];
    g_lastSky[2] = skyRadiance[2];
    remixapi_CameraEnvironmentEXT environment = {};
    environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
    environment.enabled = skyKnown;
    environment.radiance = {skyRadiance[0], skyRadiance[1], skyRadiance[2]};

    // Under water (the map's water leaves at the eye): the runtime sees
    // through the water's medium (remixapi_CameraMediumEXT), as Source's
    // underwater fog shows it. OPENREMIX_PROXY_UNDERWATER=0: not sent.
    static const bool underwater = envScale("OPENREMIX_PROXY_UNDERWATER", 1) > 0;
    remixapi_CameraMediumEXT medium = {};
    medium.sType = REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_EXT;
    {
      float eye[3], color[3], distance = 0;
      for (int i = 0; i < 3; ++i)
        eye[i] = -(main->view[12] * main->view[i * 4] + main->view[13] * main->view[i * 4 + 1] +
                   main->view[14] * main->view[i * 4 + 2]);
      g_cameraInWater = underwater && bsp::waterAt(eye, color, distance);
      if (g_cameraInWater) {
        medium.inside = 1;
        medium.transmittanceColor = {color[0], color[1], color[2]};
        medium.transmittanceMeasurementDistance = distance;
        ++g_stats.underwaterFrames;
      }
    }
    environment.pNext = &medium;
    // The screen cameras the monitors in view show (noteScreenView).
    const std::vector<remixapi_ScreenCameraEXT> screens = screenCameras();
    remixapi_CameraScreensEXT screenList = {};
    screenList.sType = REMIXAPI_STRUCT_TYPE_CAMERA_SCREENS_EXT;
    screenList.count = uint32_t(screens.size());
    screenList.cameras = screens.data();
    if (!screens.empty())
      medium.pNext = &screenList;
    remixapi_CameraInfo camera = {};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    auto setup = [&](remixapi_CameraType type, const View &v) {
      camera.pNext = type == REMIXAPI_CAMERA_TYPE_WORLD ? &environment : nullptr;
      camera.type = type;
      std::memcpy(camera.view, v.view, sizeof(camera.view));
      std::memcpy(camera.projection, v.projection, sizeof(camera.projection));
      g_api.SetupCamera(&camera);
    };
    // The view model goes to the runtime as itself: VIEW_MODEL instances and
    // camera, which it path-traces in the world camera's view, in front of the
    // scene, without shadows on it or blocking the flashlight
    // (rtx.native.pathtrace.viewModel, on by default; off, the runtime
    // rasterizes it with its camera). OPENREMIX_PROXY_WORLD_VIEWMODEL=1 places
    // it in the world as world geometry instead, as before: scaled in view
    // space so the main camera projects it like Source's view-model camera, the
    // world camera's near plane moved in to keep it unclipped.
    static const bool worldViewModel = envScale("OPENREMIX_PROXY_WORLD_VIEWMODEL", 0) > 0;
    const bool rasterViewModel = !worldViewModel; // sent with its own camera
    View world = *main;
    float viewModelTransform[3][4] = {};
    if (viewModel && !rasterViewModel) {
      viewScaleTransform(main->view, viewModel->projection[0] / main->projection[0],
                         viewModel->projection[5] / main->projection[5], viewModelTransform);
      // Move only the near plane: p10 and p14 together define both planes
      // (LH, depth 0..1: n = -p14/p10, f = p14/(1-p10)); changing p14 alone
      // also pulled the far plane in (~35000 -> ~5000 units), clipping
      // distant geometry and desynchronizing NRD's depth reconstruction.
      constexpr float kNear = 1.0f;
      const float p10 = world.projection[10], p14 = world.projection[14];
      const float nearPlane = -p14 / p10;
      if (nearPlane > kNear && p10 > 1.0f) {
        const float farPlane = p14 / (1.0f - p10);
        world.projection[10] = farPlane / (farPlane - kNear);
        world.projection[14] = -kNear * world.projection[10];
      }
    }
    // A path-traced 3D skybox lies far beyond the map (scaled up around it):
    // the far plane (raster primary visibility, depth) must reach it.
    if (const float reach = bsp::skyTracedReach(); reach > 0) {
      const float p10 = world.projection[10], p14 = world.projection[14];
      if (p10 > 1.0f) {
        const float nearPlane = -p14 / p10, farPlane = p14 / (1.0f - p10), wanted = 2.0f * reach + 4096.0f;
        if (farPlane < wanted && nearPlane > 0) {
          world.projection[10] = wanted / (wanted - nearPlane);
          world.projection[14] = -nearPlane * world.projection[10];
        }
      }
    }
    setup(REMIXAPI_CAMERA_TYPE_WORLD, world);
    // Without engine draws in the 3D skybox view (its world comes from the
    // BSP), the sky camera is the main camera moved into the skybox:
    // eye_sky = sky origin + eye / scale, same rotation.
    View skyFromMain;
    float skyCameraOrigin[3], skyCameraScale;
    if (!sky && bsp::skyCamera(skyCameraOrigin, skyCameraScale)) {
      skyFromMain = *main;
      const float *v = main->view;
      float eye[3];
      for (int i = 0; i < 3; ++i)
        eye[i] = -(v[12] * v[i * 4] + v[13] * v[i * 4 + 1] + v[14] * v[i * 4 + 2]);
      for (int i = 0; i < 3; ++i)
        eye[i] = skyCameraOrigin[i] + eye[i] / skyCameraScale;
      for (int j = 0; j < 3; ++j)
        skyFromMain.view[12 + j] = -(eye[0] * v[j] + eye[1] * v[4 + j] + eye[2] * v[8 + j]);
      sky = &skyFromMain;
    }
    if (sky)
      setup(REMIXAPI_CAMERA_TYPE_SKY, *sky);
    if (viewModel && rasterViewModel)
      setup(REMIXAPI_CAMERA_TYPE_VIEW_MODEL, *viewModel);

    // Stable identities for models and moving props, so the runtime keeps
    // their previous transforms and bones (motion vectors, denoiser history).
    // Source does not say which entity a draw belongs to: every instance of a
    // cached mesh takes over the id of the nearest instance of that mesh in
    // the previous frames (root bone or transform translation, within 64
    // units); the rest get new ids. Instances not drawn are remembered for
    // kTrackedKeep frames: a prop that leaves the view (Source's culling) and
    // comes back keeps its id, and with it the runtime's light tree slots of
    // its emissive triangles (a new id rebuilt part of the tree, ~30 ms with
    // a pile of emissive props at the edge of the view, every frame).
    struct Tracked {
      uint64_t id;
      float position[3];
      bool taken;
      uint32_t age; // frames since it was drawn
    };
    constexpr uint32_t kTrackedKeep = 600;
    static std::unordered_map<uint64_t, std::vector<Tracked>> tracked;
    static uint64_t nextTrackedId = 1;
    std::unordered_map<uint64_t, std::vector<Tracked>> nextTracked;
    auto trackedId = [&](uint64_t group, const float position[3]) {
      constexpr float kMaxDistance2 = 64.0f * 64.0f;
      auto found = tracked.find(group);
      Tracked *best = nullptr;
      float bestDistance2 = kMaxDistance2;
      if (found != tracked.end())
        for (auto &candidate : found->second) {
          if (candidate.taken)
            continue;
          float d2 = 0;
          for (int i = 0; i < 3; ++i)
            d2 += (candidate.position[i] - position[i]) * (candidate.position[i] - position[i]);
          // Those drawn last frame first: a remembered one counts at twice its distance.
          if (candidate.age)
            d2 *= 4;
          if (d2 < bestDistance2) {
            bestDistance2 = d2;
            best = &candidate;
          }
        }
      uint64_t id;
      if (best) {
        best->taken = true;
        id = best->id;
      } else {
        id = 0x7000000000000000ull | nextTrackedId++; // static ids are 64-bit hashes: collisions are negligible
      }
      nextTracked[group].push_back({id, {position[0], position[1], position[2]}, false, 0});
      return id;
    };
    std::unordered_map<uint64_t, bool> seen; // multipass draws: the first pass wins
    // A path-traced 3D skybox comes from the BSP: the sky view's draws repeat it.
    const bool skyTraced = bsp::skyTracedReach() > 0;
    for (const auto &inst : g_instances)
      if (inst.stencilRole == kStencilInside && inst.depthTest && roles[inst.view] == Role::ViewModel &&
          g_stencilGroups[inst.stencilGroup].glassSources)
        g_stencilGroups[inst.stencilGroup].throughGlass = true;
    // Screen views add what the scene lacks: models and brushes not already
    // placed by the main view or another screen (placement: a flexed face's
    // mesh is rebuilt per draw, so its handle names nothing). Effects, world
    // decals and particles are the main view's alone; of blended draws only
    // the layers of models.
    auto placement = [](const Instance &inst) {
      const float(*m)[4] = inst.bones.empty() ? inst.transform : inst.bones[0].matrix;
      // A cached mesh is the same handle in every view; a rebuilt one is named
      // by its texture (two eyes may share theirs: their meshes differ).
      // The first index tells a model's parts apart where the place does not
      // (both eyes of a model skinned on the CPU: world-space, no transform).
      int64_t at[6] = {int64_t(inst.persistent ? reinterpret_cast<uintptr_t>(inst.mesh)
                                               : reinterpret_cast<uintptr_t>(inst.texture0)),
                       int64_t(inst.primitives), int64_t(inst.firstIndex)};
      for (int r = 0; r < 3; ++r)
        at[r + 3] = std::isfinite(m[r][3]) ? std::llround(m[r][3] * 4) : 0;
      return fnv(at, sizeof(at));
    };
    std::unordered_set<uint64_t> placed;
    for (const auto &inst : g_instances)
      if (roles[inst.view] == Role::Main && !inst.offTarget)
        placed.insert(placement(inst));
    for (auto &inst : g_instances) {
      const Role role = roles[inst.view];
      if (role == Role::Skip || (inst.offTarget && role != Role::Screen))
        continue;
      if (role == Role::Screen) {
        // Blended layers of a model (an eye's iris over its eyeball: skinned,
        // or rigid with a model transform) come too; world decals and other
        // blended draws in world space do not.
        static const float worldSpace[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        const bool modelLayer = inst.modelLayer || inst.skinned || !inst.bones.empty() ||
                                std::memcmp(inst.transform, worldSpace, sizeof(worldSpace)) != 0;
        const int reason = inst.rasterOnly ? 0 : inst.blend.alphaBlendEnabled && !modelLayer ? 1
                         : inst.stencilGroup >= 0 ? 2 : !placed.insert(placement(inst)).second ? 3 : -1;
        if (reason >= 0) {
          ++g_screenSkipped[reason];
          if (inst.texture0) {
            char hex[24];
            std::snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)textureHash(inst.texture0));
            g_screenSkippedExample[reason] = hex;
          }
          continue;
        }
        ++g_stats.screenInstances;
      }
      if ((role == Role::ViewModel && &g_views[inst.view] != viewModel) ||
          (role == Role::Sky && (&g_views[inst.view] != sky || skyTraced)))
        continue;
      if (role == Role::ViewModel && !rasterViewModel) {
        if (inst.bones.empty())
          premultiply(viewModelTransform, inst.transform);
        else
          for (auto &bone : inst.bones)
            premultiply(viewModelTransform, bone.matrix);
      }
      if (inst.stencilGroup >= 0 && inst.stencilRole != kStencilCut) {
        assignStencilMasks();
        const StencilGroup &group = g_stencilGroups[inst.stencilGroup];
        remixapi_InstanceInfoMaterialEXT maskMaterial = {};
        maskMaterial.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT;
        maskMaterial.material = inst.material;
        remixapi_InstanceInfoBoneTransformsEXT maskBones = {};
        maskBones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
        maskBones.boneTransforms_values = inst.bones.data();
        maskBones.boneTransforms_count = uint32_t(inst.bones.size());
        maskMaterial.pNext = inst.bones.empty() ? nullptr : &maskBones;
        void *const chain = inst.material ? static_cast<void *>(&maskMaterial)
                                          : (inst.bones.empty() ? nullptr : static_cast<void *>(&maskBones));
        remixapi_InstanceInfo info = {};
        info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        info.mesh = inst.mesh;
        std::memcpy(info.transform.matrix, inst.transform, sizeof(info.transform.matrix));
        info.doubleSided = 1;
        if (role == Role::ViewModel && rasterViewModel)
          info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
        if (group.halo && group.fill) {
          // The sources are halo.lua's re-draws of the entities.
          if (inst.stencilRole == kStencilSource) {
            remixapi_InstanceInfoHaloEXT halo = {};
            halo.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_HALO_EXT;
            halo.pNext = chain;
            halo.color = {group.color[0], group.color[1], group.color[2]};
            halo.blurX = halo.blurY = 2; // halo.Add's default
            halo.passes = std::max(group.passes, 1u);
            halo.additive = group.additive;
            halo.ignoreZ = group.ignoreZ;
            info.pNext = &halo;
            g_api.DrawInstance(&info);
            ++g_stats.haloSources;
          }
          continue;
        }
        if (group.mask) {
          // The blend state carries the alpha test (a silhouette's cutouts)
          // and, for overlays, how they blend.
          remixapi_InstanceInfoBlendEXT maskBlend = inst.blend;
          maskBlend.pNext = chain;
          remixapi_InstanceRasterStateEXT raster = {};
          raster.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT;
          raster.pNext = &maskBlend;
          raster.depthTest = inst.depthTest && !group.throughGlass;
          raster.depthCompare = 3; // LESS_OR_EQUAL
          remixapi_InstanceInfoMaskEXT mask = {};
          mask.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MASK_EXT;
          mask.group = group.mask;
          if (inst.stencilRole == kStencilInside) {
            mask.role = REMIXAPI_INSTANCE_MASK_INSIDE_EXT;
            mask.pNext = &raster;
            ++g_stats.maskOverlays;
          } else {
            mask.role = REMIXAPI_INSTANCE_MASK_SOURCE_EXT;
            mask.pNext = &maskBlend;
            mask.within = group.within >= 0 ? g_stencilGroups[group.within].mask : 0;
            mask.depthTest = inst.depthTest;
            ++g_stats.maskSources;
          }
          info.pNext = &mask;
          if (mask.role == REMIXAPI_INSTANCE_MASK_SOURCE_EXT && group.throughGlass && inst.depthWrite)
            ; // housing: hides the reticle, as its depth did
          else if (mask.role == REMIXAPI_INSTANCE_MASK_INSIDE_EXT || group.within < 0 || mask.within)
            g_api.DrawInstance(&info);
        }
        // A housing that marks (ALWAYS/REPLACE) is also what the game shows.
        if (inst.stencilRole != kStencilSource)
          continue;
      }
      struct {
        remixapi_MeshHandle mesh;
        float transform[12];
      } id = {inst.mesh, {}};
      std::memcpy(id.transform, inst.transform, sizeof(id.transform));
      uint64_t idHash = fnv(&id, sizeof(id));
      if (!inst.bones.empty())
        idHash = fnv(inst.bones.data(), inst.bones.size() * sizeof(remixapi_Transform), idHash);
      if (!seen.emplace(idHash, true).second) {
        ++g_stats.duplicate;
        continue;
      }
      if (role == Role::Sky && inst.texture0 && g_skyBrightness != 1.0f)
        inst.material = materialFor(inst.texture0, g_skyBrightness);
      remixapi_InstanceInfoMaterialEXT material = {};
      material.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT;
      material.material = inst.material;
      remixapi_InstanceInfoBoneTransformsEXT bones = {};
      bones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
      bones.boneTransforms_values = inst.bones.data();
      bones.boneTransforms_count = uint32_t(inst.bones.size());
      material.pNext = inst.bones.empty() ? nullptr : &bones;
      remixapi_InstanceInfoBlendEXT blend = inst.blend;
      blend.pNext = inst.material ? static_cast<void *>(&material)
                                  : (inst.bones.empty() ? nullptr : static_cast<void *>(&bones));
      remixapi_InstanceInfoReplacementHashEXT replacement = {};
      replacement.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_REPLACEMENT_HASH_EXT;
      replacement.pNext = &blend;
      replacement.meshHash = inst.replacementHash;
      // World brushes (static buffers, identity transform, opaque) are static
      // instances: the runtime keeps them in its static acceleration structure
      // (with rtx.native.pathtrace.combinedTlas, set at startup, one traversal
      // per ray) and they can receive its radiance cache. ~1.5 ms of opaque
      // tracing on c8m1. OPENREMIX_PROXY_STATIC_WORLD=0 submits them dynamic.
      static const bool staticWorld = [] {
        char v[8] = {};
        return !(GetEnvironmentVariableA("OPENREMIX_PROXY_STATIC_WORLD", v, sizeof(v)) && v[0] == '0');
      }();
      static const float identityTransform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
      remixapi_InstanceIdentityEXT identity = {};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.pNext = inst.replacementHash ? static_cast<void *>(&replacement) : static_cast<void *>(&blend);
      identity.instanceId = idHash ? idHash : 1;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      identity.rasterVisible = 1;
      const bool isStatic = staticWorld && role == Role::Main && inst.persistent && inst.bones.empty() &&
                            !inst.blend.alphaBlendEnabled && !inst.categories &&
                            std::memcmp(inst.transform, identityTransform, sizeof(identityTransform)) == 0;
      // Additive draws (destination factor ONE: glows, lens flares, sparks)
      // have no surface to trace; the path tracer would show them as opaque
      // sheets. They are rasterized over the traced image with their D3D
      // blend state instead.
      // Particles likewise, whatever their blend mode (see submitParticles).
      const bool isAdditive = (inst.blend.alphaBlendEnabled && inst.blend.dstColorBlendFactor == 1) || inst.rasterOnly;
      const bool isTracked = !isStatic && !isAdditive && inst.persistent && role != Role::Sky;
      if (isAdditive) {
        identity.instanceId = 0;
        identity.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
      }
      if (isTracked) {
        float position[3];
        for (int r = 0; r < 3; ++r)
          position[r] = inst.bones.empty() ? inst.transform[r][3] : inst.bones[0].matrix[r][3];
        const uint64_t group = fnv(&inst.mesh, sizeof(inst.mesh), uint64_t(role) + 1);
        identity.instanceId = trackedId(group, position);
        identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
      }
      remixapi_InstanceInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      info.pNext = (isStatic || isTracked || isAdditive) ? static_cast<void *>(&identity)
                   : inst.replacementHash ? static_cast<void *>(&replacement)
                                          : static_cast<void *>(&blend);
      info.mesh = inst.mesh;
      info.categoryFlags = inst.categories;
      // What only a screen camera sees (Breen in his studio, the player's own
      // body): reflection-only, so the main camera does not see it and the
      // screen's rays and bounces do (the runtime's camera rays of a screen
      // include reflection-only instances; its paint, an eye's iris, coats
      // only reflection-only receivers).
      if (role == Role::Screen)
        info.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY;
      if (role == Role::ViewModel && rasterViewModel)
        info.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
      else if (role == Role::Sky)
        info.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_SKY;
      std::memcpy(info.transform.matrix, inst.transform, sizeof(info.transform.matrix));
      info.doubleSided = !inst.singleSided;
      remixapi_InstanceInfoCameraCullingEXT cameraCulling = {};
      if (inst.cameraCullBack) {
        cameraCulling.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_CAMERA_CULLING_EXT;
        cameraCulling.pNext = info.pNext;
        cameraCulling.flags = REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT;
        info.pNext = &cameraCulling;
        ++g_stats.cameraCulledModels;
      }
      // Raster-only overlays keep the order the engine drew them in: it sorts
      // its translucent renderables back to front and draws a particle
      // system's layers in the order that composes them, which the depth of
      // their meshes' centres (the runtime's order otherwise) does not tell
      // for layers that share a place, like smoke and the fire inside it.
      // (A runtime without the extension ignores it.)
      remixapi_InstanceInfoRasterOrderEXT overlayOrder = {};
      if (isAdditive) {
        overlayOrder.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_RASTER_ORDER_EXT;
        overlayOrder.pNext = info.pNext;
        overlayOrder.order = uint32_t(&inst - g_instances.data()) + 1;
        info.pNext = &overlayOrder;
      }
      // A particle's depth state is its draw's ($ignorez glows), as is a
      // wireframe draw's (lines over the scene, or through it with $ignorez).
      remixapi_InstanceInfoWireframeEXT wireframe = {};
      if (inst.wireframe) {
        wireframe.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_WIREFRAME_EXT;
        wireframe.pNext = info.pNext;
        info.pNext = &wireframe;
      }
      remixapi_InstanceRasterStateEXT overlayState = {};
      if ((inst.particle || inst.wireframe) && inst.rasterOnly) {
        overlayState.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT;
        overlayState.pNext = info.pNext;
        overlayState.depthTest = inst.depthTest;
        overlayState.depthWrite = inst.depthWrite;
        overlayState.depthCompare = 3; // LESS_OR_EQUAL
        info.pNext = &overlayState;
      }
      remixapi_InstanceInfoSpriteEXT sprite = {};
      if (inst.rasterOnly && inst.spriteFlags) {
        sprite.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_SPRITE_EXT;
        sprite.pNext = info.pNext;
        sprite.flags = inst.spriteFlags;
        sprite.softDistance = inst.spriteSoft[0];
        sprite.softFadeStart = inst.spriteSoft[1];
        sprite.softFadeEnd = inst.spriteSoft[2];
        info.pNext = &sprite;
      }
      remixapi_InstanceInfoMaskEXT cut = {};
      if (inst.stencilRole == kStencilCut) {
        assignStencilMasks();
        if ((cut.group = g_stencilGroups[inst.stencilGroup].mask)) {
          cut.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MASK_EXT;
          cut.pNext = info.pNext;
          cut.role = REMIXAPI_INSTANCE_MASK_CUT_EXT;
          info.pNext = &cut;
          ++g_stats.maskCuts;
        }
      }
      g_api.DrawInstance(&info);
      ++g_stats.submitted;
    }
    for (auto &[group, list] : tracked)
      for (auto &entry : list)
        if (!entry.taken && entry.age + 1 < kTrackedKeep) {
          entry.age++;
          nextTracked[group].push_back(entry);
        }
    tracked = std::move(nextTracked);
    {
      Timer bspTimer(g_usBsp, &g_totalUs[3]);
      bsp::submit(materialForHash);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_lightLock);
    // The engine's own light state (engine_lights), as sampled for this frame.
    const auto engineLights = engine_lights::current();
    uint32_t styled = 0, styledOff = 0, duplicates = 0, projectedCopies = 0;
    g_lightDefinitions.clear();
    // Dlights the engine also handed to a model (SetLight): one light.
    // Likewise a projected texture (the flashlight) given to the models in
    // its beam: that copy has no identity (it is keyed by its contents), so
    // while the flashlight moved it was a new light every frame, on top of
    // the tracked one. Its origin is the projected texture's, up to a frame
    // of movement.
    if (engineLights && (!engineLights->dlights.empty() || !engineLights->projected.empty()))
      for (auto it = g_frameLights.begin(); it != g_frameLights.end();) {
        bool duplicate = false;
        const auto within = [&](const float *p, float distance) {
          return std::fabs(it->second.position[0] - p[0]) < distance &&
                 std::fabs(it->second.position[1] - p[1]) < distance &&
                 std::fabs(it->second.position[2] - p[2]) < distance;
        };
        if (it->second.type != 2) {
          for (const auto &d : engineLights->dlights)
            if (within(d.position, .1f)) {
              duplicate = true;
              break;
            }
          for (const auto &d : engineLights->projected)
            if (!duplicate && within(d.position, 64.0f)) {
              duplicate = true;
              ++projectedCopies;
            }
        }
        duplicates += duplicate;
        it = duplicate ? g_frameLights.erase(it) : std::next(it);
      }
    // What is left of SetLight this frame, by type (statistics).
    for (const auto &[hash, l] : g_frameLights)
      if (l.type >= 1 && l.type <= 3)
        ++g_stats.setLightKept[l.type];
    g_stats.projectedCopies += projectedCopies;
    auto addDefined = [](uint64_t hash, const SourceLight &l) {
      g_frameLights[hash] = l;
      g_lightDefinitions[hash] = fnv(&l, sizeof(l)) | 1; // never 0 (a light without a definition)
    };
    for (const auto &b : bsp::lights()) {
      SourceLight l = {};
      l.type = b.type;
      std::memcpy(l.color, b.color, 12);
      std::memcpy(l.position, b.position, 12);
      std::memcpy(l.direction, b.direction, 12);
      l.attenuation0 = b.attenuation[0];
      l.attenuation1 = b.attenuation[1];
      l.attenuation2 = b.attenuation[2];
      l.thetaDot = b.thetaDot;
      l.phiDot = b.phiDot;
      if (b.style) {
        // Switchable and flickering lights: scaled by their lightstyle (off at 0).
        const float value = engineLights && engineLights->styles && b.style > 0 && b.style < 128
                                ? engineLights->style[b.style]
                                : 1.0f;
        ++styled;
        // An off light stays in the set, black: the runtime keeps zero-power
        // lights as slots of its light tree (a refit). Leaving it out changed
        // the light count, which rebuilds the tree and resets the light
        // guiding history of the whole scene at every flicker.
        styledOff += !(value > 0);
        for (float &c : l.color)
          c *= value > 0 ? value : 0.0f;
        addDefined(b.hash, l);
        continue;
      }
      g_frameLights[b.hash] = l;
    }
    if (engineLights)
      for (const auto *list : {&engineLights->dlights, &engineLights->projected})
        for (const auto &e : *list) {
          SourceLight l = {};
          l.type = e.type;
          std::memcpy(l.color, e.color, 12);
          std::memcpy(l.position, e.position, 12);
          std::memcpy(l.direction, e.direction, 12);
          l.attenuation0 = e.attenuation[0];
          l.attenuation1 = e.attenuation[1];
          l.attenuation2 = e.attenuation[2];
          l.thetaDot = e.thetaDot;
          l.phiDot = e.phiDot;
          // The cookie once the runtime has its texture (until then a cone;
          // the definition changes when it arrives).
          if (!e.cookie.empty())
            if (const uint64_t cookie = maskTextureHash(e.cookie, false, uint32_t(e.cookieFrame))) {
              l.cookie = cookie;
              std::memcpy(l.cookieUp, e.up, 12);
              l.cookieFov = e.fov;
              ++g_stats.lightCookies;
              if (uint32_t(e.cookieFrame) > g_stats.lightCookieFrameMax)
                g_stats.lightCookieFrameMax = uint32_t(e.cookieFrame);
            }
          addDefined(e.hash, l);
        }
    engine_lights::report(styled, styledOff, duplicates);
    for (auto &[hash, l] : g_frameLights) {
      auto &cached = g_lights[hash];
      const auto defined = g_lightDefinitions.find(hash);
      const uint64_t definition = defined == g_lightDefinitions.end() ? 0 : defined->second;
      if (!cached.handle || cached.definition != definition) {
        // CreateLight under an existing hash replaces that light in place
        // (same handle).
        const remixapi_LightHandle handle = createLight(l, hash);
        if (!handle)
          continue;
        cached.handle = handle;
        cached.definition = definition;
      }
      cached.lastFrame = g_frame;
      g_api.DrawLightInstance(cached.handle);
      ++g_stats.lights;
    }
    if (g_skyDome)
      g_api.DrawLightInstance(g_skyDome); // the captured 2D skybox (setSkyFaces)
    // Styled lights capture a separate immutable fully-on definition. The retained
    // BSP snapshot supplies emissive materials independently of this list.
    std::vector<remixapi_LightHandle> bakeLights;
    std::vector<remixapi_BakeStyledLightEXT> styledLights;
    static const float bakeLightScale = envScale("OPENREMIX_PROXY_LIGHT_SCALE", 1.f);
    static const float bakeSunScale = envScale("OPENREMIX_PROXY_SUN_SCALE", 1.f);
    bool bakeLightsReady = true;
    for (const auto &light : bsp::lights()) {
      if (light.style) {
        SourceLight authored = {};
        authored.type = light.type;
        std::memcpy(authored.color, light.color, 12);
        std::memcpy(authored.position, light.position, 12);
        std::memcpy(authored.direction, light.direction, 12);
        authored.attenuation0 = light.attenuation[0];
        authored.attenuation1 = light.attenuation[1];
        authored.attenuation2 = light.attenuation[2];
        authored.thetaDot = light.thetaDot;
        authored.phiDot = light.phiDot;
        const float currentStyle = engineLights && engineLights->styles && light.style < 128
                                       ? engineLights->style[light.style] : 1.f;
        const auto captured = g_styledBakeLights.capture(g_api, authored, light.hash, light.style,
                                                        bakeLightScale, bakeSunScale, currentStyle);
        if (!captured.authoredLight) bakeLightsReady = false;
        else styledLights.push_back(captured);
        continue;
      }
      const auto found = g_lights.find(light.hash);
      if (found == g_lights.end() || !found->second.handle)
        bakeLightsReady = false;
      else
        bakeLights.push_back(found->second.handle);
    }
    if (g_bakeSky.light())
      bakeLights.push_back(g_bakeSky.light());
    if (bakeLightsReady)
      bsp::publishBakeScene(bakeLights, styledLights);
    g_lastLights.swap(g_frameLights);
    g_frameLights.clear();
    for (auto it = g_lights.begin(); it != g_lights.end();) {
      if (g_frame - it->second.lastFrame > 300) {
        if (it->second.handle)
          g_api.DestroyLight(it->second.handle);
        it = g_lights.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (g_uiCallback)
    g_uiCallback();
  remixapi_UIDrawList ui = {};
  ui.sType = REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST;
  ui.displayWidth = g_displayWidth;
  ui.displayHeight = g_displayHeight;
  ui.pVertices = g_uiVertices.data();
  ui.vertexCount = uint32_t(g_uiVertices.size());
  ui.pIndices = g_uiIndices.data();
  ui.indexCount = uint32_t(g_uiIndices.size());
  // Stencil-tested 2D draws: masked to their group, dropped if it is a halo
  // (its fill and composites are the runtime's now) or was never marked.
  assignStencilMasks();
  if (g_uiCommandMasks.size() == g_uiCommands.size()) {
    size_t kept = 0;
    for (size_t i = 0; i < g_uiCommands.size(); ++i) {
      remixapi_UIDrawCommand command = g_uiCommands[i];
      if (const auto [index, outside] = g_uiCommandMasks[i]; index >= 0) {
        const StencilGroup &group = g_stencilGroups[index];
        if ((group.halo && group.fill) || (!group.mask && !outside))
          continue;
        if (group.mask) {
          command.flags |= group.mask << REMIXAPI_UI_NATIVE_MASK_GROUP_SHIFT;
          if (outside)
            command.flags |= REMIXAPI_UI_NATIVE_MASK_OUTSIDE;
          ++g_stats.maskUi;
        }
      }
      g_uiCommands[kept++] = command;
    }
    g_uiCommands.resize(kept);
  }
  ui.pCommands = g_uiCommands.data();
  ui.commandCount = uint32_t(g_uiCommands.size());
  g_api.SubmitUIDrawList(&ui);
  g_uiVertices.clear();
  g_uiIndices.clear();
  g_uiCommands.clear();
  g_uiCommandMasks.clear();

  applyTargetPixels();
  sampleFramePerformance();
  logFrameSpike();
  serviceScreenshotRequest();
  serviceFlatDumpRequest();
  serviceMaterialProbeRequest();
  serviceShaderProbeRequest();
  // The first frames, for the diagnostics log: a crash while rendering shows
  // how far the game got.
  static uint32_t presents = 0;
  ++presents;
  // Multicore rendering (gmod_mcore_test 1 / mat_queue_mode 2): the material
  // system presents from its own thread once a map is loaded, and from the
  // starting one again in between. OpenRemix takes a Present into the game's
  // window from either. Said once each time the thread changes.
  {
    static DWORD startThread = GetCurrentThreadId(), lastThread = startThread;
    if (const DWORD thread = GetCurrentThreadId(); thread != lastThread) {
      lastThread = thread;
      if (thread != startThread)
        diag::stage("frame %u: presenting from another thread (%lu, OpenRemix was started on %lu): the engine is "
                    "rendering on its own thread (multicore rendering)",
                    presents, thread, startThread);
      else
        diag::stage("frame %u: presenting from the starting thread again", presents);
    }
  }
  const bool mark = presents <= 3 || presents == 10 || presents == 100 || presents == 1000;
  if (mark)
    diag::stage("frame %u: presenting", presents);
  // OPENREMIX_PROXY_CRASH_TEST=<n>: a deliberate fault at frame n, to check
  // that the diagnostics log records it.
  static const uint32_t crashAt = [] {
    char value[16] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_CRASH_TEST", value, sizeof(value)) ? uint32_t(atoi(value)) : 0u;
  }();
  if (crashAt && presents == crashAt) {
    diag::stage("OPENREMIX_PROXY_CRASH_TEST: faulting on purpose");
    *reinterpret_cast<volatile int *>(uintptr_t(0x10)) = 1;
  }
  sendSceneFog();
  hostpass::submit(g_dumpFlat);
  remixapi_ErrorCode presentResult;
  {
    Timer present(g_usPresent, &g_totalUs[4]);
    presentResult = g_api.Present(nullptr);
  }
  static auto previousPresentResult = REMIXAPI_ERROR_CODE_SUCCESS;
  if (presentResult != previousPresentResult) {
    diag::stage("OpenRemix Present returned %u (previous %u)", unsigned(presentResult), unsigned(previousPresentResult));
    previousPresentResult = presentResult;
  }
  frame_profile::sample(g_frameIntervalMs, presentResult, g_gameWindow, g_api.GetWindowState);
  if (mark)
    diag::stage("frame %u: presented", presents);
  logWindowState(presents);
  logRendererState(presents, g_instances.size());
  g_instances.clear();
  for (const auto &group : g_stencilGroups) {
    g_stats.haloGroups += group.halo && group.fill;
    g_stats.maskGroups += group.mask != 0;
  }
  g_stencilGroups.clear();
  g_stencilCurrent.clear();
  g_stencilMasksAssigned = false;
  g_views.clear();
  g_cameraSpace.valid = false;
  g_worldViews.clear();
  g_viewOrder = 0;

  for (auto mesh : g_transient)
    g_api.DestroyMesh(mesh);
  g_stats.transient += g_transient.size();
  g_transient.clear();
  evictEmissiveMaterials();
  // Static meshes keyed by buffer version: a rewritten buffer never matches
  // again, so old entries age out.
  ++g_frame;
  for (auto it = g_meshes.begin(); it != g_meshes.end();) {
    if (g_frame - it->second.lastFrame > 600) {
      g_api.DestroyMesh(it->second.handle);
      ++g_stats.evicted;
      it = g_meshes.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = g_flexMeshes.begin(); it != g_flexMeshes.end();) {
    if (g_frame - it->second.lastFrame > 600) {
      g_api.DestroyMesh(it->second.handle);
      it = g_flexMeshes.erase(it);
    } else {
      ++it;
    }
  }
}

// Caller holds g_textureLock.
void storeCpuCopy(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format, std::vector<uint8_t> pixels) {
  if (!keepCpuCopy(format, width, height))
    return;
  auto &cpu = g_cpuTextures[texture];
  if (!cpu || cpu->width != width || cpu->height != height || cpu->format != format)
    cpu = std::make_shared<CpuTexture>();
  cpu->width = width;
  cpu->height = height;
  cpu->format = format;
  cpu->data = std::move(pixels);
  cpu->touch(0, 0, width, height);
  ++cpu->version;
}

namespace {
// A level's layout: the runtime's format, the bytes of a row (of a block row
// for BC formats) and the rows. False for formats that are not forwarded.
bool textureLayout(D3DFORMAT format, uint32_t width, uint32_t height, remixapi_Format &remixFormat, uint32_t &rowBytes,
                   uint32_t &rows) {
  const uint32_t blocksW = std::max(1u, (width + 3) / 4), blocksH = std::max(1u, (height + 3) / 4);
  switch (format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
    remixFormat = REMIXAPI_FORMAT_B8G8R8A8_SRGB; // memory order B, G, R, A
    rowBytes = width * 4;
    rows = height;
    break;
  case D3DFMT_DXT1:
    remixFormat = static_cast<remixapi_Format>(134); // BC1_RGBA_SRGB_EXT
    rowBytes = blocksW * 8;
    rows = blocksH;
    break;
  case D3DFMT_DXT3: // transcoded to BC3 (formats::transcodeBC2toBC3)
  case D3DFMT_DXT5:
    remixFormat = REMIXAPI_FORMAT_BC3_SRGB;
    rowBytes = blocksW * 16;
    rows = blocksH;
    break;
  default:
    return false;
  }
  return true;
}

// A level's bytes, rows packed.
std::vector<uint8_t> packLevel(const uint8_t *data, uint32_t pitch, uint32_t rowBytes, uint32_t rows) {
  std::vector<uint8_t> pixels(size_t(rowBytes) * rows);
  for (uint32_t r = 0; r < rows; ++r)
    std::memcpy(pixels.data() + size_t(r) * rowBytes, data + size_t(r) * pitch, rowBytes);
  return pixels;
}

// A mipmapped texture is given to the runtime with the levels the game wrote,
// not level 0 alone with a chain for the runtime to build: building one means
// decoding the BC blocks and filtering every level on the game's thread
// (10-14 ms a texture, in the frame that loads it), and the result is kept
// uncompressed, six times the memory. The game writes a texture's levels one
// after another: they are held here until all are in, then created at once.
// Level 0 with levels still missing waits until the next Present, where it is
// created the old way (the runtime builds the chain).
struct PendingTexture {
  uint32_t width = 0, height = 0, levels = 0;
  D3DFORMAT format = D3DFMT_UNKNOWN;
  std::vector<std::vector<uint8_t>> level;
  uint64_t contentHash = 0, legacyHash = 0, opaqueHash = 0;
  bool levelZero = false;
  bool complete() const {
    for (const auto &bytes : level)
      if (bytes.empty())
        return false;
    return !level.empty();
  }
};
std::unordered_map<const void *, PendingTexture> g_pendingTextures; // g_textureLock
std::atomic<uint32_t> g_textureChains{0}, g_textureChainsBuilt{0}, g_textureLevelsSeen{0}, g_textureSingle{0};

PendingTexture &pendingTexture(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format,
                               uint32_t levels) {
  auto &p = g_pendingTextures[texture];
  if (p.width != width || p.height != height || p.format != format || p.levels != levels) {
    p = {};
    p.width = width;
    p.height = height;
    p.format = format;
    p.levels = levels;
    p.level.resize(std::max(levels, 1u));
  }
  return p;
}

// Creates the runtime texture from what is held: every level when all are
// in, else level 0 with a chain the runtime builds. g_textureLock held.
void createPendingTexture(const void *texture, PendingTexture &p) {
  remixapi_Format remixFormat;
  uint32_t rowBytes, rows;
  if (!p.levelZero || !textureLayout(p.format, p.width, p.height, remixFormat, rowBytes, rows))
    return;
  auto t = std::make_shared<RemixTexture>();
  t->hash = p.contentHash ? p.contentHash : 1;
  t->legacyHash = p.legacyHash;
  t->opaqueHash = p.opaqueHash;
  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.hash = t->hash;
  info.width = p.width;
  info.height = p.height;
  info.depth = 1;
  info.format = remixFormat;
  remixapi_TextureInfoGenerateMipsEXT mips = {};
  mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
  mips.generate = true;
  std::vector<uint8_t> chain;
  if (p.levels > 1 && p.complete()) {
    for (const auto &bytes : p.level)
      chain.insert(chain.end(), bytes.begin(), bytes.end());
    info.mipLevels = p.levels;
    info.data = chain.data();
    info.dataSize = chain.size();
    ++g_textureChains;
  } else {
    info.mipLevels = 1;
    info.data = p.level[0].data();
    info.dataSize = p.level[0].size();
    if (p.levels > 1) {
      info.pNext = &mips;
      ++g_textureChainsBuilt;
    } else
      ++g_textureSingle;
  }
  const bool created = g_api.CreateTexture(&info, &t->texture) == REMIXAPI_ERROR_CODE_SUCCESS;
  ++g_stats.textures;
  if (created) {
    g_texturesByHash[p.contentHash] = t;
    if (t->opaqueHash)
      if (auto &alias = g_texturesByHash[t->opaqueHash]; alias.expired())
        alias = t;
    g_textures[texture] = std::move(t);
  }
}

// Level 0 written with levels that never came: created before the frame is.
void flushPendingTextures() {
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  for (auto it = g_pendingTextures.begin(); it != g_pendingTextures.end();) {
    if (it->second.levelZero) {
      createPendingTexture(it->first, it->second);
      it = g_pendingTextures.erase(it);
    } else
      ++it;
  }
}
} // namespace

void onTextureUpload(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format, const uint8_t *data,
                     uint32_t pitch, uint32_t levels) {
  if (!g_ready || !data || !width || !height)
    return;
  remixapi_Format remixFormat;
  uint32_t rowBytes, rows;
  if (!textureLayout(format, width, height, remixFormat, rowBytes, rows)) {
    ++g_stats.textureSkipped;
    return;
  }
  std::vector<uint8_t> pixels = packLevel(data, pitch, rowBytes, rows);
  // Hashed as the game wrote it, before the X8 channel is made opaque.
  const uint64_t contentHash = dxvk::textureHash(pixels.data(), pixels.size());
  const uint64_t legacyHash = dxvk::legacyTextureHash(pixels.data(), pixels.size());
  noteReplacementKey(contentHash, 'T');
  if (format == D3DFMT_DXT3)
    formats::transcodeBC2toBC3(pixels.data(), pixels.size());
  // The X bytes are whatever the engine's conversion left there (unwritten
  // memory for some textures): a texture is also found by its hash with X
  // 0xff, which a file's pixels predict (bsp_world textureHashes).
  uint64_t opaqueHash = 0;
  if (format == D3DFMT_X8R8G8B8) {
    for (size_t i = 3; i < pixels.size(); i += 4)
      pixels[i] = 0xff;
    opaqueHash = dxvk::textureHash(pixels.data(), pixels.size());
    if (opaqueHash == contentHash)
      opaqueHash = 0;
  }

  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  for (uint64_t hash : {contentHash, opaqueHash}) {
    auto shared = hash ? g_texturesByHash.find(hash) : g_texturesByHash.end();
    if (shared == g_texturesByHash.end())
      continue;
    if (auto existing = shared->second.lock()) {
      g_textures[texture] = std::move(existing);
      g_pendingTextures.erase(texture);
      storeCpuCopy(texture, width, height, format, std::move(pixels));
      return;
    }
  }
  auto &p = pendingTexture(texture, width, height, format, levels);
  p.level[0] = pixels;
  p.contentHash = contentHash;
  p.legacyHash = legacyHash;
  p.opaqueHash = opaqueHash;
  p.levelZero = true;
  storeCpuCopy(texture, width, height, format, std::move(pixels));
  // Created now with one level or with every level in; else when the last
  // level is written (onTextureLevel) or at the next Present.
  if (levels <= 1 || p.complete()) {
    createPendingTexture(texture, p);
    g_pendingTextures.erase(texture);
  }
}

void onTextureLevel(const void *texture, uint32_t level, uint32_t width, uint32_t height, uint32_t levelWidth,
                    uint32_t levelHeight, D3DFORMAT format, const uint8_t *data, uint32_t pitch, uint32_t levels) {
  if (!g_ready || !data || !level || level >= levels || !levelWidth || !levelHeight)
    return;
  remixapi_Format remixFormat;
  uint32_t rowBytes, rows;
  if (!textureLayout(format, levelWidth, levelHeight, remixFormat, rowBytes, rows))
    return;
  // The runtime's levels halve and stop at one: a level of another size is
  // not one of its chain.
  if (levelWidth != std::max(1u, width >> level) || levelHeight != std::max(1u, height >> level))
    return;
  std::vector<uint8_t> pixels = packLevel(data, pitch, rowBytes, rows);
  ++g_textureLevelsSeen;
  if (format == D3DFMT_DXT3)
    formats::transcodeBC2toBC3(pixels.data(), pixels.size());
  if (format == D3DFMT_X8R8G8B8)
    for (size_t i = 3; i < pixels.size(); i += 4)
      pixels[i] = 0xff;
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  auto &p = pendingTexture(texture, width, height, format, levels);
  p.level[level] = std::move(pixels);
  if (p.levelZero && p.complete()) {
    createPendingTexture(texture, p);
    g_pendingTextures.erase(texture);
  }
}

void onTexturePartialUpload(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format, const RECT &rect,
                            const uint8_t *data, uint32_t pitch) {
  if (!g_ready || !data || !keepCpuCopy(format, width, height))
    return;
  uint32_t rowBytes, rows;
  if (!levelLayout(format, width, height, rowBytes, rows))
    return;
  const bool blocks = format == D3DFMT_DXT1 || format == D3DFMT_DXT5;
  const uint32_t unit = blocks ? 4 : 1; // pixels per row/column unit
  const uint32_t unitBytes = rowBytes / std::max(1u, blocks ? (width + 3) / 4 : width);
  const uint32_t x0 = uint32_t(rect.left) / unit, y0 = uint32_t(rect.top) / unit;
  const uint32_t x1 = (uint32_t(rect.right) + unit - 1) / unit, y1 = (uint32_t(rect.bottom) + unit - 1) / unit;
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  auto &cpu = g_cpuTextures[texture];
  if (!cpu || cpu->width != width || cpu->height != height || cpu->format != format) {
    cpu = std::make_shared<CpuTexture>();
    cpu->width = width;
    cpu->height = height;
    cpu->format = format;
    cpu->data.assign(size_t(rowBytes) * rows, 0);
  }
  // GMod rewrites a Chromium page's whole rectangle on every repaint: only
  // the bytes that differ from the copy mark it dirty (the region forwarded
  // to the runtime), unchanged rows are not even copied.
  uint32_t changedX0 = ~0u, changedX1 = 0, changedY0 = ~0u, changedY1 = 0;
  for (uint32_t y = y0; y < std::min(y1, rows); ++y) {
    const uint32_t bytes = (std::min(x1, rowBytes / unitBytes) - x0) * unitBytes;
    uint8_t *to = cpu->data.data() + size_t(y) * rowBytes + size_t(x0) * unitBytes;
    const uint8_t *from = data + size_t(y - y0) * pitch;
    if (std::memcmp(to, from, bytes) == 0)
      continue;
    uint32_t first = 0, last = bytes;
    while (first < bytes && to[first] == from[first])
      ++first;
    while (last > first && to[last - 1] == from[last - 1])
      --last;
    changedX0 = std::min(changedX0, x0 + first / unitBytes);
    changedX1 = std::max(changedX1, x0 + (last + unitBytes - 1) / unitBytes);
    changedY0 = std::min(changedY0, y);
    changedY1 = y + 1;
    std::memcpy(to, from, bytes);
  }
  if (changedX1 <= changedX0)
    return;
  cpu->touch(changedX0 * unit, changedY0 * unit, changedX1 * unit, changedY1 * unit);
  ++cpu->version;
}

// --- Render-target contents ---------------------------------------------------
// A render-target texture the scene samples (a screen the game redraws every
// frame) is one dynamic runtime texture under a stable identity: each image
// read back rewrites its pixels in place (remixapi_UpdateTextureEXT), so the
// texture, its lit material and its unlit / self-illuminated variants are
// made once. (Under a content hash, as ordinary uploads are, every image was
// a new texture with new materials: GPU objects made and released every
// frame.) Ordinary uploads keep their content hash: mods key replacements by it.
// OPENREMIX_PROXY_TARGET_UPDATE=0, or a runtime without the entry point: a new
// texture per image, as before.
struct TargetPixels {
  const void *texture;
  uint32_t width, height;
  D3DFORMAT format;
  std::vector<uint8_t> pixels; // B8G8R8A8 rows, alpha opaque for X8R8G8B8
};
std::vector<TargetPixels> g_targetPixels; // images whose record is made after Present
std::atomic<uint32_t> g_targetImages{0}, g_targetUnchanged{0}, g_targetUpdates{0}, g_targetCreated{0},
    g_targetRecreated{0}, g_targetFailed{0};
std::atomic<uint64_t> g_usTargetUpdate{0};

PFN_remixapi_UpdateTextureEXT targetUpdateEntry() {
  static const auto entry = [] {
    char v[8] = {};
    if (GetEnvironmentVariableA("OPENREMIX_PROXY_TARGET_UPDATE", v, sizeof(v)) && v[0] == '0')
      return PFN_remixapi_UpdateTextureEXT(nullptr);
    return reinterpret_cast<PFN_remixapi_UpdateTextureEXT>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_UpdateTextureEXT")));
  }();
  return entry;
}

void onTargetPixels(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format, const uint8_t *data,
                    uint32_t pitch) {
  if (!g_ready || !data || !width || !height)
    return;
  ++g_targetImages;
  TargetPixels t = {texture, width, height, format, {}};
  t.pixels.resize(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y)
    std::memcpy(t.pixels.data() + size_t(y) * width * 4, data + size_t(y) * pitch, width * 4);
  if (format == D3DFMT_X8R8G8B8)
    for (size_t i = 3; i < t.pixels.size(); i += 4)
      t.pixels[i] = 0xff;
  if (const auto update = targetUpdateEntry()) {
    std::lock_guard<std::recursive_mutex> lock(g_textureLock);
    const auto record = g_textures.find(texture);
    if (record != g_textures.end() && record->second->dynamic && record->second->width == width &&
        record->second->height == height) {
      // The record exists: its pixels change now, in place. Nothing is made
      // or destroyed, so this needs no frame boundary. An image equal to the
      // last one (the CPU copy) is not sent at all.
      const auto copy = g_cpuTextures.find(texture);
      if (copy != g_cpuTextures.end() && copy->second && copy->second->width == width &&
          copy->second->height == height && copy->second->format == format && copy->second->data == t.pixels) {
        ++g_targetUnchanged;
        return;
      }
      LARGE_INTEGER t0, t1, frequency;
      QueryPerformanceCounter(&t0);
      const bool updated = update(record->second->texture, 0, 0, width, height, REMIXAPI_FORMAT_B8G8R8A8_SRGB,
                                  t.pixels.data(), width * 4) == REMIXAPI_ERROR_CODE_SUCCESS;
      QueryPerformanceCounter(&t1);
      QueryPerformanceFrequency(&frequency);
      g_usTargetUpdate += uint64_t((t1.QuadPart - t0.QuadPart) * 1000000 / frequency.QuadPart);
      if (updated) {
        ++g_targetUpdates;
        storeCpuCopy(texture, width, height, format, std::move(t.pixels));
        return;
      }
      ++g_targetFailed; // the record is made again below
    }
  }
  for (auto &queued : g_targetPixels)
    if (queued.texture == texture) {
      queued = std::move(t);
      return;
    }
  g_targetPixels.push_back(std::move(t));
}

// A new dynamic record for a render-target texture's first image (or its
// first of a new size). Returns false when the runtime refused it.
bool createTargetTexture(TargetPixels &t) {
  static uint64_t serial = 0;
  auto record = std::make_shared<RemixTexture>();
  std::unique_lock<std::recursive_mutex> lock(g_textureLock);
  // An identity that no upload's content hash names.
  do {
    ++serial;
    record->hash = XXH3_64bits_withSeed(&serial, sizeof(serial), 0x7461726765743264ull);
  } while (!record->hash || g_texturesByHash.count(record->hash));
  remixapi_TextureInfoDynamicEXT dynamic = {};
  dynamic.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT;
  dynamic.dynamic = 1;
  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.pNext = &dynamic;
  info.hash = record->hash;
  info.width = t.width;
  info.height = t.height;
  info.depth = 1;
  info.mipLevels = 1;
  info.format = REMIXAPI_FORMAT_B8G8R8A8_SRGB;
  info.data = t.pixels.data();
  info.dataSize = t.pixels.size();
  if (g_api.CreateTexture(&info, &record->texture) != REMIXAPI_ERROR_CODE_SUCCESS) {
    record->texture = nullptr;
    return false;
  }
  record->dynamic = true;
  record->width = t.width;
  record->height = t.height;
  ++g_stats.textures;
  auto &slot = g_textures[t.texture];
  g_targetRecreated += slot && slot->dynamic;
  ++g_targetCreated;
  std::shared_ptr<RemixTexture> previous = std::move(slot);
  slot = std::move(record);
  storeCpuCopy(t.texture, t.width, t.height, t.format, std::move(t.pixels));
  lock.unlock();
  previous.reset(); // its materials and texture go: outside the lock
  return true;
}

// After Present: this frame's instances held the previous record's materials.
void applyTargetPixels() {
  for (auto &t : g_targetPixels) {
    if (targetUpdateEntry() && createTargetTexture(t))
      continue;
    uint64_t previous = 0, previousOpaque = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(g_textureLock);
      if (auto it = g_textures.find(t.texture); it != g_textures.end()) {
        previous = it->second->hash;
        previousOpaque = it->second->opaqueHash;
      }
    }
    onTextureUpload(t.texture, t.width, t.height, t.format, t.pixels.data(), t.width * 4, 1);
    // The image changes while the view moves: no trail of dead records.
    std::lock_guard<std::recursive_mutex> lock(g_textureLock);
    for (uint64_t hash : {previous, previousOpaque})
      if (auto it = g_texturesByHash.find(hash); it != g_texturesByHash.end() && it->second.expired())
        g_texturesByHash.erase(it);
  }
  g_targetPixels.clear();
}

void onMaterialBound(const engine_materials::Shading &shading, const char *material) {
  if (shading.base.empty())
    return;
  auto known = g_shadingByBase.find(shading.base);
  if (known == g_shadingByBase.end()) {
    if (shading.mode == engine_materials::Shading::Lit)
      return; // textures only lit materials use need no record
    if (g_shadingByBase.size() > 4096) {
      g_shadingByBase.clear();
      g_emissiveBaseNames.clear();
    }
    known = g_shadingByBase.emplace(shading.base, BoundShading{}).first;
  }
  known->second.shading = shading;
  known->second.material = material ? material : "";
  known->second.frame = g_frame;
  if (shading.mode != engine_materials::Shading::Lit)
    if (const uint64_t hash = maskTextureHash(shading.base, false))
      g_emissiveBaseNames[hash] = shading.base;
}

void onTextureCopy(const void *source, const void *destination) {
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  auto it = g_textures.find(source);
  if (it != g_textures.end())
    g_textures[destination] = it->second;
  auto cpu = g_cpuTextures.find(source);
  if (cpu != g_cpuTextures.end())
    g_cpuTextures[destination] = cpu->second;
}

void onTextureReleased(const void *texture) {
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  g_textures.erase(texture);
  g_pendingTextures.erase(texture);
  auto cpu = g_cpuTextures.find(texture);
  if (cpu != g_cpuTextures.end()) {
    // UI ids name the CPU copy, which UpdateTexture destinations may share.
    if (cpu->second.use_count() == 1 && cpu->second->registeredVersion && g_ready)
      g_api.FreeUITexture(reinterpret_cast<uintptr_t>(cpu->second.get()));
    g_cpuTextures.erase(cpu);
  }
}

void dumpStatistics(FILE *out) {
  const uint32_t frames = g_stats.frames.exchange(0);
  const double f = frames ? frames : 1;
  fprintf(out,
          "== Remix per frame (%u frames, %zu cached meshes): draws %.1f accepted %.1f submitted %.1f | skipped: "
          "offTarget %.1f noPos %.1f noData %.1f skinned %.1f badPrim %.1f badRange %.1f | hits %.1f created %.1f "
          "transient %.1f evicted %.1f | single-bone models drawn rigid %.1f\n",
          frames, g_meshes.size(), g_stats.draws.exchange(0) / f, g_stats.accepted.exchange(0) / f,
          g_stats.submitted.exchange(0) / f, g_stats.offTarget.exchange(0) / f, g_stats.noPosition.exchange(0) / f,
          g_stats.noData.exchange(0) / f, g_stats.skinned.exchange(0) / f, g_stats.badPrim.exchange(0) / f,
          g_stats.badRange.exchange(0) / f, g_stats.cacheHits.exchange(0) / f, g_stats.created.exchange(0) / f,
          g_stats.transient.exchange(0) / f, g_stats.evicted.exchange(0) / f, g_stats.rigidModels.exchange(0) / f);
  fprintf(out, "   world-list draws: dropped (BSP world) %.1f, kept blended %.1f, kept dynamic %.1f | $translucent "
               "world draws with an opaque texture, drawn opaque %.1f | screen camera draws %.1f (views %zu, "
               "refused for want of a slot %.1f; instances only screen views see %.1f)\n",
          g_stats.bspDropped.exchange(0) / f, g_stats.bspKeptBlended.exchange(0) / f,
          g_stats.bspKeptDynamic.exchange(0) / f, g_stats.opaqueTranslucent.exchange(0) / f,
          g_stats.screenDraws.exchange(0) / f, g_screenViews.size(), g_stats.screenViewsFull.exchange(0) / f,
          g_stats.screenInstances.exchange(0) / f);
  fprintf(out, "   last screen draw (%s): c48 %g %g %g %g c49 %g %g %g %g | u %g..%g v %g..%g -> u %g..%g v %g..%g (built "
               "meshes only)\n",
          g_screenMaterial.c_str(), g_screenUvRows[0], g_screenUvRows[1], g_screenUvRows[2], g_screenUvRows[3],
          g_screenUvRows[4], g_screenUvRows[5], g_screenUvRows[6], g_screenUvRows[7], g_screenUvRange[0],
          g_screenUvRange[1], g_screenUvRange[2], g_screenUvRange[3], g_screenUvRange[4], g_screenUvRange[5],
          g_screenUvRange[6], g_screenUvRange[7]);
  for (int k = 0; k < 4; ++k)
    g_screenUvRange[2 * k] = 1e30f, g_screenUvRange[2 * k + 1] = -1e30f;
  fprintf(out, "   screen-view instances left out (since the last dump): raster only %llu (%s), blended in world space "
               "%llu (%s), stencil %llu (%s), placed already %llu (%s)\n",
          (unsigned long long)g_screenSkipped[0], g_screenSkippedExample[0].c_str(),
          (unsigned long long)g_screenSkipped[1], g_screenSkippedExample[1].c_str(),
          (unsigned long long)g_screenSkipped[2], g_screenSkippedExample[2].c_str(),
          (unsigned long long)g_screenSkipped[3], g_screenSkippedExample[3].c_str());
  std::fill_n(g_screenSkipped, 4, 0ull);
  for (const auto &c : screenCameras())
    fprintf(out, "   screen camera %u: at %.0f %.0f %.0f forward %.2f %.2f %.2f up %.2f %.2f %.2f fovY %.1f aspect %.2f\n",
            c.slot, c.position.x, c.position.y, c.position.z, c.forward.x, c.forward.y, c.forward.z, c.up.x, c.up.y,
            c.up.z, c.fovYDegrees, c.aspect);
  fprintf(out, "   dropped for the BSP world: static prop draws %.1f, overlay draws %.1f, func_lod draws %.1f\n",
          g_stats.bspPropsDropped.exchange(0) / f, g_stats.bspOverlaysDropped.exchange(0) / f,
          g_stats.bspBrushEntitiesDropped.exchange(0) / f);
  vb_origins::write(out, f);
  fprintf(out, "   Lua meshes lit as surfaces per frame: %.1f (OPENREMIX_PROXY_LUA_MESHES_LIT)\n",
          g_stats.luaLitDraws.exchange(0) / f);
  bsp::dumpStatistics(out);
  remix_ui::dumpStatistics(out);
  {
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    for (const auto &[hash, n] : g_keptWorldTextures)
      fprintf(out, "     kept dynamic world draws: texture %016llX (%s) %.1f/frame\n", (unsigned long long)hash,
              bsp::materialName(hash).c_str(), n / f);
    g_keptWorldTextures.clear();
  }
  fprintf(out, "   ms/frame: frame %.2f | proxy draws %.2f (mesh build %.2f) present submit %.2f (BSP world %.2f, Present call %.2f)\n",
          g_usFrame.exchange(0) / f / 1000, g_usDraw.exchange(0) / f / 1000, g_usBuild.exchange(0) / f / 1000,
          g_usSubmit.exchange(0) / f / 1000, g_usBsp.exchange(0) / f / 1000, g_usPresent.exchange(0) / f / 1000);
  if (g_ready) {
    static auto framePerformance = reinterpret_cast<PFN_remixapi_GetFramePerformanceEXT>(
        GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetFramePerformanceEXT"));
    remixapi_FramePerformanceEXT perf = {};
    perf.structSize = sizeof(perf);
    if (framePerformance && framePerformance(&perf) == REMIXAPI_ERROR_CODE_SUCCESS)
      fprintf(out,
              "   runtime (last frame): present %.2f scene %.2f record %.2f geometry %.2f upload %.2f fence %.2f "
              "acquire %.2f submit %.2f queuePresent %.2f ms; conversions %llu\n",
              perf.presentMilliseconds, perf.sceneMilliseconds, perf.recordMilliseconds, perf.geometryMilliseconds,
              perf.uploadMilliseconds, perf.fenceMilliseconds, perf.acquireMilliseconds, perf.submitMilliseconds,
              perf.queuePresentMilliseconds, static_cast<unsigned long long>(perf.geometryConversions));
    {
      std::lock_guard<std::mutex> lock(g_slowFrame.lock);
      const auto &p = g_slowFrame.performance;
      const auto &b = g_slowFrame.breakdown;
      const double n = g_slowFrame.frames ? g_slowFrame.frames : 1;
      fprintf(out,
              "   runtime record stage over %u frames: mean %.2f ms, over 8 ms on %u, over 20 ms on %u | slowest: record "
              "%.2f (scene %.2f fence %.2f geometry %.2f upload %.2f acceleration %.2f allocation %.2f; %llu "
              "conversions, %llu buffer allocations, %.2f MiB uploaded)\n",
              g_slowFrame.frames, g_slowFrame.recordSum / n, g_slowFrame.over8, g_slowFrame.over20,
              p.recordMilliseconds, p.sceneMilliseconds, p.fenceMilliseconds, p.geometryMilliseconds,
              p.uploadMilliseconds, p.accelerationMilliseconds, p.allocationMilliseconds,
              static_cast<unsigned long long>(p.geometryConversions),
              static_cast<unsigned long long>(p.bufferAllocations), p.uploadBytes / 1048576.0);
      fprintf(out,
              "     its record breakdown: raster prepare %.2f (skinning %.2f) trace begin %.2f setup %.2f surfaces %.2f "
              "(material %.2f temporal %.2f) retire %.2f acceleration %.2f table %.2f lighting %.2f sharc %.2f "
              "signature %.2f descriptors %.2f primary %.2f passes %.2f post %.2f composite %.2f ui %.2f snapshot %.2f "
              "other %.2f | present setup %.2f release %.2f\n",
              b.rasterPrepareMilliseconds, b.rasterSkinningMilliseconds, b.traceBeginMilliseconds,
              b.traceSetupMilliseconds, b.traceSurfacesMilliseconds, b.materialMilliseconds, b.temporalMilliseconds,
              b.retireMilliseconds, b.traceAccelerationMilliseconds, b.tableMilliseconds, b.lightingMilliseconds,
              b.sharcMilliseconds, b.signatureMilliseconds, b.descriptorMilliseconds, b.primaryMilliseconds,
              b.passesMilliseconds, b.postMilliseconds, b.compositeMilliseconds, b.uiMilliseconds,
              b.snapshotMilliseconds, b.otherMilliseconds, b.presentSetupMilliseconds, b.presentReleaseMilliseconds);
      // The statistics file holds one interval (and none of this once the
      // game has closed): every interval also goes to shaderapi_proxy_frames.log.
      static bool first = true;
      if (FILE *log = _wfopen((g_moduleDir + L"\\shaderapi_proxy_frames.log").c_str(), first ? L"w" : L"a")) {
        first = false;
        SYSTEMTIME now;
        GetLocalTime(&now);
        fprintf(log,
                "%02u:%02u:%02u frames %u record mean %.2f over8 %u over20 %u | slowest record %.2f scene %.2f fence %.2f "
                "geometry %.2f upload %.2f accel %.2f alloc %.2f conversions %llu allocations %llu uploaded %.2f MiB | "
                "raster %.2f skin %.2f begin %.2f setup %.2f surfaces %.2f material %.2f temporal %.2f retire %.2f "
                "traceAccel %.2f table %.2f lighting %.2f sharc %.2f signature %.2f descriptors %.2f primary %.2f "
                "passes %.2f post %.2f composite %.2f ui %.2f snapshot %.2f other %.2f presentSetup %.2f release %.2f | "
                "meshes cached %zu lights cached %zu\n",
                now.wHour, now.wMinute, now.wSecond, g_slowFrame.frames, g_slowFrame.recordSum / n, g_slowFrame.over8,
                g_slowFrame.over20, p.recordMilliseconds, p.sceneMilliseconds, p.fenceMilliseconds,
                p.geometryMilliseconds, p.uploadMilliseconds, p.accelerationMilliseconds, p.allocationMilliseconds,
                static_cast<unsigned long long>(p.geometryConversions),
                static_cast<unsigned long long>(p.bufferAllocations), p.uploadBytes / 1048576.0,
                b.rasterPrepareMilliseconds, b.rasterSkinningMilliseconds, b.traceBeginMilliseconds,
                b.traceSetupMilliseconds, b.traceSurfacesMilliseconds, b.materialMilliseconds, b.temporalMilliseconds,
                b.retireMilliseconds, b.traceAccelerationMilliseconds, b.tableMilliseconds, b.lightingMilliseconds,
                b.sharcMilliseconds, b.signatureMilliseconds, b.descriptorMilliseconds, b.primaryMilliseconds,
                b.passesMilliseconds, b.postMilliseconds, b.compositeMilliseconds, b.uiMilliseconds,
                b.snapshotMilliseconds, b.otherMilliseconds, b.presentSetupMilliseconds, b.presentReleaseMilliseconds,
                g_meshes.size(), g_lights.size());
        fclose(log);
      }
      g_slowFrame.performance = {};
      g_slowFrame.breakdown = {};
      g_slowFrame.frames = g_slowFrame.over8 = g_slowFrame.over20 = 0;
      g_slowFrame.recordSum = 0;
    }
    // GPU side: per-pass times, tracing and local-light counts.
    auto stat = [](const char *name) { return GetProcAddress(GetModuleHandleW(L"remix.dll"), name); };
    static auto passTimings = reinterpret_cast<decltype(&remixapi_GetGpuPassTimingsEXT)>(stat("remixapi_GetGpuPassTimingsEXT"));
    static auto tracing = reinterpret_cast<decltype(&remixapi_GetTracingStatisticsEXT)>(stat("remixapi_GetTracingStatisticsEXT"));
    static auto localLights = reinterpret_cast<decltype(&remixapi_GetLocalLightStatisticsEXT)>(stat("remixapi_GetLocalLightStatisticsEXT"));
    static auto breakdown = reinterpret_cast<decltype(&remixapi_GetFrameBreakdownEXT)>(stat("remixapi_GetFrameBreakdownEXT"));
    static remixapi_GpuPassTimingsEXT passes;
    passes = {};
    passes.structSize = sizeof(passes);
    if (passTimings && passTimings(&passes) == REMIXAPI_ERROR_CODE_SUCCESS && passes.available) {
      fprintf(out, "   gpu: frame %.2f ms;", passes.frameMilliseconds);
      for (uint32_t i = 0; i < passes.passCount && i < REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT; ++i)
        if (passes.passes[i].milliseconds >= 0.5)
          fprintf(out, " %s %.2f", passes.passes[i].name, passes.passes[i].milliseconds);
      fprintf(out, "\n");
    }
    {
      std::lock_guard<std::mutex> guard(g_gpuAverages.lock);
      if (g_gpuAverages.count) {
        const double n = double(g_gpuAverages.count);
        fprintf(out, "   gpu mean (%llu frames): frame %.2f ms;", static_cast<unsigned long long>(g_gpuAverages.count),
                g_gpuAverages.frame / n);
        for (const auto &[name, ms] : g_gpuAverages.passes)
          if (ms / n >= 0.3)
            fprintf(out, " %s %.2f;", name.c_str(), ms / n);
        fprintf(out, "\n");
        g_gpuAverages.passes.clear();
        g_gpuAverages.frame = 0;
        g_gpuAverages.count = 0;
      }
    }
    remixapi_TracingStatisticsEXT trace = {};
    trace.structSize = sizeof(trace);
    remixapi_LocalLightStatisticsEXT lights = {};
    lights.structSize = sizeof(lights);
    remixapi_FrameBreakdownEXT frame = {};
    frame.structSize = sizeof(frame);
    if (tracing && tracing(&trace) == REMIXAPI_ERROR_CODE_SUCCESS && localLights &&
        localLights(&lights) == REMIXAPI_ERROR_CODE_SUCCESS && breakdown && breakdown(&frame) == REMIXAPI_ERROR_CODE_SUCCESS)
      fprintf(out,
              "   trace: instances %llu surfaces %llu | blas builds %llu refits %llu | accel %.0f MiB | local lights %u "
              "(active %u)\n",
              static_cast<unsigned long long>(frame.traceInstances), static_cast<unsigned long long>(frame.traceSurfaces),
              static_cast<unsigned long long>(trace.blasBuilds), static_cast<unsigned long long>(trace.blasRefits),
              trace.accelerationBytes / 1048576.0, lights.lightCount, lights.activeLights);
    if (localLights)
      fprintf(out, "   local light tree: generation %llu, builds %llu, refits %llu, denoiser %u | PVS culling: %u "
                   "clustered, %u unrestricted\n",
              static_cast<unsigned long long>(lights.generation), static_cast<unsigned long long>(lights.builds),
              static_cast<unsigned long long>(lights.refits), lights.denoiserActive, lights.reserved & 0xffff,
              lights.reserved >> 16);
    static auto guiding = reinterpret_cast<decltype(&remixapi_GetLocalLightGuidingStatisticsEXT)>(
        stat("remixapi_GetLocalLightGuidingStatisticsEXT"));
    remixapi_LocalLightGuidingStatisticsEXT gs = {};
    gs.structSize = sizeof(gs);
    if (guiding && guiding(&gs) == REMIXAPI_ERROR_CODE_SUCCESS)
      fprintf(out,
              "   local light samples (1/64 sampled): %u, guided %u, visible %u (%.0f%%) | estimator %s%s | guiding "
              "active %u, history resets %llu / %llu frames %s\n",
              gs.sampledQueries, gs.sampledGuided, gs.sampledVisible,
              gs.sampledQueries ? 100.0 * gs.sampledVisible / gs.sampledQueries : 0.0, gs.reserved & 1 ? "RIS" : "BVH",
              gs.reserved & 2 ? " (sphere-only set)" : "", gs.active, (unsigned long long)gs.historyResets,
              (unsigned long long)gs.renderedFrames, gs.unavailableReason);
    static auto denoising = reinterpret_cast<decltype(&remixapi_GetDenoisingStatisticsEXT)>(stat("remixapi_GetDenoisingStatisticsEXT"));
    remixapi_DenoisingStatisticsEXT dn = {};
    dn.structSize = sizeof(dn);
    if (denoising && denoising(&dn) == REMIXAPI_ERROR_CODE_SUCCESS)
      fprintf(out, "   denoiser: active %u taa %u | history resets %llu, rendered frames %llu, history frames %llu\n",
              dn.activeDenoiser, dn.activeTaa, static_cast<unsigned long long>(dn.historyResets),
              static_cast<unsigned long long>(dn.renderedFrames), static_cast<unsigned long long>(dn.historyFrames));
    static auto sharc = reinterpret_cast<decltype(&remixapi_GetSharcStatisticsEXT)>(stat("remixapi_GetSharcStatisticsEXT"));
    remixapi_SharcStatisticsEXT cache = {};
    cache.structSize = sizeof(cache);
    if (sharc && sharc(&cache) == REMIXAPI_ERROR_CODE_SUCCESS && cache.active)
      fprintf(out, "   sharc: entries %u/%u, update paths %u, queries %u hits %u (%.0f%%), resets %llu of %llu frames\n",
              cache.occupiedEntries, cache.capacity, cache.updatePaths, cache.queryAttempts, cache.queryHits,
              cache.queryAttempts ? 100.0 * cache.queryHits / cache.queryAttempts : 0.0,
              static_cast<unsigned long long>(cache.resets), static_cast<unsigned long long>(cache.renderedFrames));
  }
  fprintf(out, "   transient: %.0f vertices %.0f indices per frame; by declaration:",
          g_transientVerts.exchange(0) / f, g_transientIndices.exchange(0) / f);
  {
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    for (auto &[decl, n] : g_transientDecls)
      fprintf(out, " %p:%.1f", decl, n / f);
    g_transientDecls.clear();
  }
  fprintf(out, "\n");
  fprintf(out, "   lights per frame %.1f (cached %zu); sky radiance %.3f %.3f %.3f\n", g_stats.lights.exchange(0) / f,
          g_lights.size(), g_lastSky[0], g_lastSky[1], g_lastSky[2]);
  fprintf(out, "   engine per-model lights kept per frame: point %.2f, directional %.2f, spot %.2f; copies of projected "
               "textures dropped %.2f; projected textures with their cookie %.2f (largest cookie frame %u)\n",
          g_stats.setLightKept[1].exchange(0) / f, g_stats.setLightKept[2].exchange(0) / f,
          g_stats.setLightKept[3].exchange(0) / f, g_stats.projectedCopies.exchange(0) / f,
          g_stats.lightCookies.exchange(0) / f, g_stats.lightCookieFrameMax.exchange(0));
  fprintf(out, "   shattered glass per frame: solid blocks drawn as the glass %.1f, edge pieces composed %.1f, "
               "tinted %.1f; composed textures made %u\n",
          g_stats.shatteredSolid.exchange(0) / f, g_stats.shatteredEdges.exchange(0) / f,
          g_stats.shatteredTinted.exchange(0) / f, g_stats.shatteredTextures.exchange(0));
  {
    std::lock_guard<std::mutex> lock(g_lightLock);
    for (auto &[h, l] : g_lastLights)
      fprintf(out, "    light type %d color %.3f %.3f %.3f pos %.0f %.0f %.0f dir %.2f %.2f %.2f att %.4g %.4g %.4g\n",
              l.type, l.color[0], l.color[1], l.color[2], l.position[0], l.position[1], l.position[2], l.direction[0],
              l.direction[1], l.direction[2], l.attenuation0, l.attenuation1, l.attenuation2);
  }
  {
    char diagnostic[512] = {};
    auto modsDiagnostic = reinterpret_cast<decltype(&remixapi_GetUSDModsDiagnosticEXT)>(
        GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetUSDModsDiagnosticEXT"));
    if (g_ready && modsDiagnostic)
      modsDiagnostic(diagnostic, sizeof(diagnostic));
    fprintf(out, "   USD mods: configure %d; %s\n", int(g_modsResult), diagnostic);
  }
  fprintf(out, "   frames with a sky view %.2f (skybox view had the most draws %.2f); main camera from the engine's world view %.2f\n%s",
          g_stats.skyFrames.exchange(0) / f, g_stats.skyViewDemoted.exchange(0) / f,
          g_stats.worldViewCameras.exchange(0) / f, g_viewSummary.c_str());
  fprintf(out, "   player body (l4d2-rtx shadow mesh) %.1f\n", g_stats.playerBody.exchange(0) / f);
  fprintf(out, "   rtx.conf texture categories: %zu hashes; ignored/hidden draws %.1f\n", categories::size(),
          g_stats.ignored.exchange(0) / f);
  fprintf(out, "   depthOnly %.1f additive %.1f other blended %.1f duplicate %.1f; engine flashlight passes dropped %.1f (%.1f by their lit material); black additive dropped %.1f\n",
          g_stats.depthOnly.exchange(0) / f, g_stats.additive.exchange(0) / f, g_stats.blended.exchange(0) / f,
          g_stats.duplicate.exchange(0) / f, g_stats.flashlightPasses.exchange(0) / f,
          g_stats.flashlightLitPasses.exchange(0) / f, g_stats.additiveBlack.exchange(0) / f);
  {
    // Particles (submitParticles): per frame in all, then per combination of
    // material shader, vertex shader and declaration, the most drawn first.
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    uint32_t draws = 0, expandedDraws = 0, vertices = 0, triangles = 0, premultiplied = 0, overbright = 0;
    uint32_t soft = 0, softConfirmed = 0, frameBlend = 0, frameMax = 0, greenAlpha = 0, sequence = 0, modulate = 0;
    uint32_t outcomes[kParticleOutcomes] = {};
    std::vector<const ParticleRow *> rows;
    for (const auto &[key, row] : g_particleRows) {
      draws += row.draws;
      expandedDraws += row.kind == kParticleExpanded ? row.draws : 0;
      vertices += row.vertices;
      triangles += row.triangles;
      premultiplied += row.premultiplied;
      overbright += row.overbright;
      soft += row.soft, softConfirmed += row.softConfirmed, frameBlend += row.frameBlend, frameMax += row.frameMax;
      greenAlpha += row.greenAlpha, sequence += row.sequence, modulate += row.modulate;
      for (int i = 0; i < kParticleOutcomes; ++i)
        outcomes[i] += row.outcomes[i];
      rows.push_back(&row);
    }
    std::sort(rows.begin(), rows.end(), [](const ParticleRow *a, const ParticleRow *b) {
      // What was not understood first, then by draws.
      const bool af = a->outcomes[kParticleFailed] != 0, bf = b->outcomes[kParticleFailed] != 0;
      return af != bf ? af : a->draws > b->draws;
    });
    const double ms = g_usParticles.exchange(0) / f / 1000;
    fprintf(out,
            "   particles per frame%s: draws %.2f (vertex-shader expanded %.2f, CPU expanded %.2f), %.1f quads (%.0f "
            "triangles, %.0f vertices), %.3f ms (%.3f ms per 1000 quads) | submitted: raster additive %.2f, raster "
            "blended %.2f, raster other blend %.2f, traced %.2f ($addself / $addbasetexture2 as alpha blend %.2f, "
            "overbright %.2f) | dropped: faded out %.2f, stencil only %.2f, not understood %.2f\n",
            particlesEnabled() ? "" : " (OPENREMIX_PROXY_PARTICLES=0: off)", draws / f, expandedDraws / f,
            (draws - expandedDraws) / f, triangles / 2 / f, triangles / f, vertices / f, ms,
            triangles ? ms / (triangles / 2 / f) * 1000 : 0.0, outcomes[kParticleRasterAdditive] / f,
            outcomes[kParticleRasterBlended] / f, outcomes[kParticleRasterOther] / f, outcomes[kParticleTraced] / f,
            premultiplied / f, overbright / f, outcomes[kParticleFaded] / f, outcomes[kParticleStencil] / f,
            outcomes[kParticleFailed] / f);
    fprintf(out,
            "     sprite overlay draws per frame: soft depth %.2f (scale confirmed by the pixel shader's constants "
            "%.2f), frames blended %.2f, brighter frame %.2f, green/alpha %.2f, second sequence %.2f, modulate "
            "(mod2x / multiply) %.2f\n",
            soft / f, softConfirmed / f, frameBlend / f, frameMax / f, greenAlpha / f, sequence / f, modulate / f);
    for (size_t i = 0; i < rows.size() && i < 16; ++i) {
      const ParticleRow &row = *rows[i];
      fprintf(out, "     %s, %s, vs %016llX: draws %.2f, %.1f quads |", row.shader.c_str(), kParticleKindNames[row.kind],
              static_cast<unsigned long long>(row.program), row.draws / f, row.triangles / 2 / f);
      for (int k = 0; k < kParticleOutcomes; ++k)
        if (row.outcomes[k])
          fprintf(out, " %s %.2f", kParticleOutcomeNames[k], row.outcomes[k] / f);
      if (row.outcomes[kParticleFailed])
        fprintf(out, " (%s)", row.failure);
      if (row.secondFrames)
        fprintf(out, " | second sheet frame leads on %.0f%% of vertices",
                100.0 * row.secondFrames / std::max(row.vertices, 1u));
      if (row.soft || row.frameBlend || row.frameMax || row.greenAlpha || row.sequence || row.modulate)
        fprintf(out, " | sprite: soft %.2f frames %.2f brighter %.2f green/alpha %.2f sequence %.2f modulate %.2f",
                row.soft / f, row.frameBlend / f, row.frameMax / f, row.greenAlpha / f, row.sequence / f,
                row.modulate / f);
      fprintf(out, " | %s %s | [%s]\n", row.outcomes[kParticleFailed] ? "last failing material" : "first material",
              row.material.c_str(), row.declaration.c_str());
    }
    if (rows.size() > 16)
      fprintf(out, "     ... and %zu more combinations\n", rows.size() - 16);
    g_particleRows.clear();
    // Blended draws from dynamic buffers the particle rules did not take
    // (general path: traced surfaces; additive ones rasterized as before).
    std::vector<const std::pair<const std::string, ParticleLeft> *> left;
    for (const auto &item : g_particlesLeft)
      left.push_back(&item);
    std::sort(left.begin(), left.end(), [](const auto *a, const auto *b) { return a->second.draws > b->second.draws; });
    if (!left.empty()) {
      fprintf(out, "     blended dynamic draws left to the general path (not particles by shader or material path), by "
                   "material:");
      for (size_t i = 0; i < left.size() && i < 12; ++i)
        fprintf(out, " %s (%s, blend %u/%u) %.2f;", left[i]->first.c_str(), left[i]->second.shader.c_str(),
                left[i]->second.srcBlend, left[i]->second.dstBlend, left[i]->second.draws / f);
      fprintf(out, "%s\n", left.size() > 12 ? " ..." : "");
    }
    g_particlesLeft.clear();
    fprintf(out, "     camera-space draws (identity view matrix: the client particle manager's) per frame: particles %.2f, "
                 "other blended dynamic draws %.2f, both placed in the world by the view drawn before; left out: other "
                 "draws %.2f, without a view before them %.2f\n",
            g_cameraSpaceParticles.exchange(0) / f, g_cameraSpaceOther.exchange(0) / f, g_cameraSpaceLeft.exchange(0) / f,
            g_cameraSpaceNoView.exchange(0) / f);
  }
  {
    // Decals: per frame by what they paint and by what marked them (a draw
    // can be marked several ways), then the materials that drew them.
    const uint32_t along = g_stats.windingAlong.load(), against = g_stats.windingAgainst.load();
    fprintf(out,
            "   decals per frame: alpha %.1f, mod2x %.1f, multiply %.1f, opaque/alpha-tested (lifted only) %.1f | marked by: "
            "$decal %.1f, Decal* shader %.1f, depth bias %.1f, rtx.conf %.1f (depth bias alone %.1f, material without "
            "depth bias %.1f) | in world lists %.1f, on models (skinned) %.1f | lifted along vertex normals %.1f, face "
            "normals %.1f | with vertex colours %.1f\n"
            "     not painted: decal material with another blend state (additive, ...) %.1f, draw under a "
            "DecalModulate material that is not mod2x %.1f, no mesh %.1f | DecalModulate with vertex-alpha fade (not "
            "applied) %.1f | winding of meshes with normals (since launch): e1 x e2 along the normal %u, against %u -> "
            "face-normal sign %+.0f\n",
            g_stats.decalKinds[kDecalAlpha].exchange(0) / f, g_stats.decalKinds[kDecalMod2x].exchange(0) / f,
            g_stats.decalKinds[kDecalMultiply].exchange(0) / f, g_stats.decalKinds[kDecalOpaque].exchange(0) / f,
            g_stats.decalByFlag.exchange(0) / f, g_stats.decalByShader.exchange(0) / f,
            g_stats.decalByBias.exchange(0) / f, g_stats.decalByCategory.exchange(0) / f,
            g_stats.decalBiasOnly.exchange(0) / f, g_stats.decalMaterialOnly.exchange(0) / f,
            g_stats.decalWorld.exchange(0) / f, g_stats.decalSkinned.exchange(0) / f,
            g_stats.decalVertexNormals.exchange(0) / f, g_stats.decalFaceNormals.exchange(0) / f,
            g_stats.decalVertexColors.exchange(0) / f, g_stats.decalOtherBlend.exchange(0) / f,
            g_stats.decalNotItsBlend.exchange(0) / f, g_stats.decalNoMesh.exchange(0) / f,
            g_stats.decalModulateFade.exchange(0) / f, along, against, faceNormalSign(D3DCULL_CCW));
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    std::vector<const std::pair<const std::string, DecalSeen> *> order;
    for (const auto &item : g_decalsSeen)
      order.push_back(&item);
    std::sort(order.begin(), order.end(), [](const auto *a, const auto *b) { return a->second.draws > b->second.draws; });
    static const char *const kinds[] = {"-", "alpha", "mod2x", "multiply", "opaque"};
    for (size_t i = 0; i < order.size() && i < 16; ++i) {
      const DecalSeen &seen = order[i]->second;
      fprintf(out, "     decal %s (%s): %s, by%s%s%s%s, %.2f draws/frame\n", order[i]->first.c_str(), seen.shader.c_str(),
              kinds[seen.kind], seen.how & kDecalByFlag ? " $decal" : "", seen.how & kDecalByShader ? " shader" : "",
              seen.how & kDecalByBias ? " depth-bias" : "", seen.how & kDecalByCategory ? " rtx.conf" : "",
              seen.draws / f);
    }
    if (order.size() > 16)
      fprintf(out, "     ... and %zu more decal materials\n", order.size() - 16);
    g_decalsSeen.clear();
  }
  {
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    if (!g_census.empty())
      fprintf(out, "   stencil / render-target draws per frame (D3D compare: 1 never 2 less 3 equal 4 lessequal 5 greater "
                   "6 notequal 7 gequal 8 always; stencil op: 1 keep 3 replace 4 incrsat 7 incr):\n");
    for (const auto &[key, row] : g_census)
      fprintf(out, "     %7.2f draws %8.1f prims  %s\n", row.draws / f, row.primitives / f, key.c_str());
    // The statistics file holds one interval: every state seen since launch
    // also goes, once, to shaderapi_proxy_census.log (texture and target
    // pointers left out of the identity, so a state is not repeated per
    // texture), with the interval it first appeared in.
    static std::unordered_set<std::string> logged;
    static uint32_t interval = 0;
    ++interval;
    FILE *log = nullptr;
    for (const auto &[key, row] : g_census) {
      std::string identity = key;
      for (size_t at = identity.find("0000"); at != std::string::npos; at = identity.find("0000", at)) {
        size_t end = at;
        while (end < identity.size() && std::isxdigit(static_cast<unsigned char>(identity[end])))
          ++end;
        identity.replace(at, end - at, "*");
      }
      if (!logged.insert(identity).second)
        continue;
      if (!log)
        log = _wfopen((g_moduleDir + L"\\shaderapi_proxy_census.log").c_str(), logged.size() == 1 ? L"w" : L"a");
      if (log)
        fprintf(log, "[%4u] %7.2f draws %8.1f prims  %s\n", interval, row.draws / f, row.primitives / f, key.c_str());
    }
    if (log)
      fclose(log);
    g_census.clear();
  }
  {
    static const float scale = envScale("OPENREMIX_PROXY_EMISSIVE_SCALE", 1.0f);
    size_t variants = 0, pinned = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(g_textureLock);
      variants = g_emissiveMaterials.size();
      for (const auto &[hash, material] : g_emissiveMaterials)
        pinned += material.pins != 0;
    }
    fprintf(out,
            "   emissive draws per frame (scale %.2f): unlit %.1f (with vertex colours %.1f), self-illuminated: base "
            "alpha %.1f, mask texture %.1f, envmap mask alpha %.1f | lighting the scene %.1f | materials: %zu live "
            "(%zu held by the BSP world), created %u, evicted %u, rejected %u | stayed lit: no base texture %.1f, mask "
            "without a texture hash %.1f, mask not uploaded %.1f\n",
            scale, g_stats.unlitDraws.exchange(0) / f, g_stats.unlitVertexColorDraws.exchange(0) / f,
            g_stats.selfIllumDraws.exchange(0) / f, g_stats.selfIllumMaskDraws.exchange(0) / f,
            g_stats.selfIllumMaskAlphaDraws.exchange(0) / f, g_stats.emissiveLightDraws.exchange(0) / f, variants,
            pinned, g_stats.emissiveCreated.exchange(0), g_stats.emissiveEvicted.exchange(0),
            g_stats.emissiveFailed.exchange(0), g_stats.emissiveNoTexture.exchange(0) / f,
            g_stats.emissiveMaskNoHash.exchange(0) / f, g_stats.emissiveMaskNotUploaded.exchange(0) / f);
    fprintf(out,
            "   under water (bsp water leaves at the eye; OPENREMIX_PROXY_UNDERWATER): %.0f%% of frames | flat Refract "
            "screen effects dropped per frame %.1f\n",
            100.0 * g_stats.underwaterFrames.exchange(0) / std::max(1.0, f), g_stats.refractOverlaysDropped.exchange(0) / f);
    fprintf(out,
            "   PBR conversions (pbr_rules; OPENREMIX_PROXY_PBR=%d) per frame: %.1f draws, with a normal map %.1f, a "
            "roughness map %.1f, a metal map %.1f | stayed diffuse: not the material's base texture %.1f | a map not uploaded %.1f | "
            "lit PBR materials created %u\n",
            pbr::enabled() ? 1 : 0, g_stats.pbrDraws.exchange(0) / f, g_stats.pbrNormalDraws.exchange(0) / f,
            g_stats.pbrMapDraws.exchange(0) / f, g_stats.pbrMetalDraws.exchange(0) / f, g_stats.pbrNotBase.exchange(0) / f,
            g_stats.pbrMapsMissing.exchange(0) / f, g_stats.pbrCreated.exchange(0));
    {
      std::lock_guard<std::mutex> lock(g_transientSampleLock);
      for (const auto &[base, n] : g_pbrNotBase) {
        const std::string drawn = bsp::materialName(n.texture);
        fprintf(out, "     not its base texture: %s (material %s): %.2f draws per frame, last texture %016llX%s%s, %zu "
                     "candidate hashes from its file\n",
                base.empty() ? "(no base texture)" : base.c_str(), n.material.c_str(), n.draws / f,
                static_cast<unsigned long long>(n.texture), drawn.empty() ? "" : " = ", drawn.c_str(), n.candidates);
      }
      g_pbrNotBase.clear();
    }
    {
      float m[4];
      {
        std::lock_guard<std::mutex> lock(g_transientSampleLock);
        std::copy_n(g_modulationSample, 4, m);
      }
      float fog[8];
      {
        std::lock_guard<std::mutex> lock(g_transientSampleLock);
        std::copy_n(g_fogSample, 8, fog);
      }
      const uint32_t fogFrames = g_fogFrames.exchange(0);
      fprintf(out,
              "   scene fog (remixapi_SetFogState; OPENREMIX_PROXY_FOG): sent in %u of %u frames | last: mode %.0f (-1 not "
              "read), start %.0f, end %.0f, max density %.3f, colour %.0f %.0f %.0f -> end sent %.0f\n",
              g_fogSent.exchange(0), fogFrames, fog[0], fog[1], fog[2], fog[3], fog[4], fog[5], fog[6], fog[7]);
      fprintf(out,
              "   draws tinted by their modulation colour (pixel shader c1: render colour x alpha, as the texture factor) per "
              "frame: %.1f, of them adding nothing (not drawn) %.1f, masked by the base texture's alpha "
              "($blendtintbybasealpha) %.1f | not tinted: near-black tint by base alpha with $phongalbedotint (a "
              "metalness mask) %.1f | last: %.3f %.3f %.3f x %.3f\n",
              g_stats.modulatedDraws.exchange(0) / f, g_stats.modulatedBlack.exchange(0) / f,
              g_stats.modulatedMasked.exchange(0) / f, g_stats.modulatedMetal.exchange(0) / f, m[0], m[1], m[2], m[3]);
    }
  fprintf(out, "   cable (rope) draws per frame: base texture by name %.1f, not resolved (untextured) %.1f\n",
          g_stats.cableDraws.exchange(0) / f, g_stats.cableNoBase.exchange(0) / f);
  fprintf(out, "   wireframe draws (D3DFILL_WIREFRAME, raster-only lines) per frame: %.1f\n",
          g_stats.wireframeDraws.exchange(0) / f);
  fprintf(out, "   flexed draws per frame (faces with flex deltas this frame): %.1f, meshes refreshed in place %.1f (%zu flex meshes live) | flex stream bound with scale 0: %.1f\n",
          g_stats.flexed.exchange(0) / f, g_stats.flexRefreshed.exchange(0) / f, g_flexMeshes.size(),
          g_stats.flexUnscaled.exchange(0) / f);
  fprintf(out, "   eye draws per frame (Eyes and EyeRefract shaders): %.1f, iris overlays submitted %.1f, EyeRefract eyes retextured %.1f\n",
          g_stats.eyes.exchange(0) / f, g_stats.eyeIrises.exchange(0) / f, g_stats.eyeRefracts.exchange(0) / f);
  {
    // Cached meshes built more than once in the interval (see MeshBuilds).
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    static const char *const kFields[13] = {"vb", "ib", "decl", "vbVersion", "ibVersion", "offset", "stride",
                                            "baseVertex", "minIndex", "numVertices", "startIndex", "primCount",
                                            "bonesPerVertex"};
    std::vector<std::pair<uint32_t, std::string>> order;
    for (const auto &[name, b] : g_meshBuilds)
      if (b.count > 1)
        order.push_back({b.count, name});
    std::sort(order.rbegin(), order.rend());
    fprintf(out, "   cached meshes rebuilt (no motion vectors while it lasts): %zu materials with repeated builds\n",
            order.size());
    for (size_t i = 0; i < order.size() && i < 12; ++i) {
      const MeshBuilds &b = g_meshBuilds[order[i].second];
      std::string changed;
      for (int k = 0; k < 13; ++k)
        if (b.last[k] != b.previous[k]) {
          char text[96];
          snprintf(text, sizeof(text), " %s %llu -> %llu", kFields[k], (unsigned long long)b.previous[k],
                   (unsigned long long)b.last[k]);
          changed += text;
        }
      fprintf(out, "     %s: %.2f builds per frame; last two keys differ in:%s\n", order[i].second.c_str(),
              b.count / f, changed.empty() ? " nothing (a variant flag)" : changed.c_str());
    }
    g_meshBuilds.clear();
  }
  {
    std::lock_guard<std::mutex> lock(g_transientSampleLock);
    fprintf(out, "   draws matched to a material by base texture while another was bound: %zu materials; emissive draws of a "
                 "texture its bound material's file does not list %.1f per frame\n",
            g_rebound.size(), g_stats.emissiveNotItsTexture.exchange(0) / f);
    for (const auto &[material, r] : g_rebound)
      fprintf(out, "     %s: %.2f draws per frame, last while %s was bound\n", material.c_str(), r.draws / f,
              r.bound.empty() ? "(unknown)" : r.bound.c_str());
    g_rebound.clear();
  }
  }
  fprintf(out, "   halos per frame: %.2f groups, %.2f source instances | stencil masks: %.2f groups, %.2f sources, "
               "%.2f overlays, %.2f UI draws, %.2f cut instances\n",
          g_stats.haloGroups.exchange(0) / f, g_stats.haloSources.exchange(0) / f, g_stats.maskGroups.exchange(0) / f,
          g_stats.maskSources.exchange(0) / f, g_stats.maskOverlays.exchange(0) / f, g_stats.maskUi.exchange(0) / f,
          g_stats.maskCuts.exchange(0) / f);
  if (const double culled = g_stats.cameraCulledModels.exchange(0) / f; culled > 0)
    fprintf(out, "   engine-drawn models with back faces hidden from the camera "
                 "(OPENREMIX_PROXY_CULL_BACKFACES_MODELS): %.1f per frame\n", culled);
  fprintf(out, "   UI draws %.1f (render-target sampling skipped %.1f, stencil-masked dropped %.1f); CPU texture copies %zu\n",
          g_stats.uiDraws.exchange(0) / f, g_stats.uiNoTexture.exchange(0) / f, g_stats.uiStencil.exchange(0) / f, [] {
            std::lock_guard<std::recursive_mutex> lock(g_textureLock);
            return g_cpuTextures.size();
          }());
  {
    std::lock_guard<std::recursive_mutex> lock(g_textureLock);
    size_t bytes = 0;
    for (auto &[t, c] : g_cpuTextures)
      bytes += c->data.size();
    fprintf(out, "   CPU texture copies: %.1f MiB; UI texture registrations per frame %.2f, region updates %.2f (%.2f MiB, %.2f ms, runtime %.2f ms)\n",
            bytes / 1048576.0, g_uiRegistrations.exchange(0) / f, g_uiRegionUpdates.exchange(0) / f,
            g_uiRegisteredBytes.exchange(0) / f / 1048576.0,
            g_usUiRegister.exchange(0) / f / 1000, g_usUiRegisterApi.exchange(0) / f / 1000);
  }
  fprintf(out, "   flat (2D) %.1f; textures uploaded %u, unsupported formats %u, live %zu\n",
          g_stats.flat.exchange(0) / f, g_stats.textures.exchange(0), g_stats.textureSkipped.exchange(0), [] {
            std::lock_guard<std::recursive_mutex> lock(g_textureLock);
            return g_textures.size();
          }());
  {
    // Render-target contents (onTargetPixels) and what the runtime made of
    // them: with the tool gun out, images arrive every frame and nothing
    // should be created.
    const uint32_t images = g_targetImages.exchange(0), updates = g_targetUpdates.exchange(0);
    fprintf(out, "   render-target images: %.2f per frame (equal to the last, not sent %.2f), updated in place %.2f "
                 "(%.3f ms each in the call), records created %u (replacing a dynamic one %u), update failures %u%s\n",
            images / f, g_targetUnchanged.exchange(0) / f, updates / f,
            g_usTargetUpdate.exchange(0) / (updates ? updates : 1) / 1000.0, g_targetCreated.exchange(0),
            g_targetRecreated.exchange(0), g_targetFailed.exchange(0),
            targetUpdateEntry() ? "" : " | in-place updates off: a new texture per image");
    static const auto statistics = reinterpret_cast<PFN_remixapi_GetTextureStatisticsEXT>(reinterpret_cast<void *>(
        GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetTextureStatisticsEXT")));
    static remixapi_TextureStatisticsEXT last = {};
    remixapi_TextureStatisticsEXT now = {};
    now.structSize = sizeof(now);
    if (statistics && statistics(&now) == REMIXAPI_ERROR_CODE_SUCCESS) {
      const double changed = double(now.updates - last.updates), uploads = double(now.uploads - last.uploads);
      fprintf(out, "   texture mip chains since launch: %u as the game wrote them, %u built by the runtime from level 0, "
                   "%u single-level | levels above 0 seen written %u, textures waiting for levels %zu\n",
              g_textureChains.load(), g_textureChainsBuilt.load(), g_textureSingle.load(), g_textureLevelsSeen.load(),
              g_pendingTextures.size());
      fprintf(out, "   runtime textures per frame: created %.2f destroyed %.2f, materials created %.2f destroyed %.2f | "
                   "GPU objects: textures made %.2f released %.2f, materials made %.2f released %.2f, making %.3f ms "
                   "(%.1f KiB staged), releasing %.3f ms | in-place updates: calls %.2f, changed pixels %.2f "
                   "(%.1f KiB, %.3f ms each), uploads %.2f (%.1f KiB, %.3f ms each) | emissive estimates remade %llu "
                   "(%.2f ms in all)\n",
              (now.texturesCreated - last.texturesCreated) / f, (now.texturesDestroyed - last.texturesDestroyed) / f,
              (now.materialsCreated - last.materialsCreated) / f,
              (now.materialsDestroyed - last.materialsDestroyed) / f,
              (now.gpuTexturesCreated - last.gpuTexturesCreated) / f,
              (now.gpuTexturesDestroyed - last.gpuTexturesDestroyed) / f,
              (now.gpuMaterialsCreated - last.gpuMaterialsCreated) / f,
              (now.gpuMaterialsDestroyed - last.gpuMaterialsDestroyed) / f,
              (now.gpuCreateMilliseconds - last.gpuCreateMilliseconds) / f,
              (now.gpuCreateBytes - last.gpuCreateBytes) / f / 1024.0,
              (now.gpuDestroyMilliseconds - last.gpuDestroyMilliseconds) / f,
              (now.updateCalls - last.updateCalls) / f, changed / f,
              (now.updateBytes - last.updateBytes) / (changed ? changed : 1) / 1024.0,
              (now.updateMilliseconds - last.updateMilliseconds) /
                  (now.updateCalls != last.updateCalls ? double(now.updateCalls - last.updateCalls) : 1),
              uploads / f, (now.uploadBytes - last.uploadBytes) / (uploads ? uploads : 1) / 1024.0,
              (now.uploadMilliseconds - last.uploadMilliseconds) / (uploads ? uploads : 1),
              static_cast<unsigned long long>(now.emissionRefreshes - last.emissionRefreshes),
              now.emissionRefreshMilliseconds - last.emissionRefreshMilliseconds);
      last = now;
    }
  }
  fprintf(out, "   nonFinite %u rejected %u; camera (%u draws) view row3 %.1f %.1f %.1f proj diag %.3f %.3f %.4f %.3f\n",
          g_stats.nonFinite.exchange(0), g_stats.rejected.exchange(0), g_lastCamera.draws, g_lastCamera.view[12],
          g_lastCamera.view[13], g_lastCamera.view[14], g_lastCamera.projection[0], g_lastCamera.projection[5],
          g_lastCamera.projection[10], g_lastCamera.projection[14]);
}

} // namespace proxy::remix

// Exports for in-process UI contributors (see g_uiCallback).
extern "C" __declspec(dllexport) void __cdecl openremix_ui_set_callback(void(__cdecl *callback)()) {
  proxy::remix::g_uiCallback = callback;
}

// Adds draws to this frame's UI list. Command index offsets refer to
// `indices`; indices refer to `vertices` (positions in back-buffer pixels).
extern "C" __declspec(dllexport) void __cdecl openremix_ui_append(const remixapi_UIVertex *vertices,
                                                                   uint32_t vertexCount, const uint32_t *indices,
                                                                   uint32_t indexCount,
                                                                   const remixapi_UIDrawCommand *commands,
                                                                   uint32_t commandCount) {
  using namespace proxy::remix;
  if (!vertices || !indices || !commands)
    return;
  const uint32_t baseVertex = uint32_t(g_uiVertices.size()), baseIndex = uint32_t(g_uiIndices.size());
  g_uiVertices.insert(g_uiVertices.end(), vertices, vertices + vertexCount);
  for (uint32_t i = 0; i < indexCount; ++i)
    g_uiIndices.push_back(baseVertex + indices[i]);
  for (uint32_t c = 0; c < commandCount; ++c) {
    remixapi_UIDrawCommand command = commands[c];
    command.indexOffset += baseIndex;
    g_uiCommands.push_back(command);
    g_uiCommandMasks.push_back({-1, false});
  }
}

// dxvk-remix variables that map onto proxy behaviour. Returns 1 when handled;
// callers forward other variables to the runtime.
extern "C" __declspec(dllexport) int __cdecl openremix_set_legacy_variable(const char *key, const char *value) {
  if (!key || !value)
    return 0;
  if (proxy::categories::set(key, value))
    return 1;
  if (std::strcmp(key, "rtx.skyBrightness") == 0) {
    const float brightness = float(atof(value));
    proxy::remix::g_skyBrightness = std::isfinite(brightness) && brightness > 0 ? brightness : 1.0f;
    return 1;
  }
  return 0;
}

namespace proxy::remix {
bool texturePixels(const void *texture, uint32_t &width, uint32_t &height, uint32_t &version, bool &bgra,
                   std::vector<uint8_t> *pixels) {
  std::lock_guard<std::recursive_mutex> lock(g_textureLock);
  auto it = g_cpuTextures.find(texture);
  if (it == g_cpuTextures.end())
    return false;
  const auto &t = *it->second;
  const bool wide = t.format == D3DFMT_A8R8G8B8 || t.format == D3DFMT_X8R8G8B8;
  if (!wide && t.format != D3DFMT_DXT1 && t.format != D3DFMT_DXT5)
    return false;
  width = t.width;
  height = t.height;
  version = t.version;
  bgra = wide;
  if (pixels) {
    if (t.format == D3DFMT_DXT1)
      *pixels = formats::decodeBC1(t.data.data(), t.width, t.height);
    else if (t.format == D3DFMT_DXT5)
      *pixels = formats::decodeBC3(t.data.data(), t.width, t.height);
    else {
      *pixels = t.data;
      if (t.format == D3DFMT_X8R8G8B8)
        for (size_t i = 3; i < pixels->size(); i += 4)
          (*pixels)[i] = 0xff;
    }
    if (pixels->size() != size_t(width) * height * 4)
      return false;
  }
  return true;
}

bool onMarker(const char *text) {
  constexpr char kPrefix[] = "openremix:";
  if (!text || std::strncmp(text, kPrefix, sizeof(kPrefix) - 1) != 0)
    return false;
  text += sizeof(kPrefix) - 1;
  auto argument = [&](const char *name) -> const char * {
    const size_t n = std::strlen(name);
    if (std::strncmp(text, name, n) != 0 || (text[n] != ' ' && text[n] != 0))
      return nullptr;
    return text[n] == ' ' ? text + n + 1 : text + n;
  };
  auto depth = [](int &counter, const char *value) {
    counter += std::atoi(value) ? 1 : -1;
    if (counter < 0)
      counter = 0;
  };
  if (const char *v = argument("world_lists")) {
    if (g_worldLists == 0 && std::atoi(v) && g_shaderApi && g_worldViews.size() < 16 && d3d::backBufferBound()) {
      WorldView w;
      getMatrix(kMatrixView, w.view);
      getMatrix(kMatrixProjection, w.projection);
      // As onDraw stores views: left-handed.
      if (std::fabs(std::fabs(w.projection[11]) - 1.0f) < 1e-3f && std::fabs(w.projection[15]) < 1e-3f) {
        if (w.projection[11] < 0) {
          for (int r = 0; r < 4; ++r)
            w.view[r * 4 + 2] = -w.view[r * 4 + 2];
          for (int c = 0; c < 4; ++c)
            w.projection[8 + c] = -w.projection[8 + c];
        }
        g_worldViews.push_back(w);
        readSceneFog();
      }
    } else if (g_worldLists == 0 && std::atoi(v) && g_shaderApi) {
      // A view drawn into a render-target texture: a screen camera's.
      if (const void *target = d3d::boundTargetTexture()) {
        WorldView w;
        getMatrix(kMatrixView, w.view);
        getMatrix(kMatrixProjection, w.projection);
        if (std::fabs(std::fabs(w.projection[11]) - 1.0f) < 1e-3f && std::fabs(w.projection[15]) < 1e-3f) {
          if (w.projection[11] < 0) {
            for (int r = 0; r < 4; ++r)
              w.view[r * 4 + 2] = -w.view[r * 4 + 2];
            for (int c = 0; c < 4; ++c)
              w.projection[8 + c] = -w.projection[8 + c];
          }
          // Not the main camera's own view drawn again (an effect's
          // render.RenderView of the scene, as dev_ssr's): no screen shows it.
          if (!samePlacement(w.view, g_lastCamera.view))
            noteScreenView(target, w.view, w.projection);
        }
      }
    }
    depth(g_worldLists, v);
  }
  else if (const char *v = argument("overlays"))
    depth(g_overlays, v);
  else if (const char *v = argument("body"))
    g_markerCategories = std::atoi(v) ? REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL |
                                            REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY
                                      : 0;
  else if (const char *v = argument("lights"))
    engine_lights::activate(uint32_t(std::strtoul(v, nullptr, 10)));
  else if (const char *v = argument("map")) {
    loadMap(v);
  } else
    return false;
  return true;
}
} // namespace proxy::remix

// l4d2-rtx: the map that is loading (nullptr or "" when leaving it). Returns
// 1 if its world is taken from the BSP, so the engine's world draws are dropped.
extern "C" __declspec(dllexport) int __cdecl openremix_proxy_load_map(const char *mapName) {
  using namespace proxy::remix;
  return loadMap(mapName) ? 1 : 0;
}

// l4d2-rtx: whether the BSP world draws the static prop at this placement
// (matrix3x4_t, model to world); the engine can skip drawing it. Thread-safe.
extern "C" __declspec(dllexport) int __cdecl openremix_proxy_provides_prop(const float *modelToWorld) {
  return proxy::bsp::providesProp(modelToWorld) ? 1 : 0;
}

// l4d2-rtx: wraps IVRenderView::DrawWorldLists (1 before, 0 after).
extern "C" __declspec(dllexport) void __cdecl openremix_proxy_world_lists(int begin) {
  using namespace proxy::remix;
  g_worldLists += begin ? 1 : -1;
  if (g_worldLists < 0)
    g_worldLists = 0;
}

// l4d2-rtx: wraps COverlayMgr::RenderOverlays (1 before, 0 after).
extern "C" __declspec(dllexport) void __cdecl openremix_proxy_overlays(int begin) {
  using namespace proxy::remix;
  g_overlays += begin ? 1 : -1;
  if (g_overlays < 0)
    g_overlays = 0;
}
