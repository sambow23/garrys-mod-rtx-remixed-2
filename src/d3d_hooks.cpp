// Define the D3D9 interface GUIDs (IID_IDirect3DTexture9) in this unit.
#include <initguid.h>

#include "d3d_hooks.h"
#include "engine_hooks.h"
#include "sky_faces.h"

#include "bsp_world.h"
#include "remix_output.h"
#include "vertex_program.h"
#include "dxvk_hash.h"
#include "host_passes.h"

#include <algorithm>
#include <psapi.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace proxy::d3d {
namespace {

// Vtable slots.
enum : unsigned {
  // IUnknown
  kRelease = 2,
  // IDirect3D9
  kD3DCreateDevice = 16,
  // IDirect3DDevice9
  kReset = 16,
  kPresent = 17,
  kCreateTexture = 23,
  kCreateVertexBuffer = 26,
  kCreateIndexBuffer = 27,
  kUpdateSurface = 30,
  kUpdateTexture = 31,
  kStretchRect = 34,
  kSetRenderTarget = 37,
  kSetViewport = 47,
  kClear = 43,
  kSetRenderState = 57,
  kSetTexture = 65,
  kSetSamplerState = 69,
  kSetScissorRect = 75,
  kDrawPrimitive = 81,
  kDrawIndexedPrimitive = 82,
  kDrawPrimitiveUP = 83,
  kDrawIndexedPrimitiveUP = 84,
  kSetVertexDeclaration = 87,
  kCreateVertexShader = 91,
  kSetVertexShader = 92,
  kSetVertexShaderConstantF = 94,
  kSetVertexShaderConstantI = 96,
  kSetVertexShaderConstantB = 98,
  kSetStreamSource = 100,
  kSetIndices = 104,
  kCreatePixelShader = 106,
  kSetPixelShader = 107,
  kSetPixelShaderConstantF = 109,
  kSetPixelShaderConstantI = 111,
  kSetPixelShaderConstantB = 113,
  kCreateQuery = 118,
  // IDirect3DQuery9
  kQueryIssue = 6,
  kQueryGetData = 7,
  // IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9
  kBufferLock = 11,
  kBufferUnlock = 12,
  // IDirect3DTexture9
  kTextureGetSurfaceLevel = 18,
  kTextureLockRect = 19,
  kTextureUnlockRect = 20,
  // IDirect3DSurface9
  kSurfaceLockRect = 13,
  kSurfaceUnlockRect = 14,
};

void **vtableOf(void *object) { return *static_cast<void ***>(object); }

// Replaces a vtable entry; returns the previous function.
void *patch(void **vtable, unsigned slot, void *hook) {
  DWORD old;
  VirtualProtect(&vtable[slot], sizeof(void *), PAGE_READWRITE, &old);
  void *previous = vtable[slot];
  vtable[slot] = hook;
  VirtualProtect(&vtable[slot], sizeof(void *), old, &old);
  return previous;
}

template <class Fn> void hookSlot(void **vtable, unsigned slot, Fn hook, Fn &original) {
  original = reinterpret_cast<Fn>(patch(vtable, slot, reinterpret_cast<void *>(hook)));
}

uint64_t nowUs() {
  static const double usPerTick = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1e6 / double(f.QuadPart);
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return uint64_t(double(t.QuadPart) * usPerTick);
}

struct Stats {
  std::atomic<uint64_t> usPresent{0}, usLock{0};
  std::atomic<uint32_t> frames{0}, dip{0}, dp{0}, dipUP{0}, dpUP{0}, depthDraws{0}, occlusionQueries{0}, occluderDraws{0}, occluderClears{0}, queriesVisible{0}, queriesHidden{0}, rtChanges{0}, textureBinds{0};
  std::atomic<uint32_t> vsConstCalls{0}, streamSets{0}, partialTextureLocks{0};
  std::atomic<uint32_t> vertexPrograms{0}; // vertex shaders decoded since launch
  std::atomic<uint64_t> dipPrims{0};
  std::atomic<uint64_t> shadowBytes{0}, shadowCopies{0}, wholeLocks{0}, usShadow{0};
  std::atomic<uint32_t> skyDraws{0}, skyCaptures{0}, skyUpdates{0}, skyFailures{0};
  std::atomic<uint64_t> usSky{0}, usSkyReadback{0};
} g_stats;

// --- Buffer shadows -------------------------------------------------------
// CPU copies of vertex/index buffer contents, captured at Unlock.
struct Shadow {
  std::vector<uint8_t> data;
  uint32_t version = 0;
  bool dynamic = false;
  bool is32 = false;
  uint8_t *lockPtr = nullptr;
  uint32_t lockOffset = 0, lockSize = 0;
  bool lockReadOnly = false;
  uint32_t usage = 0, pool = 0, lockFlags = 0; // diagnostics
};
// Diagnostics: Unlock write-back cost per (usage, pool, lock flags).
struct CopyClass {
  uint64_t us = 0, bytes = 0, copies = 0;
};
std::unordered_map<uint64_t, CopyClass> g_copyClasses;
std::mutex g_shadowLock;
std::unordered_map<const void *, Shadow> g_shadows;

using ReleaseFn = ULONG(STDMETHODCALLTYPE *)(IUnknown *);
using LockFn = HRESULT(STDMETHODCALLTYPE *)(void *, UINT, UINT, void **, DWORD);
using UnlockFn = HRESULT(STDMETHODCALLTYPE *)(void *);
ReleaseFn g_vbRelease = nullptr, g_ibRelease = nullptr;
LockFn g_vbLock = nullptr, g_ibLock = nullptr;
UnlockFn g_vbUnlock = nullptr, g_ibUnlock = nullptr;

void forget(const void *buffer) {
  std::lock_guard<std::mutex> lock(g_shadowLock);
  g_shadows.erase(buffer);
}

// Locked buffer memory is GPU memory mapped write-combined (DXVK): writes are
// fast, reads crawl (~5 MB/s). A writable lock therefore hands the game its
// range of the shadow, which holds the buffer's current contents, and Unlock
// writes that range to the real mapping. Read-only locks get the real memory.
// Returns the pointer to give the caller.
void *noteLock(void *buffer, UINT offset, UINT size, void *ptr, DWORD flags) {
  std::lock_guard<std::mutex> lock(g_shadowLock);
  auto it = g_shadows.find(buffer);
  if (it == g_shadows.end())
    return ptr;
  auto &s = it->second;
  s.lockPtr = static_cast<uint8_t *>(ptr);
  s.lockOffset = offset;
  s.lockSize = size ? size : uint32_t(s.data.size()) - offset;
  if (!size)
    ++g_stats.wholeLocks;
  s.lockReadOnly = (flags & D3DLOCK_READONLY) != 0;
  s.lockFlags = flags;
  if (s.lockReadOnly || offset >= s.data.size() || s.lockSize > s.data.size() - offset)
    return ptr;
  return s.data.data() + offset;
}

void noteUnlock(void *buffer) {
  std::lock_guard<std::mutex> lock(g_shadowLock);
  auto it = g_shadows.find(buffer);
  if (it == g_shadows.end())
    return;
  auto &s = it->second;
  if (s.lockPtr && !s.lockReadOnly && s.lockOffset < s.data.size() && s.lockSize <= s.data.size() - s.lockOffset) {
    const uint64_t t0 = nowUs();
    std::memcpy(s.lockPtr, s.data.data() + s.lockOffset, s.lockSize);
    const uint64_t t1 = nowUs();
    g_stats.usShadow += t1 - t0;
    {
      auto &c = g_copyClasses[(uint64_t(s.usage) << 32) | (uint64_t(s.pool) << 24) | s.lockFlags];
      c.us += t1 - t0;
      c.bytes += s.lockSize;
      ++c.copies;
    }
    g_stats.shadowBytes += s.lockSize;
    ++g_stats.shadowCopies;
    ++s.version;
  }
  s.lockPtr = nullptr;
}

ULONG STDMETHODCALLTYPE vbRelease(IUnknown *self) {
  const ULONG refs = g_vbRelease(self);
  if (refs == 0)
    forget(self);
  return refs;
}
ULONG STDMETHODCALLTYPE ibRelease(IUnknown *self) {
  const ULONG refs = g_ibRelease(self);
  if (refs == 0)
    forget(self);
  return refs;
}
HRESULT STDMETHODCALLTYPE vbLock(void *self, UINT offset, UINT size, void **data, DWORD flags) {
  const uint64_t t0 = nowUs();
  const HRESULT hr = g_vbLock(self, offset, size, data, flags);
  g_stats.usLock += nowUs() - t0;
  if (SUCCEEDED(hr) && data)
    *data = noteLock(self, offset, size, *data, flags);
  return hr;
}
HRESULT STDMETHODCALLTYPE ibLock(void *self, UINT offset, UINT size, void **data, DWORD flags) {
  const uint64_t t0 = nowUs();
  const HRESULT hr = g_ibLock(self, offset, size, data, flags);
  g_stats.usLock += nowUs() - t0;
  if (SUCCEEDED(hr) && data)
    *data = noteLock(self, offset, size, *data, flags);
  return hr;
}
HRESULT STDMETHODCALLTYPE vbUnlock(void *self) {
  noteUnlock(self);
  return g_vbUnlock(self);
}
HRESULT STDMETHODCALLTYPE ibUnlock(void *self) {
  noteUnlock(self);
  return g_ibUnlock(self);
}

// --- Textures --------------------------------------------------------------
// Level 0 of 2D textures goes to the Remix output at UnlockRect, straight
// from the locked memory (no CPU copy is kept).
struct TextureLock {
  const uint8_t *bits = nullptr;
  uint32_t pitch = 0;
  bool hasRect = false;
  RECT rect = {};
};
std::mutex g_textureLocks;
std::unordered_map<const void *, TextureLock> g_lockedTextures;
// A texture level's key in g_lockedTextures (a surface's is the surface):
// levels are few and the objects far apart.
const void *levelKey(const void *texture, UINT level) { return static_cast<const uint8_t *>(texture) + level; }

using TextureLockRectFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *,
                                                       DWORD);
using TextureUnlockRectFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DTexture9 *, UINT);
ReleaseFn g_textureRelease = nullptr;
TextureLockRectFn g_textureLockRect = nullptr;
TextureUnlockRectFn g_textureUnlockRect = nullptr;

// Render-target textures (CreateTexture with D3DUSAGE_RENDERTARGET) and the
// one render target 0 currently belongs to.
struct TargetTexture {
  uint32_t width, height;
  D3DFORMAT format;
};
std::unordered_map<const void *, TargetTexture> g_targetTextures;
const void *g_renderTargetTexture = nullptr;

