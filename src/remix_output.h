#pragma once
#include <windows.h>

#include <d3d9.h>

#include <cstdint>
#include <vector>
#include <cstdio>

// Converts the draws the original shader API issues into Remix API calls
// (through remix_bridge.dll) and presents them into the game window.
namespace proxy::engine_materials {
struct Shading;
}
namespace proxy::vsprog {
struct Program;
}
namespace proxy::remix {

struct VertexStream {
  const uint8_t *data = nullptr; // shadow copy of the vertex buffer (nullptr if unknown)
  uint32_t size = 0;
  uint32_t offset = 0;
  uint32_t stride = 0;
  const void *buffer = nullptr; // D3D buffer identity
  uint32_t version = 0;
  bool dynamic = false;          // D3DUSAGE_DYNAMIC: rewritten continuously
};

struct IndexStream {
  const uint8_t *data = nullptr;
  uint32_t size = 0;
  bool is32 = false;
  const void *buffer = nullptr;
  uint32_t version = 0;
  bool dynamic = false;
};

// Everything a DrawIndexedPrimitive needs, gathered by the D3D hooks.
struct Draw {
  D3DPRIMITIVETYPE type;
  int baseVertex;
  uint32_t minIndex, numVertices, startIndex, primCount;
  VertexStream streams[4];
  IndexStream indices;
  const D3DVERTEXELEMENT9 *elements = nullptr; // terminated by D3DDECL_END
  const void *declaration = nullptr;
  const float *vsConstants = nullptr;          // 256 float4 registers
  const float *psConstants = nullptr;          // 224 float4 pixel shader registers
  const void *vertexShader = nullptr;          // IDirect3DVertexShader9 identity
  const void *pixelShader = nullptr;           // IDirect3DPixelShader9 identity
  const vsprog::Program *vertexProgram = nullptr; // it, decoded (nullptr: not seen at creation)
  const void *texture0 = nullptr;              // texture bound to sampler 0 (base texture)
  const void *texture1 = nullptr;              // texture bound to sampler 1 (Eyes shader: the iris)
  const void *stageTextures[8] = {};           // samplers 0-7 (diagnostics: the shader probe)
  bool mainTarget = false;                     // render target 0 is the back buffer
  uint32_t colorWrite = 0xf;                   // D3DRS_COLORWRITEENABLE
  bool blend = false;                          // D3DRS_ALPHABLENDENABLE
  uint32_t srcBlend = D3DBLEND_ONE, dstBlend = D3DBLEND_ZERO;
  uint32_t blendOp = D3DBLENDOP_ADD;
  bool separateAlpha = false;                  // D3DRS_SEPARATEALPHABLENDENABLE
  uint32_t srcBlendAlpha = D3DBLEND_ONE, dstBlendAlpha = D3DBLEND_ZERO, blendOpAlpha = D3DBLENDOP_ADD;
  bool alphaTest = false;
  uint32_t alphaRef = 0;
  uint32_t alphaFunc = D3DCMP_ALWAYS;
  bool depthBias = false;       // D3DRS_DEPTHBIAS / SLOPESCALEDEPTHBIAS set (decals, overlays)
  uint32_t cullMode = D3DCULL_CCW; // D3DRS_CULLMODE
  bool stencil = false;                 // D3DRS_STENCILENABLE
  uint32_t stencilFunc = D3DCMP_ALWAYS; // D3DRS_STENCILFUNC
  uint32_t stencilPass = D3DSTENCILOP_KEEP; // D3DRS_STENCILPASS
  bool depthTest = true;                // D3DRS_ZENABLE and ZFUNC != ALWAYS
  uint32_t stencilRef = 0, stencilMask = 0xffffffff, stencilWriteMask = 0xffffffff;
  uint32_t stencilFail = D3DSTENCILOP_KEEP, stencilZFail = D3DSTENCILOP_KEEP;
  uint32_t zFunc = D3DCMP_LESSEQUAL; // D3DRS_ZFUNC
  uint32_t fillMode = D3DFILL_SOLID;  // D3DRS_FILLMODE (the Wireframe shader: D3DFILL_WIREFRAME)
  bool zEnable = true, zWrite = true;
  // Render target 0 when it is a render-target texture (nullptr: the back
  // buffer or a plain surface), and whether sampler 0 holds such a texture.
  const void *target = nullptr;
  uint32_t targetWidth = 0, targetHeight = 0, targetFormat = 0;
  bool texture0Target = false;
  uint32_t forkCategories = 0; // dxvk-remix fork InstanceCategories (render state 42, set by l4d2-rtx)
  // For host passes (screen-space effects replayed by the runtime).
  const void *const *textures = nullptr;        // the 16 pixel samplers' textures
  uint32_t textureTargets = 0;                  // bit n: textures[n] is a render-target texture
  const DWORD (*samplerStates)[16] = nullptr;   // [sampler][D3DSAMPLERSTATETYPE]
  const int32_t *psConstantsI = nullptr;        // 16 int4 pixel shader registers
  const int32_t *vsConstantsI = nullptr;        // 16 int4 vertex shader registers
  uint32_t psBools = 0, vsBools = 0;            // 16 boolean registers each
  D3DVIEWPORT9 viewport = {};
  RECT scissor = {};
  bool scissorTest = false;
};

bool enabled();

// A texture's contents as the proxy keeps them (its CPU copy): size, the
// version its contents are at, whether the pixels are BGRA (else RGBA) and,
// when `pixels` is given, the pixels themselves, decoded. False: unknown.
bool texturePixels(const void *texture, uint32_t &width, uint32_t &height, uint32_t &version, bool &bgra,
                   std::vector<uint8_t> *pixels);

// Called from IDirect3D9::CreateDevice. May redirect presentation to a hidden
// window; `gameWindow` receives Remix's output.
void onCreateDevice(HWND gameWindow, D3DPRESENT_PARAMETERS *pp);
void onDeviceCreated(IDirect3DDevice9 *device);
void onDraw(const Draw &draw);

// Texture contents: level 0 of a 2D texture was written (`pitch` = bytes per
// row, or per block row for BC formats; `levels`: the texture's mip count).
void onTextureUpload(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format, const uint8_t *data,
                     uint32_t pitch, uint32_t levels);
// A whole level above 0 was written (`width`, `height`: level 0's).
void onTextureLevel(const void *texture, uint32_t level, uint32_t width, uint32_t height, uint32_t levelWidth,
                    uint32_t levelHeight, D3DFORMAT format, const uint8_t *data, uint32_t pitch, uint32_t levels);
// A sub-rectangle of level 0 was written (`data` points at the rect's first row).
void onTexturePartialUpload(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format, const RECT &rect,
                            const uint8_t *data, uint32_t pitch);
// UpdateTexture(src, dst): dst now shows src's contents.
void onTextureCopy(const void *source, const void *destination);
void onTextureReleased(const void *texture);
// IShaderAPI::Bind (engine_materials): the material now bound and its name.
void onMaterialBound(const engine_materials::Shading &shading, const char *material);
// Contents read back from a render-target texture the scene samples (32-bit,
// B8G8R8A8 rows). They rewrite the texture's image in place, at once (the
// runtime's dynamic texture: nothing is created); the first image of a
// texture makes its record after this frame's Present.
void onTargetPixels(const void *texture, uint32_t width, uint32_t height, D3DFORMAT format, const uint8_t *data,
                    uint32_t pitch);
// The draw being captured has a perspective projection (not 2D).
bool drawIsPerspective();
void onPresent();
// The game is shutting its renderer down: stop OpenRemix while every thread
// is alive (exit-time teardown after ExitProcess hung on its workers).
void shutdown();

// IShaderAPI::SetLights(int count, const LightDesc_t *lights): the lights the
// game hands to models (including the map's directional sun).
void onSetLights(int count, const void *lights);

// IShaderAPI::SetPIXMarker text starting "openremix:" (l4d2-rtx signals sent
// through IMatRenderContext::SetPIXMarker, so they stay ordered with the
// draws also when the material system queues rendering to its own thread):
//   world_lists 0|1, overlays 0|1  (engine draws the BSP world replaces)
//   body 0|1                       (the local player's first-person body)
//   map <name>                     (the map loading; empty when leaving)
// Returns whether the marker was one of these.
bool onMarker(const char *text);

// Supplies the real shader API object for matrix queries (GetMatrix).
void setShaderApi(void *realShaderApi);

// The current view and projection (row-major D3DX), from the shader API.
bool viewMatrices(float view[16], float projection[16]);

// Whether a draw belongs to the 2D skybox (dynamic-buffer opaque draws inside
// IVRenderView::DrawWorldLists): captured by the D3D hooks, not submitted.
bool isSkyDraw(const Draw &draw);

// The 2D skybox as six BGRA faces (proxy::sky::faceBasis, kFaceSize square):
// becomes the dome light (environment and background). Returns whether the
// faces differed from the dome's (false: nothing was resampled or uploaded).
void setSkyFaces(const uint8_t *const faces[6]);
// A dome was made from faces given since the last call (the faces are
// compared and resampled on another thread, some frames after they are given).
bool takeSkyChanged();
// Counts the times the dome was dropped (a new map): faces captured before
// belong to another sky.
uint32_t skyEpoch();

void dumpStatistics(FILE *out);
// Around the statistics thread's whole dump (shaderapi_proxy_spikes.log says
// whether a slow frame fell within one).
void noteStatisticsDump(bool begin);
// The thread that presents and how long its current frame has run (ms since
// the last onPresent began); false before the first frame.
bool frameInProgress(unsigned long &thread, double &milliseconds);

} // namespace proxy::remix