// Render-target textures the scene samples (requestTargetContents). D3D
// rasterizes nothing else: their 2D draws are let through, and when the
// target is left its image is read back, reduced to at most kReadSize a side
// (a readback costs about 1.3 ms per MiB here, and the upload more).
// A target that receives 3D or depth-tested draws is a scene view (water
// reflection, shadows) and is left alone for good.
//
// The readback is deferred: leaving the target only issues the copy into the
// texture's own system-memory surface (and has D3D submit it); the surface is
// locked when the target is left again in a later frame, or at that frame's
// Present, when the GPU has long finished the copy. Locking it at once made
// D3D submit the frame's queued work and wait for the GPU to run it: ~3 ms
// per readback of which the copy itself is a tenth. The image drawn in frame
// N is read during frame N + 1 and shown by that frame, as it was before (it
// used to be read in frame N and applied after its Present).
// OPENREMIX_PROXY_TARGET_READ_DEFER=0 locks at once, as before.
namespace targetread {
constexpr uint32_t kReadSize = 256, kMaxSize = 1024, kWantedFrames = 30;
struct Live {
  bool scene = false, dirty = false;
  uint64_t wanted = 0; // g_d3dFrame of the last request
  // The copy issued and not yet read: into `system`, in frame `issued`.
  IDirect3DSurface9 *system = nullptr;
  uint32_t width = 0, height = 0;
  D3DFORMAT format = D3DFMT_UNKNOWN;
  bool pending = false;
  uint64_t issued = 0;
};
std::unordered_map<const void *, Live> g_live;
IDirect3DSurface9 *g_small = nullptr; // StretchRect destination, shared (copies are issued in order)
IDirect3DQuery9 *g_flush = nullptr;   // event query: has D3D submit its queued work without waiting
uint32_t g_width = 0, g_height = 0;
D3DFORMAT g_format = D3DFMT_UNKNOWN;
std::atomic<uint32_t> g_draws{0}, g_reads{0}, g_failures{0}, g_issues{0}, g_superseded{0};
std::atomic<uint64_t> g_issueUs{0}, g_collectUs{0};

void drop(Live &live) {
  if (live.system)
    live.system->Release();
  live.system = nullptr;
  live.pending = false;
  live.width = live.height = 0;
}
void release() {
  if (g_small)
    g_small->Release();
  if (g_flush)
    g_flush->Release();
  g_small = nullptr;
  g_flush = nullptr;
  g_width = g_height = 0;
  for (auto &[texture, live] : g_live)
    drop(live);
}
bool draw();
void leave(IDirect3DDevice9 *dev, IDirect3DSurface9 *surface);
void present();
} // namespace targetread

ULONG STDMETHODCALLTYPE textureRelease(IUnknown *self) {
  const ULONG refs = g_textureRelease(self);
  if (refs == 0) {
    if (auto live = targetread::g_live.find(self); live != targetread::g_live.end()) {
      targetread::drop(live->second);
      targetread::g_live.erase(live);
    }
    g_targetTextures.erase(self);
    if (g_renderTargetTexture == self)
      g_renderTargetTexture = nullptr;
  }
  if (refs == 0)
    remix::onTextureReleased(self);
  return refs;
}

HRESULT STDMETHODCALLTYPE textureLockRect(IDirect3DTexture9 *self, UINT level, D3DLOCKED_RECT *locked,
                                          const RECT *rect, DWORD flags) {
  const HRESULT hr = g_textureLockRect(self, level, locked, rect, flags);
  if (SUCCEEDED(hr) && locked && !(flags & D3DLOCK_READONLY)) {
    std::lock_guard<std::mutex> lock(g_textureLocks);
    g_lockedTextures[levelKey(self, level)] = {static_cast<const uint8_t *>(locked->pBits), uint32_t(locked->Pitch),
                                               rect != nullptr, rect ? *rect : RECT{}};
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE textureUnlockRect(IDirect3DTexture9 *self, UINT level) {
  TextureLock l;
  {
    std::lock_guard<std::mutex> lock(g_textureLocks);
    auto it = g_lockedTextures.find(levelKey(self, level));
    if (it != g_lockedTextures.end()) {
      l = it->second;
      g_lockedTextures.erase(it);
    }
  }
  if (level != 0) {
    // The levels above 0, whole: the runtime gets the chain the game wrote.
    D3DSURFACE_DESC desc, top;
    if (l.bits && SUCCEEDED(self->GetLevelDesc(level, &desc)) && SUCCEEDED(self->GetLevelDesc(0, &top)) &&
        (!l.hasRect || (l.rect.left == 0 && l.rect.top == 0 && UINT(l.rect.right) >= desc.Width &&
                        UINT(l.rect.bottom) >= desc.Height)))
      remix::onTextureLevel(self, level, top.Width, top.Height, desc.Width, desc.Height, desc.Format, l.bits, l.pitch,
                            self->GetLevelCount());
  } else {
    D3DSURFACE_DESC desc;
    if (l.bits && SUCCEEDED(self->GetLevelDesc(0, &desc))) {
      // The shader API locks whole levels with an explicit rect; partial
      // updates (e.g. lightmap pages) are not forwarded yet.
      const bool whole = !l.hasRect || (l.rect.left == 0 && l.rect.top == 0 && UINT(l.rect.right) >= desc.Width &&
                                        UINT(l.rect.bottom) >= desc.Height);
      if (whole)
        remix::onTextureUpload(self, desc.Width, desc.Height, desc.Format, l.bits, l.pitch, self->GetLevelCount());
      else {
        ++g_stats.partialTextureLocks;
        remix::onTexturePartialUpload(self, desc.Width, desc.Height, desc.Format, l.rect, l.bits, l.pitch);
      }
    }
  }
  return g_textureUnlockRect(self, level);
}

// Uploads through level surfaces (GetSurfaceLevel + IDirect3DSurface9::LockRect,
// as D3DX does): forward level 0 of the containing texture.
using SurfaceLockRectFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
using SurfaceUnlockRectFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DSurface9 *);
using GetSurfaceLevelFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DTexture9 *, UINT, IDirect3DSurface9 **);
SurfaceLockRectFn g_surfaceLockRect = nullptr;
SurfaceUnlockRectFn g_surfaceUnlockRect = nullptr;
GetSurfaceLevelFn g_getSurfaceLevel = nullptr;

// Returns the 2D texture whose level 0 is `surface` (not AddRef'd), or nullptr.
IDirect3DTexture9 *levelZeroTexture(IDirect3DSurface9 *surface) {
  IDirect3DTexture9 *texture = nullptr;
  if (FAILED(surface->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void **>(&texture))) || !texture)
    return nullptr;
  texture->Release(); // the texture outlives its surfaces' use here
  D3DSURFACE_DESC level0, mine;
  if (FAILED(texture->GetLevelDesc(0, &level0)) || FAILED(surface->GetDesc(&mine)) || level0.Width != mine.Width ||
      level0.Height != mine.Height)
    return nullptr;
  return texture;
}

HRESULT STDMETHODCALLTYPE surfaceLockRect(IDirect3DSurface9 *self, D3DLOCKED_RECT *locked, const RECT *rect,
                                          DWORD flags) {
  const HRESULT hr = g_surfaceLockRect(self, locked, rect, flags);
  if (SUCCEEDED(hr) && locked && !(flags & D3DLOCK_READONLY)) {
    std::lock_guard<std::mutex> lock(g_textureLocks);
    g_lockedTextures[self] = {static_cast<const uint8_t *>(locked->pBits), uint32_t(locked->Pitch), rect != nullptr,
                              rect ? *rect : RECT{}};
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE surfaceUnlockRect(IDirect3DSurface9 *self) {
  TextureLock l;
  {
    std::lock_guard<std::mutex> lock(g_textureLocks);
    auto it = g_lockedTextures.find(self);
    if (it != g_lockedTextures.end()) {
      l = it->second;
      g_lockedTextures.erase(it);
    }
  }
  D3DSURFACE_DESC desc;
  if (l.bits && SUCCEEDED(self->GetDesc(&desc))) {
    const bool whole = !l.hasRect || (l.rect.left == 0 && l.rect.top == 0 && UINT(l.rect.right) >= desc.Width &&
                                      UINT(l.rect.bottom) >= desc.Height);
    if (IDirect3DTexture9 *texture = levelZeroTexture(self)) {
      if (whole)
        remix::onTextureUpload(texture, desc.Width, desc.Height, desc.Format, l.bits, l.pitch,
                               texture->GetLevelCount());
      else {
        ++g_stats.partialTextureLocks;
        remix::onTexturePartialUpload(texture, desc.Width, desc.Height, desc.Format, l.rect, l.bits, l.pitch);
      }
    } else if (whole) {
      // A level above 0 of a mipmapped texture: the runtime gets the chain
      // the game wrote (remix::onTextureLevel).
      IDirect3DTexture9 *container = nullptr;
      if (SUCCEEDED(self->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void **>(&container))) && container) {
        container->Release(); // the texture outlives its surfaces' use here
        D3DSURFACE_DESC top;
        const UINT levels = container->GetLevelCount();
        if (SUCCEEDED(container->GetLevelDesc(0, &top)))
          for (UINT level = 1; level < levels; ++level)
            if (std::max(1u, top.Width >> level) == desc.Width && std::max(1u, top.Height >> level) == desc.Height) {
              remix::onTextureLevel(container, level, top.Width, top.Height, desc.Width, desc.Height, desc.Format,
                                    l.bits, l.pitch, levels);
              break;
            }
      }
    }
  }
  return g_surfaceUnlockRect(self);
}

HRESULT STDMETHODCALLTYPE textureGetSurfaceLevel(IDirect3DTexture9 *self, UINT level, IDirect3DSurface9 **out) {
  const HRESULT hr = g_getSurfaceLevel(self, level, out);
  if (SUCCEEDED(hr) && out && *out && !g_surfaceLockRect) {
    void **vt = vtableOf(*out);
    hookSlot(vt, kSurfaceLockRect, &surfaceLockRect, g_surfaceLockRect);
    hookSlot(vt, kSurfaceUnlockRect, &surfaceUnlockRect, g_surfaceUnlockRect);
  }
  return hr;
}

// --- Device state ----------------------------------------------------------
struct StreamState {
  IDirect3DVertexBuffer9 *buffer = nullptr;
  UINT offset = 0, stride = 0;
};
StreamState g_streams[4];
IDirect3DIndexBuffer9 *g_indices = nullptr;
IDirect3DVertexDeclaration9 *g_decl = nullptr;
std::unordered_map<IDirect3DVertexDeclaration9 *, std::vector<D3DVERTEXELEMENT9>> g_declElements;
float g_vsConstants[256 * 4] = {};
// Pixel shader registers c0..c223: the factors particle shaders apply per
// pixel (soft-particle depth scale), and every constant of a screen-space
// pass replayed by the runtime (host passes).
float g_psConstants[224 * 4] = {};
int32_t g_psConstantsI[16 * 4] = {}, g_vsConstantsI[16 * 4] = {};
uint32_t g_psBools = 0, g_vsBools = 0;
IDirect3DSurface9 *g_backBuffer = nullptr;
IDirect3DSurface9 *g_renderTarget0 = nullptr;
IDirect3DBaseTexture9 *g_texture0 = nullptr;
IDirect3DBaseTexture9 *g_texture1 = nullptr;
IDirect3DBaseTexture9 *g_textures[16] = {}; // every pixel sampler's texture
uint32_t g_textureTargets = 0;              // bit n: g_textures[n] is a render-target texture
DWORD g_samplerStates[16][16] = {};         // [sampler][D3DSAMPLERSTATETYPE]
D3DVIEWPORT9 g_viewport = {};
RECT g_scissor = {};
DWORD g_renderStates[256] = {};
// Vertex shaders decoded at creation (vsprog): the draws whose shader builds
// the geometry (particles) are run through it on the CPU. A shader object's
// address is reused after its release, by a shader created anew.
std::mutex g_vertexProgramLock;
std::unordered_map<IDirect3DVertexShader9 *, std::shared_ptr<const vsprog::Program>> g_vertexPrograms;
// Every shader's D3D9 bytecode, by its object (vertex and pixel; guarded by
// g_vertexProgramLock): screen-space passes are replayed on the runtime with
// the game's own shaders (dxso), and OPENREMIX_PROXY_DUMP_SHADERS=1 writes
// each to shaderapi_proxy_shaders\{vs,ps}_<hash>.bin at creation.
std::unordered_map<const void *, std::shared_ptr<const std::vector<uint32_t>>> g_shaderCode;
IDirect3DPixelShader9 *g_pixelShader = nullptr; // render thread
// This DLL's directory (the game's bin\\win64), for the dump.
const std::wstring &moduleDir() {
  static const std::wstring dir = [] {
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleDir), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring d = path;
    d.resize(d.find_last_of(L"\\/"));
    return d;
  }();
  return dir;
}
// The bytecode's length in DWORDs, as the created shader reports it
// (GetFunction). Not found by walking the tokens to the END token: shader
// model 1 gives no instruction lengths, and a def's float constants read as
// instructions then, so a walk could miss the END and run off the end of the
// game's buffer (it did, into unmapped stack, on Windows).
template <typename Shader> size_t shaderWords(Shader *shader) {
  UINT bytes = 0;
  if (!shader || FAILED(shader->GetFunction(nullptr, &bytes)) || bytes < 8 || bytes % 4)
    return 0;
  return bytes / 4;
}
template <typename Shader> void keepShaderCode(Shader *shader, const DWORD *function, const char *kind) {
  const size_t words = function ? shaderWords(shader) : 0;
  if (!words)
    return;
  auto code = std::make_shared<std::vector<uint32_t>>(function, function + words);
  static const bool dump = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_DUMP_SHADERS", v, sizeof(v)) && v[0] == '1';
  }();
  if (dump) {
    const std::wstring dir = moduleDir() + L"\\shaderapi_proxy_shaders";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, L"\\%hs_%016llx.bin", kind, (unsigned long long)proxy::dxvk::textureHash(reinterpret_cast<const uint8_t *>(code->data()), words * 4));
    if (FILE *f = _wfopen((dir + name).c_str(), L"wb")) {
      fwrite(code->data(), 4, words, f);
      fclose(f);
    }
  }
  std::lock_guard<std::mutex> lock(g_vertexProgramLock);
  g_shaderCode[shader] = std::move(code);
}
IDirect3DVertexShader9 *g_vertexShader = nullptr;
std::shared_ptr<const vsprog::Program> g_vertexProgram; // of g_vertexShader (nullptr: unknown)
std::atomic<uint32_t> g_vertexProgramsCreated{0};       // changes g_vertexProgram may have missed
uint32_t g_vertexProgramSeen = 0;

using CreateFn = IDirect3D9 *(WINAPI *)(UINT);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
using ResetFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND,
                                               const RGNDATA *);
using CreateTextureFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                     IDirect3DTexture9 **, HANDLE *);
using UpdateSurfaceFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *,
                                                     IDirect3DSurface9 *, const POINT *);
using UpdateTextureFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *,
                                                     IDirect3DBaseTexture9 *);
using CreateVertexBufferFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL,
                                                          IDirect3DVertexBuffer9 **, HANDLE *);
using CreateIndexBufferFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                         IDirect3DIndexBuffer9 **, HANDLE *);
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
using SetRenderStateFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
using SetTextureFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT,
                                                            UINT, UINT);
using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *,
                                                       UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                              const void *, D3DFORMAT, const void *, UINT);
using SetVertexDeclarationFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DVertexDeclaration9 *);
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const DWORD *, IDirect3DVertexShader9 **);
using SetVertexShaderFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DVertexShader9 *);
using SetVertexShaderConstantFFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, const float *, UINT);
using SetPixelShaderConstantFFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, const float *, UINT);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const DWORD *, IDirect3DPixelShader9 **);
using SetPixelShaderFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DPixelShader9 *);
using SetStreamSourceFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, IDirect3DVertexBuffer9 *, UINT,
                                                       UINT);
using SetIndicesFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DIndexBuffer9 *);
using CreateQueryFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DQUERYTYPE, IDirect3DQuery9 **);
using QueryIssueFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DQuery9 *, DWORD);
using QueryGetDataFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DQuery9 *, void *, DWORD, DWORD);

CreateFn g_create = nullptr;
CreateDeviceFn g_createDevice = nullptr;
ResetFn g_reset = nullptr;
PresentFn g_present = nullptr;
CreateTextureFn g_createTexture = nullptr;
UpdateTextureFn g_updateTexture = nullptr;
UpdateSurfaceFn g_updateSurface = nullptr;
CreateVertexBufferFn g_createVertexBuffer = nullptr;
CreateIndexBufferFn g_createIndexBuffer = nullptr;
SetRenderTargetFn g_setRenderTarget = nullptr;
SetTextureFn g_setTexture = nullptr;
SetRenderStateFn g_setRenderState = nullptr;
DrawPrimitiveFn g_drawPrimitive = nullptr;
DrawIndexedPrimitiveFn g_drawIndexedPrimitive = nullptr;
DrawPrimitiveUPFn g_drawPrimitiveUP = nullptr;
DrawIndexedPrimitiveUPFn g_drawIndexedPrimitiveUP = nullptr;
SetVertexDeclarationFn g_setVertexDeclaration = nullptr;
CreateVertexShaderFn g_createVertexShader = nullptr;
SetVertexShaderFn g_setVertexShader = nullptr;
SetVertexShaderConstantFFn g_setVertexShaderConstantF = nullptr;
SetPixelShaderConstantFFn g_setPixelShaderConstantF = nullptr;
CreatePixelShaderFn g_createPixelShader = nullptr;
SetPixelShaderFn g_setPixelShader = nullptr;
SetStreamSourceFn g_setStreamSource = nullptr;
SetIndicesFn g_setIndices = nullptr;
CreateQueryFn g_createQuery = nullptr;
using ClearFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
ClearFn g_clear = nullptr;
using StretchRectFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *,
                                                   IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
using SetViewportFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
using SetScissorRectFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *);
using SetSamplerStateFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using SetShaderConstantIFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, const int *, UINT);
using SetShaderConstantBFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, const BOOL *, UINT);
StretchRectFn g_stretchRect = nullptr;
SetViewportFn g_setViewport = nullptr;
SetScissorRectFn g_setScissorRect = nullptr;
SetSamplerStateFn g_setSamplerState = nullptr;
SetShaderConstantIFn g_setVertexShaderConstantI = nullptr, g_setPixelShaderConstantI = nullptr;
SetShaderConstantBFn g_setVertexShaderConstantB = nullptr, g_setPixelShaderConstantB = nullptr;
QueryIssueFn g_queryIssue = nullptr;
QueryGetDataFn g_queryGetData = nullptr;
// Occlusion queries between Issue(BEGIN) and Issue(END).
std::unordered_set<IDirect3DQuery9 *> g_openOcclusion;
// Render targets the game issued occlusion queries on, by the frame it last
// did: only their depth-writing draws are rasterized (see
// drawIndexedPrimitive). Shadow, reflection and monitor views, and frames
// without queries, then cost D3D9 nothing.
std::unordered_map<IDirect3DSurface9 *, uint64_t> g_queryTargets;
uint64_t g_d3dFrame = 0;
constexpr uint64_t kQueryTargetFrames = 60;

// Occluders: with the BSP world loaded, the map's opaque faces and static
// props (bsp::occluders) are rasterized into the depth buffer once per frame
// and render target, before its first occlusion query, in a few
// fixed-function draws; the engine's own depth-writing draws (one per
// object, ~1000 a frame) are then skipped. Dynamic models no longer occlude
// queries. OPENREMIX_PROXY_D3D_OCCLUDERS=0 keeps the engine's depth draws.
struct OccluderBuffers {
  IDirect3DDevice9 *device = nullptr;
  IDirect3DVertexBuffer9 *vb = nullptr;
  IDirect3DIndexBuffer9 *ib = nullptr;
  uint32_t vertices = 0, triangles = 0;
  uint64_t version = 0;
  bool failed = false;
} g_occluders;
std::unordered_map<IDirect3DSurface9 *, uint64_t> g_occludersDrawn; // by target: frame

bool occludersEnabled() {
  static const bool enabled = [] {
    char v[8] = {};
    const bool occluders = !(GetEnvironmentVariableA("OPENREMIX_PROXY_D3D_OCCLUDERS", v, sizeof(v)) && v[0] == '0');
    return occluders && !(GetEnvironmentVariableA("OPENREMIX_PROXY_D3D_DEPTH", v, sizeof(v)) && v[0] == '0');
  }();
  return enabled;
}

namespace skycap {
void finish(IDirect3DDevice9 *dev);
void release();
} // namespace skycap

void refreshBackBuffer(IDirect3DDevice9 *dev) {
  IDirect3DSurface9 *bb = nullptr;
  if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
    g_backBuffer = bb; // identity only; the device keeps it alive
    bb->Release();
  }
  g_renderTarget0 = g_backBuffer;
}

HRESULT STDMETHODCALLTYPE present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND wnd,
                                  const RGNDATA *dirty) {
  ++g_stats.frames;
  targetread::present(); // copies issued in earlier frames; before the frame number changes
  ++g_d3dFrame;
  const uint64_t t0 = nowUs();
  // The runtime presents into the game window; D3D9's own presentation there
  // only fights it (DXVK recreated its swapchain every frame, ~10% of the
  // render thread). Its queued work (depth, occlusion queries) is still
  // flushed by the queries. OPENREMIX_PROXY_D3D_PRESENT=1 presents anyway.
  static const bool presentD3D = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_D3D_PRESENT", v, sizeof(v)) && v[0] == '1';
  }();
  const HRESULT hr = presentD3D || !remix::enabled() ? g_present(dev, src, dst, wnd, dirty) : D3D_OK;
  g_stats.usPresent += nowUs() - t0;
  skycap::finish(dev);
  engine::install();
  remix::onPresent();
  return hr;
}

HRESULT STDMETHODCALLTYPE reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp) {
  skycap::release(); // D3DPOOL_DEFAULT targets must go before Reset
  targetread::release();
  remix::onCreateDevice(nullptr, pp);
  const HRESULT hr = g_reset(dev, pp);
  if (SUCCEEDED(hr))
    refreshBackBuffer(dev);
  return hr;
}

void trackBuffer(const void *buffer, UINT length, DWORD usage, bool is32, D3DPOOL pool) {
  std::lock_guard<std::mutex> lock(g_shadowLock);
  auto &s = g_shadows[buffer];
  s = Shadow();
  s.data.resize(length);
  s.dynamic = (usage & D3DUSAGE_DYNAMIC) != 0;
  s.is32 = is32;
  s.usage = usage;
  s.pool = pool;
}

HRESULT STDMETHODCALLTYPE createVertexBuffer(IDirect3DDevice9 *dev, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool,
                                             IDirect3DVertexBuffer9 **out, HANDLE *shared) {
  const HRESULT hr = g_createVertexBuffer(dev, length, usage, fvf, pool, out, shared);
  if (SUCCEEDED(hr) && out && *out) {
    if (!g_vbLock) {
      void **vt = vtableOf(*out);
      hookSlot(vt, kRelease, &vbRelease, g_vbRelease);
      hookSlot(vt, kBufferLock, &vbLock, g_vbLock);
      hookSlot(vt, kBufferUnlock, &vbUnlock, g_vbUnlock);
    }
    trackBuffer(*out, length, usage, false, pool);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE createIndexBuffer(IDirect3DDevice9 *dev, UINT length, DWORD usage, D3DFORMAT format,
                                            D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *shared) {
  const HRESULT hr = g_createIndexBuffer(dev, length, usage, format, pool, out, shared);
  if (SUCCEEDED(hr) && out && *out) {
    if (!g_ibLock) {
      void **vt = vtableOf(*out);
      hookSlot(vt, kRelease, &ibRelease, g_ibRelease);
      hookSlot(vt, kBufferLock, &ibLock, g_ibLock);
      hookSlot(vt, kBufferUnlock, &ibUnlock, g_ibUnlock);
    }
    trackBuffer(*out, length, usage, format == D3DFMT_INDEX32, pool);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE createTexture(IDirect3DDevice9 *dev, UINT width, UINT height, UINT levels, DWORD usage,
                                        D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared) {
  const HRESULT hr = g_createTexture(dev, width, height, levels, usage, format, pool, out, shared);
  if (SUCCEEDED(hr) && out && *out && (usage & D3DUSAGE_RENDERTARGET))
    g_targetTextures[*out] = {width, height, format};
  if (SUCCEEDED(hr) && out && *out && !g_textureLockRect) {
    void **vt = vtableOf(*out);
    hookSlot(vt, kRelease, &textureRelease, g_textureRelease);
    hookSlot(vt, kTextureGetSurfaceLevel, &textureGetSurfaceLevel, g_getSurfaceLevel);
    hookSlot(vt, kTextureLockRect, &textureLockRect, g_textureLockRect);
    hookSlot(vt, kTextureUnlockRect, &textureUnlockRect, g_textureUnlockRect);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE updateSurface(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const RECT *srcRect,
                                        IDirect3DSurface9 *dst, const POINT *dstPoint) {
  if (!srcRect && !dstPoint && src && dst) {
    IDirect3DTexture9 *from = levelZeroTexture(src), *to = levelZeroTexture(dst);
    if (from && to)
      remix::onTextureCopy(from, to);
  }
  return g_updateSurface(dev, src, srcRect, dst, dstPoint);
}

HRESULT STDMETHODCALLTYPE updateTexture(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) {
  remix::onTextureCopy(src, dst);
  return g_updateTexture(dev, src, dst);
}

HRESULT STDMETHODCALLTYPE setRenderTarget(IDirect3DDevice9 *dev, DWORD index, IDirect3DSurface9 *surface) {
  ++g_stats.rtChanges;
  if (index == 0) {
    if (g_renderTargetTexture && surface != g_renderTarget0)
      targetread::leave(dev, g_renderTarget0);
    g_renderTarget0 = surface;
    // The render-target texture this surface belongs to, if any.
    g_renderTargetTexture = nullptr;
    IDirect3DTexture9 *container = nullptr;
    if (surface && surface != g_backBuffer && !g_targetTextures.empty() &&
        SUCCEEDED(surface->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void **>(&container))) && container) {
      container->Release();
      if (g_targetTextures.count(container))
        g_renderTargetTexture = container;
    }
  }
  return g_setRenderTarget(dev, index, surface);
}

HRESULT STDMETHODCALLTYPE setRenderState(IDirect3DDevice9 *dev, D3DRENDERSTATETYPE state, DWORD value) {
  if (unsigned(state) < 256)
    g_renderStates[state] = value;
  return g_setRenderState(dev, state, value);
}

HRESULT STDMETHODCALLTYPE setTexture(IDirect3DDevice9 *dev, DWORD stage, IDirect3DBaseTexture9 *texture) {
  ++g_stats.textureBinds;
  if (stage == 0)
    g_texture0 = texture;
  else if (stage == 1)
    g_texture1 = texture;
  if (stage < 16) {
    g_textures[stage] = texture;
    const bool target = texture && !g_targetTextures.empty() && g_targetTextures.count(texture) != 0;
    g_textureTargets = (g_textureTargets & ~(1u << stage)) | (target ? 1u << stage : 0u);
  }

  return g_setTexture(dev, stage, texture);
}

HRESULT STDMETHODCALLTYPE setVertexDeclaration(IDirect3DDevice9 *dev, IDirect3DVertexDeclaration9 *decl) {
  g_decl = decl;
  if (decl && !g_declElements.count(decl)) {
    D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1];
    UINT n = 0;
    if (SUCCEEDED(decl->GetDeclaration(elems, &n)))
      g_declElements[decl].assign(elems, elems + n);
  }
  return g_setVertexDeclaration(dev, decl);
}

HRESULT STDMETHODCALLTYPE createVertexShader(IDirect3DDevice9 *dev, const DWORD *function,
                                             IDirect3DVertexShader9 **out) {
  const HRESULT hr = g_createVertexShader(dev, function, out);
  if (SUCCEEDED(hr) && out && *out && function) {
    static_assert(sizeof(DWORD) == sizeof(uint32_t));
    auto program = vsprog::compile(reinterpret_cast<const uint32_t *>(function));
    std::lock_guard<std::mutex> lock(g_vertexProgramLock);
    g_stats.vertexPrograms += program != nullptr;
    g_vertexPrograms[*out] = std::move(program);
    ++g_vertexProgramsCreated;
  }
  if (SUCCEEDED(hr) && out && *out)
    keepShaderCode(*out, function, "vs");
  return hr;
}

HRESULT STDMETHODCALLTYPE createPixelShader(IDirect3DDevice9 *dev, const DWORD *function, IDirect3DPixelShader9 **out) {
  const HRESULT hr = g_createPixelShader(dev, function, out);
  if (SUCCEEDED(hr) && out && *out)
    keepShaderCode(*out, function, "ps");
  return hr;
}

HRESULT STDMETHODCALLTYPE setPixelShader(IDirect3DDevice9 *dev, IDirect3DPixelShader9 *shader) {
  g_pixelShader = shader;
  return g_setPixelShader(dev, shader);
}

HRESULT STDMETHODCALLTYPE setVertexShader(IDirect3DDevice9 *dev, IDirect3DVertexShader9 *shader) {
  if (const uint32_t created = g_vertexProgramsCreated.load(std::memory_order_relaxed);
      shader != g_vertexShader || created != g_vertexProgramSeen) {
    g_vertexProgramSeen = created;
    std::lock_guard<std::mutex> lock(g_vertexProgramLock);
    const auto found = shader ? g_vertexPrograms.find(shader) : g_vertexPrograms.end();
    g_vertexProgram = found == g_vertexPrograms.end() ? nullptr : found->second;
    g_vertexShader = shader;
  }
  return g_setVertexShader(dev, shader);
}

HRESULT STDMETHODCALLTYPE setVertexShaderConstantF(IDirect3DDevice9 *dev, UINT start, const float *data, UINT count) {
  ++g_stats.vsConstCalls;
  if (data && start < 256)
    std::memcpy(&g_vsConstants[start * 4], data, std::min(count, 256 - start) * 16);
  return g_setVertexShaderConstantF(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE setPixelShaderConstantF(IDirect3DDevice9 *dev, UINT start, const float *data, UINT count) {
  if (data && start < 224)
    std::memcpy(&g_psConstants[start * 4], data, std::min(count, 224 - start) * 16);
  return g_setPixelShaderConstantF(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE setPixelShaderConstantI(IDirect3DDevice9 *dev, UINT start, const int *data, UINT count) {
  if (data && start < 16)
    std::memcpy(&g_psConstantsI[start * 4], data, std::min(count, 16 - start) * 16);
  return g_setPixelShaderConstantI(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE setVertexShaderConstantI(IDirect3DDevice9 *dev, UINT start, const int *data, UINT count) {
  if (data && start < 16)
    std::memcpy(&g_vsConstantsI[start * 4], data, std::min(count, 16 - start) * 16);
  return g_setVertexShaderConstantI(dev, start, data, count);
}

void setBools(uint32_t &bools, UINT start, const BOOL *data, UINT count) {
  for (UINT i = 0; data && start + i < 16 && i < count; ++i)
    bools = (bools & ~(1u << (start + i))) | (data[i] ? 1u << (start + i) : 0u);
}

HRESULT STDMETHODCALLTYPE setPixelShaderConstantB(IDirect3DDevice9 *dev, UINT start, const BOOL *data, UINT count) {
  setBools(g_psBools, start, data, count);
  return g_setPixelShaderConstantB(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE setVertexShaderConstantB(IDirect3DDevice9 *dev, UINT start, const BOOL *data, UINT count) {
  setBools(g_vsBools, start, data, count);
  return g_setVertexShaderConstantB(dev, start, data, count);
}

HRESULT STDMETHODCALLTYPE setSamplerState(IDirect3DDevice9 *dev, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
  if (sampler < 16 && unsigned(type) < 16)
    g_samplerStates[sampler][type] = value;
  return g_setSamplerState(dev, sampler, type, value);
}

HRESULT STDMETHODCALLTYPE setViewport(IDirect3DDevice9 *dev, const D3DVIEWPORT9 *viewport) {
  if (viewport)
    g_viewport = *viewport;
  return g_setViewport(dev, viewport);
}

HRESULT STDMETHODCALLTYPE setScissorRect(IDirect3DDevice9 *dev, const RECT *rect) {
  if (rect)
    g_scissor = *rect;
  return g_setScissorRect(dev, rect);
}

// The back buffer (nullptr) or a render-target texture a surface belongs to;
// false for any other surface.
bool copySurface(IDirect3DSurface9 *surface, const void *&texture) {
  texture = nullptr;
  if (surface == g_backBuffer)
    return surface != nullptr;
  IDirect3DTexture9 *container = levelZeroTexture(surface);
  if (!container || !g_targetTextures.count(container))
    return false;
  texture = container;
  return true;
}

HRESULT STDMETHODCALLTYPE stretchRect(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const RECT *srcRect,
                                      IDirect3DSurface9 *dst, const RECT *dstRect, D3DTEXTUREFILTERTYPE filter) {
  const void *from = nullptr, *to = nullptr;
  if (copySurface(src, from) && copySurface(dst, to) && from != to)
    hostpass::recordCopy(from, srcRect, to, dstRect, filter != D3DTEXF_POINT && filter != D3DTEXF_NONE);
  return g_stretchRect(dev, src, srcRect, dst, dstRect, filter);
}

HRESULT STDMETHODCALLTYPE setStreamSource(IDirect3DDevice9 *dev, UINT stream, IDirect3DVertexBuffer9 *vb, UINT offset,
                                          UINT stride) {
  ++g_stats.streamSets;
  if (stream < 4)
    g_streams[stream] = {vb, offset, stride};
  return g_setStreamSource(dev, stream, vb, offset, stride);
}

HRESULT STDMETHODCALLTYPE setIndices(IDirect3DDevice9 *dev, IDirect3DIndexBuffer9 *ib) {
  g_indices = ib;
  return g_setIndices(dev, ib);
}

HRESULT STDMETHODCALLTYPE drawPrimitive(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT start, UINT count) {
  ++g_stats.dp;
  return g_drawPrimitive(dev, type, start, count);
}

void releaseOccluders() {
  if (g_occluders.vb)
    g_occluders.vb->Release();
  if (g_occluders.ib)
    g_occluders.ib->Release();
  g_occluders = OccluderBuffers{};
  g_occludersDrawn.clear();
}

// The current map's occluder buffers on this device (created on first use).
bool occludersReady(IDirect3DDevice9 *dev) {
  if (!occludersEnabled() || !bsp::active() || !g_vbLock || !g_ibLock)
    return false;
  const auto &o = bsp::occluders();
  if (o.indices.empty())
    return false;
  if (g_occluders.device == dev && g_occluders.version == o.version)
    return !g_occluders.failed;
  releaseOccluders();
  g_occluders.device = dev;
  g_occluders.version = o.version;
  g_occluders.failed = true;
  const UINT vbBytes = UINT(o.positions.size() * sizeof(float)), ibBytes = UINT(o.indices.size() * 4);
  // MANAGED: survives device resets. Created and filled through the real
  // entry points, so the proxy's buffer tracking never sees them.
  if (FAILED(g_createVertexBuffer(dev, vbBytes, D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_MANAGED, &g_occluders.vb,
                                  nullptr)) ||
      FAILED(g_createIndexBuffer(dev, ibBytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_MANAGED, &g_occluders.ib,
                                 nullptr)))
    return false;
  void *data = nullptr;
  if (FAILED(g_vbLock(g_occluders.vb, 0, 0, &data, 0)))
    return false;
  std::memcpy(data, o.positions.data(), vbBytes);
  g_vbUnlock(g_occluders.vb);
  if (FAILED(g_ibLock(g_occluders.ib, 0, 0, &data, 0)))
    return false;
  std::memcpy(data, o.indices.data(), ibBytes);
  g_ibUnlock(g_occluders.ib);
  g_occluders.vertices = UINT(o.positions.size() / 3);
  g_occluders.triangles = UINT(o.indices.size() / 3);
  g_occluders.failed = false;
  return true;
}

// Rasterizes the occluders' depth with the current view (vertex shader
// constants c8..c11: the view-projection, one register per clip-space
// component) and viewport; every other state is restored.
void drawOccluders(IDirect3DDevice9 *dev) {
  IDirect3DStateBlock9 *saved = nullptr;
  if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)))
    return;
  D3DMATRIX projection = {}, identity = {};
  for (int r = 0; r < 4; ++r) {
    identity.m[r][r] = 1;
    for (int c = 0; c < 4; ++c)
      projection.m[r][c] = g_vsConstants[(8 + c) * 4 + r];
  }
  g_setVertexShader(dev, nullptr); // past the hook: the state block restores the shader, and its tracking would not see that
  dev->SetPixelShader(nullptr);
  dev->SetFVF(D3DFVF_XYZ);
  dev->SetTransform(D3DTS_WORLD, &identity);
  dev->SetTransform(D3DTS_VIEW, &identity);
  dev->SetTransform(D3DTS_PROJECTION, &projection);
  static const std::pair<D3DRENDERSTATETYPE, DWORD> states[] = {
      {D3DRS_ZENABLE, D3DZB_TRUE}, {D3DRS_ZWRITEENABLE, TRUE}, {D3DRS_ZFUNC, D3DCMP_LESSEQUAL},
      {D3DRS_COLORWRITEENABLE, 0}, {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_ALPHABLENDENABLE, FALSE},
      {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE}, {D3DRS_LIGHTING, FALSE},
      {D3DRS_FOGENABLE, FALSE}, {D3DRS_CLIPPING, TRUE}, {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_DEPTHBIAS, 0},
      {D3DRS_SLOPESCALEDEPTHBIAS, 0}, {D3DRS_SCISSORTESTENABLE, FALSE}, {D3DRS_FILLMODE, D3DFILL_SOLID}};
  for (auto [state, value] : states)
    g_setRenderState(dev, state, value);
  g_setStreamSource(dev, 0, g_occluders.vb, 0, 12);
  g_setIndices(dev, g_occluders.ib);
  // In batches the device's primitive limit allows.
  D3DCAPS9 caps = {};
  dev->GetDeviceCaps(&caps);
  const UINT batch = caps.MaxPrimitiveCount ? caps.MaxPrimitiveCount : 65535;
  for (UINT first = 0; first < g_occluders.triangles; first += batch)
    g_drawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, g_occluders.vertices, first * 3,
                           std::min(batch, g_occluders.triangles - first));
  saved->Apply();
  saved->Release();
  ++g_stats.occluderDraws;
}

HRESULT STDMETHODCALLTYPE queryGetData(IDirect3DQuery9 *query, void *data, DWORD size, DWORD flags) {
  const HRESULT hr = g_queryGetData(query, data, size, flags);
  if (hr == S_OK && data && size >= 4 && query->GetType() == D3DQUERYTYPE_OCCLUSION)
    ++(*static_cast<const DWORD *>(data) ? g_stats.queriesVisible : g_stats.queriesHidden);
  return hr;
}

HRESULT STDMETHODCALLTYPE queryIssue(IDirect3DQuery9 *query, DWORD flags) {
  if (query->GetType() == D3DQUERYTYPE_OCCLUSION) {
    if (flags & D3DISSUE_BEGIN) {
      // The first query of the frame on this target: its depth, unless
      // another query is open (the occluders would count towards it).
      IDirect3DDevice9 *dev = nullptr;
      if (g_openOcclusion.empty() && SUCCEEDED(query->GetDevice(&dev))) {
        uint64_t &drawn = g_occludersDrawn[g_renderTarget0];
        if (drawn != g_d3dFrame && occludersReady(dev)) {
          drawn = g_d3dFrame;
          drawOccluders(dev);
        }
        dev->Release();
      }
      g_openOcclusion.insert(query);
      g_queryTargets[g_renderTarget0] = g_d3dFrame;
      ++g_stats.occlusionQueries;
    }
    else if (flags & D3DISSUE_END)
      g_openOcclusion.erase(query);
  }
  return g_queryIssue(query, flags);
}

// A depth clear takes the occluders with it: the engine clears depth between
// the views of a frame (3D skybox, then the main view), so occluders drawn
// for a query of the skybox view were gone, and never redrawn, by the time
// the main view tested its glows and the sun against them. The next query on
// this target draws them again, with that view's matrices.
HRESULT STDMETHODCALLTYPE clear(IDirect3DDevice9 *dev, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color,
                                float z, DWORD stencil) {
  if ((flags & D3DCLEAR_TARGET) && count == 0 && g_renderTargetTexture)
    hostpass::recordClear(g_renderTargetTexture, color);
  if (flags & D3DCLEAR_ZBUFFER) {
    auto it = g_occludersDrawn.find(g_renderTarget0);
    if (it != g_occludersDrawn.end() && it->second == g_d3dFrame) {
      g_occludersDrawn.erase(it);
      ++g_stats.occluderClears;
    }
  }
  return g_clear(dev, count, rects, flags, color, z, stencil);
}

HRESULT STDMETHODCALLTYPE createQuery(IDirect3DDevice9 *dev, D3DQUERYTYPE type, IDirect3DQuery9 **out) {
  const HRESULT hr = g_createQuery(dev, type, out);
  if (SUCCEEDED(hr) && out && *out && type == D3DQUERYTYPE_OCCLUSION && !g_queryIssue) {
    hookSlot(vtableOf(*out), kQueryIssue, &queryIssue, g_queryIssue);
    hookSlot(vtableOf(*out), kQueryGetData, &queryGetData, g_queryGetData);
  }
  return hr;
}

// Hands the draw to the Remix output; returns whether it is a 2D skybox
// draw instead (captured into the sky faces, not submitted).
bool captureDraw(D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices, UINT startIndex,
                 UINT primCount) {
  auto decl = g_declElements.find(g_decl);
  if (decl == g_declElements.end())
    return false;
  remix::Draw d;
  d.type = type;
  d.baseVertex = baseVertex;
  d.minIndex = minIndex;
  d.numVertices = numVertices;
  d.startIndex = startIndex;
  d.primCount = primCount;
  d.vsConstants = g_vsConstants;
  d.psConstants = g_psConstants;
  d.vertexShader = g_vertexShader;
  d.pixelShader = g_pixelShader;
  d.vertexProgram = g_vertexProgram.get();
  d.mainTarget = g_renderTarget0 == g_backBuffer;
  d.texture0 = g_texture0;
  d.texture1 = g_texture1;
  for (int s = 0; s < 8; ++s)
    d.stageTextures[s] = g_textures[s];
  d.colorWrite = g_renderStates[D3DRS_COLORWRITEENABLE];
  d.blend = g_renderStates[D3DRS_ALPHABLENDENABLE] != 0;
  d.srcBlend = g_renderStates[D3DRS_SRCBLEND];
  d.dstBlend = g_renderStates[D3DRS_DESTBLEND];
  d.blendOp = g_renderStates[D3DRS_BLENDOP];
  d.separateAlpha = g_renderStates[D3DRS_SEPARATEALPHABLENDENABLE] != 0;
  d.srcBlendAlpha = g_renderStates[D3DRS_SRCBLENDALPHA];
  d.dstBlendAlpha = g_renderStates[D3DRS_DESTBLENDALPHA];
  d.blendOpAlpha = g_renderStates[D3DRS_BLENDOPALPHA];
  d.alphaTest = g_renderStates[D3DRS_ALPHATESTENABLE] != 0;
  d.alphaRef = g_renderStates[D3DRS_ALPHAREF];
  d.alphaFunc = g_renderStates[D3DRS_ALPHAFUNC];
  d.depthBias = g_renderStates[D3DRS_DEPTHBIAS] != 0 || g_renderStates[D3DRS_SLOPESCALEDEPTHBIAS] != 0;
  d.cullMode = g_renderStates[D3DRS_CULLMODE];
  d.stencil = g_renderStates[D3DRS_STENCILENABLE] != 0;
  d.stencilFunc = g_renderStates[D3DRS_STENCILFUNC];
  d.stencilPass = g_renderStates[D3DRS_STENCILPASS];
  d.depthTest = g_renderStates[D3DRS_ZENABLE] != D3DZB_FALSE && g_renderStates[D3DRS_ZFUNC] != D3DCMP_ALWAYS;
  d.stencilRef = g_renderStates[D3DRS_STENCILREF];
  d.stencilMask = g_renderStates[D3DRS_STENCILMASK];
  d.stencilWriteMask = g_renderStates[D3DRS_STENCILWRITEMASK];
  d.stencilFail = g_renderStates[D3DRS_STENCILFAIL];
  d.stencilZFail = g_renderStates[D3DRS_STENCILZFAIL];
  d.zFunc = g_renderStates[D3DRS_ZFUNC];
  d.fillMode = g_renderStates[D3DRS_FILLMODE];
  d.zEnable = g_renderStates[D3DRS_ZENABLE] != D3DZB_FALSE;
  d.zWrite = g_renderStates[D3DRS_ZWRITEENABLE] != 0;
  if (g_renderTargetTexture)
    if (auto it = g_targetTextures.find(g_renderTargetTexture); it != g_targetTextures.end()) {
      d.target = g_renderTargetTexture;
      d.targetWidth = it->second.width;
      d.targetHeight = it->second.height;
      d.targetFormat = it->second.format;
    }
  d.texture0Target = g_texture0 && g_targetTextures.count(g_texture0) != 0;
  d.textures = reinterpret_cast<const void *const *>(g_textures);
  d.textureTargets = g_textureTargets;
  d.samplerStates = g_samplerStates;
  d.psConstantsI = g_psConstantsI;
  d.vsConstantsI = g_vsConstantsI;
  d.psBools = g_psBools;
  d.vsBools = g_vsBools;
  d.viewport = g_viewport;
  d.scissor = g_scissor;
  d.scissorTest = g_renderStates[D3DRS_SCISSORTESTENABLE] != 0;
  d.forkCategories = g_renderStates[42]; // unused by D3D9; dxvk-remix fork category tag
  d.elements = decl->second.data();
  d.declaration = g_decl;
  // The shadow lock stays held while the mesh is built from the shadows.
  std::lock_guard<std::mutex> lock(g_shadowLock);
  for (int i = 0; i < 4; ++i) {
    auto it = g_streams[i].buffer ? g_shadows.find(g_streams[i].buffer) : g_shadows.end();
    if (it == g_shadows.end())
      continue;
    auto &s = d.streams[i];
    s.data = it->second.data.data();
    s.size = uint32_t(it->second.data.size());
    s.offset = g_streams[i].offset;
    s.stride = g_streams[i].stride;
    s.buffer = g_streams[i].buffer;
    s.version = it->second.version;
    s.dynamic = it->second.dynamic;
  }
  auto it = g_indices ? g_shadows.find(g_indices) : g_shadows.end();
  if (it != g_shadows.end())
    d.indices = {it->second.data.data(), uint32_t(it->second.data.size()), it->second.is32, g_indices,
                 it->second.version, it->second.dynamic};
  if (remix::isSkyDraw(d))
    return true;
  remix::onDraw(d);
  return false;
}

// --- 2D skybox capture -----------------------------------------------------
// GMod's 2D skybox (painted or textured) is drawn by its own shaders. Now
// and then (due) its draws (dynamic-buffer draws inside DrawWorldLists,
// remix::isSkyDraw) are rasterized again into six world-aligned cube faces
// around the camera, with the game's shaders and constants and only the
// matrix registers replaced; the faces are read back at the Presents after
// it, one a frame, and become the Remix dome (remix::setSkyFaces) when they
// differ from the one there is.
// The faces accumulate: they are cleared for a new map only, so a face the
// engine does not draw in some frame (it may skip those behind the camera)
// keeps what an earlier capture put there.
// A capture reads 6 * kFaceSize^2 * 4 bytes back (6 MiB) and, when the sky
// changed, resamples (on another thread) and uploads the dome, so captures are spaced: from
// kSkyInterval frames, doubling up to kSkyMaxInterval while the sky stays the
// same, and never costing more than 1 / kSkyCostFactor of the time. A sky
// texture not seen since the map began (a face coming into view) captures
// kSkyFreshInterval frames after the last capture; only the first
// kSkyFreshTextures count (a sky that binds no texture of its own would
// show whatever the draw before it left bound).
namespace skycap {
constexpr uint64_t kSkyInterval = 30, kSkyMaxInterval = 240, kSkyCostFactor = 50, kSkyFreshInterval = 4;
constexpr size_t kSkyFreshTextures = 12;
struct Faces {
  IDirect3DDevice9 *device = nullptr;
  IDirect3DSurface9 *targets[6] = {}, *readback[6] = {};
  bool failed = false, captured = false;
  int reading = -1;                 // the face read back at this Present (-1: none)
  uint64_t readUs = 0;              // spent on the capture being read
  bool reset = true;                // the targets hold nothing of this map's sky
  uint32_t epoch = 0;               // remix::skyEpoch() the faces belong to
  uint64_t captureFrame = ~0ull, lastCapture = 0, interval = kSkyInterval, notBeforeUs = 0;
  std::unordered_set<const void *> seen; // base textures captured since the reset
  std::vector<uint8_t> pixels[6];
} g;

void release() {
  for (int f = 0; f < 6; ++f) {
    if (g.targets[f])
      g.targets[f]->Release();
    if (g.readback[f])
      g.readback[f]->Release();
    g.targets[f] = g.readback[f] = nullptr;
  }
  g.device = nullptr;
  g.captured = false;
  g.reading = -1;
  g.reset = true; // new targets start undefined
}

bool ready(IDirect3DDevice9 *dev) {
  if (g.device == dev && g.targets[0])
    return true;
  if (g.failed)
    return false;
  release();
  for (int f = 0; f < 6; ++f) {
    if (FAILED(dev->CreateRenderTarget(sky::kFaceSize, sky::kFaceSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                       &g.targets[f], nullptr)) ||
        FAILED(dev->CreateOffscreenPlainSurface(sky::kFaceSize, sky::kFaceSize, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                &g.readback[f], nullptr))) {
      release();
      g.failed = true;
      return false;
    }
  }
  g.device = dev;
  return true;
}

// Row-major 4x4, row vectors (D3DX): out = a * b.
void multiply(const float *a, const float *b, float *out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float v = 0;
      for (int k = 0; k < 4; ++k)
        v += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = v;
    }
}

bool invert(const float *m, float *out) {
  float a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c)
      a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.f : 0.f);
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
        pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-12f)
      return false;
    for (int k = 0; k < 8; ++k)
      std::swap(a[c][k], a[pivot][k]);
    const float inv = 1 / a[c][c];
    for (int k = 0; k < 8; ++k)
      a[c][k] *= inv;
    for (int r = 0; r < 4; ++r)
      if (r != c && a[r][c] != 0) {
        const float f = a[r][c];
        for (int k = 0; k < 8; ++k)
          a[r][k] -= f * a[c][k];
      }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      out[r * 4 + c] = a[r][c + 4];
  return true;
}

// Right-handed D3DX view * projection for a face around `eye` (90 degrees).
// D3D9 samples a pixel at its top-left corner in these terms: the half-pixel
// shift (x - w / size, y + w / size; w = -z) puts texel centres at
// half-integers, as the resampling assumes, so adjacent faces meet. Nothing
// is depth-tested: the planes only have to contain the sky geometry (a box
// of half-width zFar / sqrt(3) around the eye).
void faceViewProjection(int face, const float eye[3], float *out) {
  float right[3], up[3], forward[3];
  sky::faceBasis(face, right, up, forward);
  float view[16] = {};
  for (int i = 0; i < 3; ++i) {
    view[i * 4 + 0] = right[i];
    view[i * 4 + 1] = up[i];
    view[i * 4 + 2] = -forward[i];
  }
  for (int c = 0; c < 3; ++c)
    view[12 + c] = -(eye[0] * view[c] + eye[1] * view[4 + c] + eye[2] * view[8 + c]);
  view[15] = 1;
  constexpr float n = 0.5f, f = 1e6f, h = 1.0f / sky::kFaceSize;
  const float projection[16] = {1, 0, 0, 0, 0, 1, 0, 0, h, -h, f / (n - f), -1, 0, 0, n * f / (n - f), 0};
  multiply(view, projection, out);
}

// Register block (c[k] = column k of the row-vector matrix) <-> matrix.
void fromRegisters(const float *regs, float *m) {
  for (int k = 0; k < 4; ++k)
    for (int j = 0; j < 4; ++j)
      m[j * 4 + k] = regs[k * 4 + j];
}
void toRegisters(const float *m, float *regs) {
  for (int k = 0; k < 4; ++k)
    for (int j = 0; j < 4; ++j)
      regs[k * 4 + j] = m[j * 4 + k];
}

// `fresh`: the draw's base texture was not captured since the reset.
bool due(bool fresh) {
  if (g.captureFrame == g_d3dFrame || g.reset)
    return true; // the frame's other sky draws; a new map
  if (fresh && g_d3dFrame - g.lastCapture >= kSkyFreshInterval)
    return true; // a new face
  return g_d3dFrame - g.lastCapture >= g.interval && nowUs() >= g.notBeforeUs;
}

void capture(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices,
             UINT startIndex, UINT primCount) {
  ++g_stats.skyDraws;
  if (g.epoch != remix::skyEpoch()) {
    // Another map: its sky starts from cleared faces, captured at once.
    g.epoch = remix::skyEpoch();
    g.reset = true;
    g.captured = false;
  }
  const bool fresh = g.seen.size() < kSkyFreshTextures && !g.seen.count(g_texture0);
  if (!due(fresh) || !ready(dev))
    return;
  float view[16], projection[16];
  if (!remix::viewMatrices(view, projection))
    return;
  float eye[3];
  for (int i = 0; i < 3; ++i)
    eye[i] = -(view[12] * view[i * 4] + view[13] * view[i * 4 + 1] + view[14] * view[i * 4 + 2]);
  float mvp[16], viewProj[16], inverseViewProj[16], model[16];
  fromRegisters(&g_vsConstants[4 * 4], mvp);
  fromRegisters(&g_vsConstants[8 * 4], viewProj);
  if (!invert(viewProj, inverseViewProj))
    return;
  multiply(mvp, inverseViewProj, model);

  float saved[8 * 4];
  std::memcpy(saved, &g_vsConstants[4 * 4], sizeof(saved));
  IDirect3DSurface9 *oldTarget = nullptr, *oldDepth = nullptr;
  dev->GetRenderTarget(0, &oldTarget);
  dev->GetDepthStencilSurface(&oldDepth);
  D3DVIEWPORT9 oldViewport;
  dev->GetViewport(&oldViewport);
  // The view's clip planes (water, 3D skybox) do not apply to the faces.
  const DWORD zEnable = g_renderStates[D3DRS_ZENABLE], zWrite = g_renderStates[D3DRS_ZWRITEENABLE],
              scissor = g_renderStates[D3DRS_SCISSORTESTENABLE], clipPlanes = g_renderStates[D3DRS_CLIPPLANEENABLE];
  dev->SetDepthStencilSurface(nullptr);
  g_setRenderState(dev, D3DRS_ZENABLE, FALSE);
  g_setRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
  g_setRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
  if (clipPlanes)
    g_setRenderState(dev, D3DRS_CLIPPLANEENABLE, 0);
  const D3DVIEWPORT9 faceViewport = {0, 0, sky::kFaceSize, sky::kFaceSize, 0, 1};
  for (int face = 0; face < 6; ++face) {
    g_setRenderTarget(dev, 0, g.targets[face]);
    dev->SetViewport(&faceViewport);
    if (g.reset)
      dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
    float vp[16], m[16], regs[8 * 4];
    faceViewProjection(face, eye, vp);
    multiply(model, vp, m);
    toRegisters(m, regs);
    toRegisters(vp, regs + 16);
    g_setVertexShaderConstantF(dev, 4, regs, 8);
    g_drawIndexedPrimitive(dev, type, baseVertex, minIndex, numVertices, startIndex, primCount);
  }
  if (g.reset)
    g.seen.clear();
  g.reset = false;
  if (g.seen.size() < kSkyFreshTextures)
    g.seen.insert(g_texture0);
  g.captureFrame = g_d3dFrame;
  g.captured = true;
  g_setVertexShaderConstantF(dev, 4, saved, 8);
  g_setRenderTarget(dev, 0, oldTarget);
  dev->SetDepthStencilSurface(oldDepth);
  dev->SetViewport(&oldViewport);
  g_setRenderState(dev, D3DRS_ZENABLE, zEnable);
  g_setRenderState(dev, D3DRS_ZWRITEENABLE, zWrite);
  g_setRenderState(dev, D3DRS_SCISSORTESTENABLE, scissor);
  if (clipPlanes)
    g_setRenderState(dev, D3DRS_CLIPPLANEENABLE, clipPlanes);
  if (oldTarget)
    oldTarget->Release();
  if (oldDepth)
    oldDepth->Release();
}

// At Present: read the captured faces back and hand them to the dome. A
// readback waits for the GPU to have drawn the face and copies 1 MiB, so the
// faces are read one a frame, from the frame after the capture.
void finish(IDirect3DDevice9 *dev) {
  if (g.epoch != remix::skyEpoch())
    g.captured = false, g.reading = -1; // captured before the map changed
  if (g.device != dev)
    return;
  if (g.captured) {
    // This frame's capture: read from the next Present (again, if the faces
    // of the capture before it were still being read).
    g.captured = false;
    g.lastCapture = g_d3dFrame;
    g.reading = 0;
    g.readUs = 0;
    return;
  }
  if (g.reading < 0)
    return;
  const uint64_t t0 = nowUs();
  const int f = g.reading;
  // One sequential copy out of the locked surface; the dome reads the copy.
  D3DLOCKED_RECT locked;
  if (FAILED(dev->GetRenderTargetData(g.targets[f], g.readback[f])) ||
      FAILED(g.readback[f]->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
    ++g_stats.skyFailures;
    g.reading = -1;
    return;
  }
  g.pixels[f].resize(size_t(sky::kFaceSize) * sky::kFaceSize * 4);
  for (UINT y = 0; y < sky::kFaceSize; ++y)
    std::memcpy(g.pixels[f].data() + size_t(y) * sky::kFaceSize * 4,
                static_cast<uint8_t *>(locked.pBits) + size_t(y) * locked.Pitch, sky::kFaceSize * 4);
  g.readback[f]->UnlockRect();
  const uint64_t t1 = nowUs();
  g_stats.usSkyReadback += t1 - t0;
  g.readUs += t1 - t0;
  if (++g.reading < 6)
    return;
  g.reading = -1;
  ++g_stats.skyCaptures;
  const uint8_t *pointers[6];
  for (int i = 0; i < 6; ++i)
    pointers[i] = g.pixels[i].data();
  remix::setSkyFaces(pointers);
  // An unchanged sky is looked at less and less often; one whose dome was
  // replaced since the last capture as often as its cost allows.
  const bool changed = remix::takeSkyChanged();
  g_stats.skyUpdates += changed;
  g.interval = changed ? kSkyInterval : std::min(g.interval * 2, kSkyMaxInterval);
  const uint64_t t2 = nowUs();
  g.readUs += t2 - t1;
  g_stats.usSky += g.readUs;
  g.notBeforeUs = t2 + g.readUs * kSkyCostFactor;
}
} // namespace skycap

namespace targetread {
// The draw into the bound render-target texture is to be rasterized.
bool draw() {
  auto it = g_live.find(g_renderTargetTexture);
  if (it == g_live.end() || it->second.scene || g_d3dFrame - it->second.wanted > kWantedFrames)
    return false;
  if (g_renderStates[D3DRS_ZENABLE] || remix::drawIsPerspective()) {
    it->second.scene = true;
    return false;
  }
  it->second.dirty = true;
  ++g_draws;
  return true;
}

// Reads the copy issued for `texture` and hands the image to the output.
void collect(const void *texture, Live &live) {
  if (!live.pending || !live.system)
    return;
  live.pending = false;
  const uint64_t t0 = nowUs();
  D3DLOCKED_RECT locked;
  if (FAILED(live.system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
    ++g_failures;
    return;
  }
  remix::onTargetPixels(texture, live.width, live.height, live.format, static_cast<const uint8_t *>(locked.pBits),
                        uint32_t(locked.Pitch));
  live.system->UnlockRect();
  ++g_reads;
  g_collectUs += nowUs() - t0;
}

// Render target 0 is about to leave `surface` (level 0 of the bound texture).
void leave(IDirect3DDevice9 *dev, IDirect3DSurface9 *surface) {
  static const bool defer = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_TARGET_READ_DEFER", v, sizeof(v)) && v[0] == '0');
  }();
  const void *const texture = g_renderTargetTexture;
  auto it = g_live.find(texture);
  if (it == g_live.end() || !it->second.dirty || !surface)
    return;
  Live &live = it->second;
  live.dirty = false;
  if (live.scene)
    return;
  const TargetTexture &target = g_targetTextures[texture];
  const uint32_t width = std::min(target.width, kReadSize), height = std::min(target.height, kReadSize);
  // The image of an earlier frame first: its surface receives the new copy.
  // (A copy issued earlier in this frame is not waited for: the new one
  // replaces it.)
  if (live.pending && live.issued != g_d3dFrame)
    collect(texture, live);
  else if (live.pending)
    ++g_superseded;
  const uint64_t t0 = nowUs();
  const bool stretch = width != target.width || height != target.height;
  if (stretch && (width != g_width || height != g_height || target.format != g_format)) {
    if (g_small)
      g_small->Release();
    g_small = nullptr;
    g_width = g_height = 0;
    if (FAILED(dev->CreateRenderTarget(width, height, target.format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_small,
                                       nullptr))) {
      g_small = nullptr;
      ++g_failures;
      return;
    }
    g_width = width, g_height = height, g_format = target.format;
  }
  if (!live.system || live.width != width || live.height != height || live.format != target.format) {
    drop(live);
    if (FAILED(dev->CreateOffscreenPlainSurface(width, height, target.format, D3DPOOL_SYSTEMMEM, &live.system,
                                                nullptr))) {
      live.system = nullptr;
      ++g_failures;
      return;
    }
    live.width = width, live.height = height, live.format = target.format;
  }
  IDirect3DSurface9 *source = surface;
  if (stretch) {
    if (FAILED(dev->StretchRect(surface, nullptr, g_small, nullptr, D3DTEXF_LINEAR))) {
      ++g_failures;
      return;
    }
    source = g_small;
  }
  if (FAILED(dev->GetRenderTargetData(source, live.system))) {
    live.pending = false;
    ++g_failures;
    return;
  }
  live.pending = true;
  live.issued = g_d3dFrame;
  ++g_issues;
  if (defer) {
    // Nothing presents through D3D (see present): without this the copy
    // could stay queued until the lock, which would then wait as before.
    if (!g_flush && FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_flush)))
      g_flush = nullptr;
    if (g_flush && SUCCEEDED(g_flush->Issue(D3DISSUE_END)))
      g_flush->GetData(nullptr, 0, D3DGETDATA_FLUSH);
  }
  g_issueUs += nowUs() - t0;
  if (!defer)
    collect(texture, live);
}

// The frame ends: the images of targets that were not left again since the
// frame before (a screen that stopped changing still gets its last image).
void present() {
  for (auto &[texture, live] : g_live)
    if (live.pending && live.issued != g_d3dFrame)
      collect(texture, live);
}
} // namespace targetread

void wantTargetContents(const void *texture) {
  auto target = g_targetTextures.find(texture);
  if (target == g_targetTextures.end() || target->second.width > targetread::kMaxSize ||
      target->second.height > targetread::kMaxSize ||
      (target->second.format != D3DFMT_A8R8G8B8 && target->second.format != D3DFMT_X8R8G8B8))
    return;
  auto it = targetread::g_live.find(texture);
  if (it == targetread::g_live.end()) {
    if (targetread::g_live.size() >= 16)
      return;
    it = targetread::g_live.emplace(texture, targetread::Live{}).first;
  }
  it->second.wanted = g_d3dFrame;
}

HRESULT STDMETHODCALLTYPE drawIndexedPrimitive(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, INT baseVertex,
                                               UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount) {
  ++g_stats.dip;
  g_stats.dipPrims += primCount;
  if (remix::enabled()) {
    if (captureDraw(type, baseVertex, minIndex, numVertices, startIndex, primCount))
      skycap::capture(dev, type, baseVertex, minIndex, numVertices, startIndex, primCount);
    // Phase 2b: Remix presents the frame, so D3D9 no longer rasterizes it.
    // OPENREMIX_PROXY_D3D_DRAWS=1 keeps the original rendering (hidden window).
    static const bool keep = [] {
      char v[8] = {};
      return GetEnvironmentVariableA("OPENREMIX_PROXY_D3D_DRAWS", v, sizeof(v)) && v[0] == '1';
    }();
    if (!keep && g_renderTargetTexture && targetread::draw())
      return g_drawIndexedPrimitive(dev, type, baseVertex, minIndex, numVertices, startIndex, primCount);
    if (!keep) {
      // D3D9 keeps only depth, for the game's occlusion queries (sun glow
      // pixel visibility): depth-writing opaque draws and the query draws
      // themselves are rasterized with color writes off.
      // OPENREMIX_PROXY_D3D_DEPTH=0 skips every draw.
      static const bool depth = [] {
        char v[8] = {};
        return !(GetEnvironmentVariableA("OPENREMIX_PROXY_D3D_DEPTH", v, sizeof(v)) && v[0] == '0');
      }();
      const bool query = !g_openOcclusion.empty();
      bool writesDepth = g_renderStates[D3DRS_ZENABLE] && g_renderStates[D3DRS_ZWRITEENABLE] &&
                         !g_renderStates[D3DRS_ALPHABLENDENABLE];
      if (writesDepth && !query && g_occluders.vb && g_occluders.device == dev && !g_occluders.failed &&
          bsp::active())
        writesDepth = false; // the occluders stand in for it
      if (writesDepth && !query) {
        auto it = g_queryTargets.find(g_renderTarget0);
        writesDepth = it != g_queryTargets.end() && g_d3dFrame - it->second <= kQueryTargetFrames;
      }
      if (!depth || !(query || writesDepth))
        return D3D_OK;
      ++g_stats.depthDraws;
      const DWORD colorWrite = g_renderStates[D3DRS_COLORWRITEENABLE];
      if (colorWrite)
        g_setRenderState(dev, D3DRS_COLORWRITEENABLE, 0);
      const HRESULT hr = g_drawIndexedPrimitive(dev, type, baseVertex, minIndex, numVertices, startIndex, primCount);
      if (colorWrite)
        g_setRenderState(dev, D3DRS_COLORWRITEENABLE, colorWrite);
      return hr;
    }
  }
  return g_drawIndexedPrimitive(dev, type, baseVertex, minIndex, numVertices, startIndex, primCount);
}

HRESULT STDMETHODCALLTYPE drawPrimitiveUP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT count, const void *data,
                                          UINT stride) {
  ++g_stats.dpUP;
  return g_drawPrimitiveUP(dev, type, count, data, stride);
}

HRESULT STDMETHODCALLTYPE drawIndexedPrimitiveUP(IDirect3DDevice9 *dev, D3DPRIMITIVETYPE type, UINT minIndex,
                                                 UINT numVertices, UINT primCount, const void *indices,
                                                 D3DFORMAT format, const void *vertices, UINT stride) {
  ++g_stats.dipUP;
  return g_drawIndexedPrimitiveUP(dev, type, minIndex, numVertices, primCount, indices, format, vertices, stride);
}

void hookDevice(IDirect3DDevice9 *device) {
  if (g_present)
    return; // all devices share the vtable
  void **vt = vtableOf(device);
  hookSlot(vt, kReset, &reset, g_reset);
  hookSlot(vt, kPresent, &present, g_present);
  hookSlot(vt, kCreateTexture, &createTexture, g_createTexture);
  hookSlot(vt, kUpdateTexture, &updateTexture, g_updateTexture);
  hookSlot(vt, kUpdateSurface, &updateSurface, g_updateSurface);
  hookSlot(vt, kCreateVertexBuffer, &createVertexBuffer, g_createVertexBuffer);
  hookSlot(vt, kCreateIndexBuffer, &createIndexBuffer, g_createIndexBuffer);
  hookSlot(vt, kSetRenderTarget, &setRenderTarget, g_setRenderTarget);
  hookSlot(vt, kClear, &clear, g_clear);
  hookSlot(vt, kStretchRect, &stretchRect, g_stretchRect);
  hookSlot(vt, kSetViewport, &setViewport, g_setViewport);
  hookSlot(vt, kSetScissorRect, &setScissorRect, g_setScissorRect);
  hookSlot(vt, kSetSamplerState, &setSamplerState, g_setSamplerState);
  hookSlot(vt, kSetVertexShaderConstantI, &setVertexShaderConstantI, g_setVertexShaderConstantI);
  hookSlot(vt, kSetVertexShaderConstantB, &setVertexShaderConstantB, g_setVertexShaderConstantB);
  hookSlot(vt, kSetPixelShaderConstantI, &setPixelShaderConstantI, g_setPixelShaderConstantI);
  hookSlot(vt, kSetPixelShaderConstantB, &setPixelShaderConstantB, g_setPixelShaderConstantB);
  hookSlot(vt, kSetTexture, &setTexture, g_setTexture);
  hookSlot(vt, kSetRenderState, &setRenderState, g_setRenderState);
  hookSlot(vt, kDrawPrimitive, &drawPrimitive, g_drawPrimitive);
  hookSlot(vt, kDrawIndexedPrimitive, &drawIndexedPrimitive, g_drawIndexedPrimitive);
  hookSlot(vt, kDrawPrimitiveUP, &drawPrimitiveUP, g_drawPrimitiveUP);
  hookSlot(vt, kDrawIndexedPrimitiveUP, &drawIndexedPrimitiveUP, g_drawIndexedPrimitiveUP);
  hookSlot(vt, kSetVertexDeclaration, &setVertexDeclaration, g_setVertexDeclaration);
  hookSlot(vt, kCreateVertexShader, &createVertexShader, g_createVertexShader);
  hookSlot(vt, kSetVertexShader, &setVertexShader, g_setVertexShader);
  hookSlot(vt, kCreatePixelShader, &createPixelShader, g_createPixelShader);
  hookSlot(vt, kSetPixelShader, &setPixelShader, g_setPixelShader);
  hookSlot(vt, kSetVertexShaderConstantF, &setVertexShaderConstantF, g_setVertexShaderConstantF);
  hookSlot(vt, kSetPixelShaderConstantF, &setPixelShaderConstantF, g_setPixelShaderConstantF);
  hookSlot(vt, kSetStreamSource, &setStreamSource, g_setStreamSource);
  hookSlot(vt, kSetIndices, &setIndices, g_setIndices);
  hookSlot(vt, kCreateQuery, &createQuery, g_createQuery);
}

HRESULT STDMETHODCALLTYPE createDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                       D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
  remix::onCreateDevice(focus, pp);
  const HRESULT hr = g_createDevice(d3d, adapter, type, focus, flags, pp, out);
  if (SUCCEEDED(hr) && out && *out) {
    hookDevice(*out);
    refreshBackBuffer(*out);
    g_renderStates[D3DRS_COLORWRITEENABLE] = 0xf;
    g_renderStates[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    g_renderStates[D3DRS_STENCILPASS] = g_renderStates[D3DRS_STENCILFAIL] = g_renderStates[D3DRS_STENCILZFAIL] =
        D3DSTENCILOP_KEEP;
    g_renderStates[D3DRS_STENCILMASK] = g_renderStates[D3DRS_STENCILWRITEMASK] = 0xffffffff;
    g_renderStates[D3DRS_ZWRITEENABLE] = TRUE;
    g_renderStates[D3DRS_ZENABLE] = D3DZB_TRUE;
    g_renderStates[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    g_renderStates[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    g_renderStates[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    g_renderStates[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    g_renderStates[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    g_renderStates[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    g_renderStates[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    g_renderStates[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    remix::onDeviceCreated(*out);
  }
  return hr;
}

IDirect3D9 *WINAPI direct3DCreate9(UINT sdk) {
  IDirect3D9 *d3d = g_create(sdk);
  if (d3d && !g_createDevice)
    hookSlot(vtableOf(d3d), kD3DCreateDevice, &createDevice, g_createDevice);
  return d3d;
}

// Rewrites `module`'s IAT entry for dll!name.
void *patchImport(HMODULE module, const char *dll, const char *name, void *hook) {
  auto *base = reinterpret_cast<uint8_t *>(module);
  auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
  const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<char *>(base + imp->Name), dll) != 0)
      continue;
    auto *names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imp->OriginalFirstThunk);
    auto *iat = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++iat) {
      if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG64)
        continue;
      auto *byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<char *>(byName->Name), name) != 0)
        continue;
      DWORD old;
      VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &old);
      void *previous = reinterpret_cast<void *>(iat->u1.Function);
      iat->u1.Function = reinterpret_cast<ULONGLONG>(hook);
      VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), old, &old);
      return previous;
    }
  }
  return nullptr;
}

} // namespace

void *patchModuleImport(HMODULE module, const char *dll, const char *name, void *hook) {
  return patchImport(module, dll, name, hook);
}

bool install(HMODULE module) {
  g_create = reinterpret_cast<CreateFn>(
      patchImport(module, "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void *>(&direct3DCreate9)));
  return g_create != nullptr;
}

bool backBufferBound() { return g_renderTarget0 == g_backBuffer; }
const void *boundTargetTexture() {
  return g_renderTargetTexture && g_targetTextures.count(g_renderTargetTexture) ? g_renderTargetTexture : nullptr;
}
bool targetTexture(const void *texture, uint32_t &width, uint32_t &height, uint32_t &format) {
  auto it = g_targetTextures.find(texture);
  if (it == g_targetTextures.end())
    return false;
  width = it->second.width;
  height = it->second.height;
  format = it->second.format;
  return true;
}
std::shared_ptr<const std::vector<uint32_t>> shaderCode(const void *shader) {
  std::lock_guard<std::mutex> lock(g_vertexProgramLock);
  auto it = g_shaderCode.find(shader);
  return it == g_shaderCode.end() ? nullptr : it->second;
}
void requestTargetContents(const void *texture) { wantTargetContents(texture); }

void dumpStatistics(FILE *out) {
  const uint32_t frames = g_stats.frames.exchange(0);
  const double f = frames ? frames : 1;
  fprintf(out, "== D3D9 per frame (%u frames): DIP %.1f (%.0f prims) DP %.1f DIPUP %.1f DPUP %.1f, depth DIP %.1f (occlusion queries %.1f: visible %.1f hidden %.1f; occluder passes %.1f, depth clears after one %.1f)\n",
          frames, g_stats.dip.exchange(0) / f, g_stats.dipPrims.exchange(0) / f, g_stats.dp.exchange(0) / f,
          g_stats.dipUP.exchange(0) / f, g_stats.dpUP.exchange(0) / f, g_stats.depthDraws.exchange(0) / f, g_stats.occlusionQueries.exchange(0) / f,
          g_stats.queriesVisible.exchange(0) / f, g_stats.queriesHidden.exchange(0) / f, g_stats.occluderDraws.exchange(0) / f,
          g_stats.occluderClears.exchange(0) / f);
  fprintf(out, "   ms/frame in D3D9: Present %.2f, buffer Lock %.2f\n", g_stats.usPresent.exchange(0) / f / 1000,
          g_stats.usLock.exchange(0) / f / 1000);
  fprintf(out, "   buffer shadow copies per frame: %.1f (%.2f MiB, %.2f ms), whole-buffer locks %.1f\n",
          g_stats.shadowCopies.exchange(0) / f, g_stats.shadowBytes.exchange(0) / f / 1048576.0,
          g_stats.usShadow.exchange(0) / f / 1000, g_stats.wholeLocks.exchange(0) / f);
  {
    std::lock_guard<std::mutex> lock(g_shadowLock);
    for (auto &[key, c] : g_copyClasses)
      fprintf(out, "     usage 0x%x pool %u lock 0x%x: %.1f copies %.3f MiB %.2f ms (%.0f MB/s)\n",
              unsigned(key >> 32), unsigned((key >> 24) & 0xff), unsigned(key & 0xffffff), c.copies / f,
              c.bytes / f / 1048576.0, c.us / f / 1000, c.us ? c.bytes / double(c.us) : 0.0);
    g_copyClasses.clear();
  }
  {
    // The 2D sky capture (skycap), over the interval: no draws = the engine
    // drew no 2D sky (or its draws are not recognized: remix::isSkyDraw).
    const uint32_t captures = g_stats.skyCaptures.exchange(0);
    const double c = captures ? captures : 1;
    fprintf(out, "   2D sky: draws %.2f per frame; captures %u (dome updates %u, failed readbacks %u), %.1f ms each (readback of %u^2 x 6: %.1f ms)\n",
            g_stats.skyDraws.exchange(0) / f, captures, g_stats.skyUpdates.exchange(0), g_stats.skyFailures.exchange(0),
            g_stats.usSky.exchange(0) / c / 1000, sky::kFaceSize, g_stats.usSkyReadback.exchange(0) / c / 1000);
  }
  {
    const uint32_t reads = targetread::g_reads.exchange(0), issues = targetread::g_issues.exchange(0);
    size_t scene = 0;
    for (const auto &[texture, live] : targetread::g_live)
      scene += live.scene;
    // issue: StretchRect, GetRenderTargetData and the flush, when the target
    // is left; lock: LockRect and the copy out, a frame later.
    fprintf(out, "   render-target contents: %zu textures sampled by the scene (%zu scene views, left alone); 2D draws "
                 "rasterized %.2f per frame, readbacks issued %.2f and read %.2f per frame (replaced unread %u), "
                 "issue %.2f ms + lock %.2f ms each, failed %u\n",
            targetread::g_live.size(), scene, targetread::g_draws.exchange(0) / f, issues / f, reads / f,
            targetread::g_superseded.exchange(0), targetread::g_issueUs.exchange(0) / (issues ? issues : 1) / 1000.0,
            targetread::g_collectUs.exchange(0) / (reads ? reads : 1) / 1000.0, targetread::g_failures.exchange(0));
  }
  fprintf(out, "   RT changes %.1f, SetTexture %.1f, SetStreamSource %.1f, SetVSConstF %.1f\n",
          g_stats.rtChanges.exchange(0) / f, g_stats.textureBinds.exchange(0) / f, g_stats.streamSets.exchange(0) / f,
          g_stats.vsConstCalls.exchange(0) / f);
  {
    std::lock_guard<std::mutex> lock(g_vertexProgramLock);
    size_t runnable = 0, expanding = 0;
    for (const auto &[shader, program] : g_vertexPrograms)
      if (program) {
        runnable += vsprog::info(*program).supported;
        expanding += vsprog::info(*program).expands;
      }
    fprintf(out, "   vertex shaders decoded since launch %u; live %zu: %zu the proxy can run (the others skin), %zu "
                 "build their geometry (particles)\n",
            g_stats.vertexPrograms.load(), g_vertexPrograms.size(), runnable, expanding);
  }
  size_t shadowBytes = 0, shadowCount = 0;
  {
    std::lock_guard<std::mutex> lock(g_shadowLock);
    shadowCount = g_shadows.size();
    for (auto &[b, s] : g_shadows)
      shadowBytes += s.data.size();
  }
  {
    // 32-bit address space is the constraint that matters in the game process.
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
    fprintf(out, "   process memory: address space used %.0f of %.0f MiB, private %.0f MiB\n",
            (mem.ullTotalVirtual - mem.ullAvailVirtual) / 1048576.0, mem.ullTotalVirtual / 1048576.0,
            pmc.PrivateUsage / 1048576.0);
  }
  fprintf(out, "   buffer shadows: %zu buffers, %.1f MiB; partial texture locks %u\n", shadowCount,
          shadowBytes / 1048576.0, g_stats.partialTextureLocks.exchange(0));
  remix::dumpStatistics(out);
  hostpass::dumpStatistics(out);
}

} // namespace proxy::d3d
