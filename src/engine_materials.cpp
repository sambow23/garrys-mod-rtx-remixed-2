#include "engine_materials.h"
#include "bsp_world.h"
#include "remix_output.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

// The material system binds a material with IShaderAPI::Bind(IMaterial *)
// (CMatRenderContext::Bind, after the material's bind proxies ran; with a
// queued material system the proxies' writes and the Bind reach the render
// thread in that order). CShaderAPIDx8::Bind flushes the primitives buffered
// for the previous material before it stores the new one, so every D3D draw
// belongs to the material bound last.
//
// Everything below is for one build of materialsystem.dll and of the shader
// API (shaderapidx9_orig.dll). install() requires each function used at its
// RVA with these instruction bytes, the offsets read are the ones in those
// instructions, and per object the vtable must be the class's: anything else
// (another build, CMaterialSubRect, a material var of another class) is not
// read and counts as Lit.
//
//   CMaterial (IMaterial, materialsystem.dll): 0 GetName, 40 ShaderParamCount
//     (movzx eax, byte [rcx+0x32]), 41 GetShaderParams (mov rax, [rcx+0x38]),
//     49 GetShaderName (the shader at [rcx+0x20], its slot 0).
//   CMaterialVar (IMaterialVar): 1 GetName, 6 GetStringValue, 17 IsDefined
//     (type: low nibble of [rcx+0x24], 6 undefined, 3 texture), 26
//     GetIntValueInternal, 27 GetFloatValueInternal, 29 GetVecValueInternal()
//     (-> float[4]). GetStringValue's texture case reads the ITexture at
//     [rcx+0x39] (-1: env_cubemap) and calls its slot 0, GetName.
//     The getters forward to a queue-thread twin ([rcx+0x29]) only while the
//     calling thread's render context has a call queue; on the thread that
//     executes Bind they return the var's own value, as the shaders read it.
//   CMaterialSubRect (a rectangle of an atlas material: the world's bullet
//     holes, blood and scorch marks are "decals/x_subrect" in "decals/
//     decals_mod2x" or "decals/decals_lit"): 0 GetName (its own name symbol at
//     +0x14), 7 InMaterialPage (true), 10 GetMaterialPage (mov rax, [rcx+8]),
//     the CMaterial every other call forwards to. Optional: without it such
//     materials stay unread.
//   CShaderAPIDx8 (IShaderAPI, ShaderApi030): 114 Bind(IMaterial *). Between
//     two sub-rectangles of one page it neither flushes nor stores (they draw
//     as one batch), but the hook still sees every call.
namespace proxy::engine_materials {
namespace {

constexpr uintptr_t kMaterialVtable = 0xadf08, kVarVtable = 0xb3260;
constexpr unsigned kSlotGetName = 0, kSlotParamCount = 40, kSlotParams = 41, kSlotShaderName = 49;
constexpr unsigned kVarGetName = 1, kVarGetString = 6, kVarIsDefined = 17, kVarGetInt = 26, kVarGetFloat = 27,
                   kVarGetVec = 29;
constexpr uintptr_t kGetName = 0xe7e0, kParamCount = 0x11cd0, kParams = 0xf020, kShaderName = 0xefd0;
constexpr uintptr_t kVarName = 0x26b10, kVarString = 0x26ba0, kVarDefined = 0x27080, kVarInt = 0x26840,
                    kVarFloat = 0x26720, kVarVec = 0x26f60;
constexpr uintptr_t kSubRectVtable = 0xaf050, kSubRectName = 0x15420, kSubRectInPage = 0xfe50, kSubRectPage = 0x12980;
constexpr unsigned kSlotInPage = 7, kSlotPage = 10;
constexpr size_t kSubRectPageOffset = 0x8, kSubRectNameOffset = 0x14;
constexpr unsigned kSlotBind = 114;
constexpr uintptr_t kBind = 0x11540;

// CMaterial: m_VarCount, m_pShaderParams, m_Name (CUtlSymbol), m_pShader.
constexpr size_t kMaterialCount = 0x32, kMaterialParams = 0x38, kMaterialName = 0x28, kMaterialShader = 0x20;
// CMaterialVar: type nibble, ITexture * of a texture var.
constexpr size_t kVarType = 0x24, kVarTexture = 0x39;
// MaterialVarType_t: float 0, string 1, vector 2, texture 3, int 4, fourcc 5, undefined 6, matrix 7, material 8.
constexpr uint8_t kTypeFloat = 0, kTypeString = 1, kTypeVector = 2, kTypeTexture = 3, kTypeInt = 4, kTypeUndefined = 6;
// MaterialVarFlags_t.
constexpr int kFlagVertexColor = 1 << 4, kFlagVertexAlpha = 1 << 5, kFlagSelfIllum = 1 << 6, kFlagAdditive = 1 << 7,
              kFlagAlphaTest = 1 << 8, kFlagDecal = 1 << 16, kFlagEnvmapSphere = 1 << 17,
              kFlagBaseAlphaEnvmapMask = 1 << 20, kFlagTranslucent = 1 << 21, kFlagNormalAlphaEnvmapMask = 1 << 22,
              kFlagEnvmapMode = 1 << 25;

struct Expected {
  bool shaderApi; // else materialsystem.dll
  uintptr_t rva;
  const char *bytes; // hex, space separated
  const char *what;
};
constexpr Expected kExpected[] = {
    // mov rsi,rcx; mov rdi,rdx; mov rcx,[rcx+0x2d10] (m_pMaterial); ... InMaterialPage on both
    {true, 0x11540,
     "48 89 74 24 10 57 48 83 EC 20 48 8B F1 48 8B FA 48 8B 89 10 2D 00 00 48 85 C9 74 47 48 85 D2 74 42 48 8B 01 FF "
     "50 38",
     "Bind entry"},
    // mov rax,[rsi]; mov rcx,rsi; call [rax+0x398] (FlushBufferedPrimitives, slot 115); mov [rsi+0x2d10],rdi
    {true, 0x115d0, "48 8B 06 48 8B CE FF 90 98 03 00 00 48 89 BE 10 2D 00 00", "Bind flushes, then stores"},
    // add rcx,0x28; jmp CUtlSymbol::String
    {false, kGetName, "48 83 C1 28 E9", "CMaterial::GetName"},
    {false, kParamCount, "0F B6 41 32 C3", "CMaterial::ShaderParamCount"},
    {false, kParams, "48 8B 41 38 C3", "CMaterial::GetShaderParams"},
    // PrecacheVars (returns at once on a precached material); mov rcx,[rbx+0x20]; ... jmp [rax] / "shader not loaded"
    {false, kShaderName,
     "40 53 48 83 EC 30 48 8B 01 45 33 C9 45 33 C0 C7 44 24 20 00 00 00 00 33 D2 48 8B D9 FF 90 00 02 00 00 48 8B 4B "
     "20 48 85 C9 74 0B 48 8B 01 48 83 C4 30 5B 48 FF 20",
     "CMaterial::GetShaderName"},
    // movzx ebx, word [rcx+0x26] (the name symbol); 0xffff: none
    {false, kVarName, "40 53 48 83 EC 20 0F B7 59 26 B8 FF FF 00 00 66 3B D8", "CMaterialVar::GetName"},
    {false, kVarDefined, "0F B6 41 24 24 0F 3C 06 0F 95 C0 C3", "CMaterialVar::IsDefined"},
    // ...; mov eax,[rbx+0x10]; add rsp,0x20; pop rbx; ret
    {false, kVarInt,
     "40 53 48 83 EC 20 48 8B D9 48 8B 0D 00 48 0B 00 48 8B 01 FF 90 70 04 00 00 48 85 C0 74 18 48 8B 4B 29 48 85 C9 "
     "74 0F 48 8B 01 48 83 C4 20 5B 48 FF A0 D0 00 00 00 8B 43 10 48 83 C4 20 5B C3",
     "CMaterialVar::GetIntValueInternal"},
    // ...; movss xmm0,[rbx+0x14]; ...
    {false, kVarFloat,
     "40 53 48 83 EC 20 48 8B D9 48 8B 0D 20 49 0B 00 48 8B 01 FF 90 70 04 00 00 48 85 C0 74 18 48 8B 4B 29 48 85 C9 "
     "74 0F 48 8B 01 48 83 C4 20 5B 48 FF A0 D8 00 00 00 F3 0F 10 43 14 48 83 C4 20 5B C3",
     "CMaterialVar::GetFloatValueInternal"},
    // ...; lea rax,[rbx+0x14]; ...
    {false, kVarVec,
     "40 53 48 83 EC 20 48 8B D9 48 8B 0D E0 40 0B 00 48 8B 01 FF 90 70 04 00 00 48 85 C0 74 18 48 8B 4B 29 48 85 C9 "
     "74 0F 48 8B 01 48 83 C4 20 5B 48 FF A0 E8 00 00 00 48 8D 43 14 48 83 C4 20 5B C3",
     "CMaterialVar::GetVecValueInternal"},
    {false, kVarString, "41 56 48 83 EC 30 4C 8B F1", "CMaterialVar::GetStringValue"},
    // Its jump table entry for type 3 (texture): the case below.
    {false, 0x26e3c, "A4 6D 02 00", "GetStringValue texture case"},
    // mov rcx,[r14+0x39]; cmp rcx,-1; jne +0x12
    {false, 0x26da4, "49 8B 4E 39 48 83 F9 FF 75 12", "texture var's ITexture"},
    // mov rax,[rcx]; call [rax]
    {false, 0x26dc0, "48 8B 01 FF 10", "ITexture::GetName call"},
};

struct Module {
  uintptr_t base = 0, size = 0;
  bool open(HMODULE module) {
    if (!module)
      return false;
    base = reinterpret_cast<uintptr_t>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
      return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
      return false;
    size = nt->OptionalHeader.SizeOfImage;
    return true;
  }
  bool inside(uintptr_t address, size_t bytes) const {
    return address >= base && bytes <= size && address - base <= size - bytes;
  }
  bool bytesAt(uintptr_t rva, const char *hex) const {
    for (const char *p = hex; *p; ++rva) {
      char *end = nullptr;
      const unsigned long byte = std::strtoul(p, &end, 16);
      if (end == p || rva >= size || *reinterpret_cast<const uint8_t *>(base + rva) != uint8_t(byte))
        return false;
      for (p = end; *p == ' ';)
        ++p;
    }
    return true;
  }
} g_materialSystem, g_shaderApi;

std::atomic<bool> g_installed{false}, g_subRects{false};
std::atomic<const char *> g_status{"not installed"}, g_detail{""};

using NameFn = const char *(*)(void *);
using IntFn = int (*)(void *);
using FloatFn = float (*)(void *);
using VecFn = const float *(*)(void *);
NameFn g_materialName = nullptr, g_shaderName = nullptr, g_varName = nullptr, g_subRectName = nullptr,
       g_varString = nullptr;
IntFn g_varInt = nullptr;
FloatFn g_varFloat = nullptr;
VecFn g_varVec = nullptr;

std::string lower(const char *text) {
  std::string out = text ? text : "";
  for (char &c : out)
    c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// What is known of a material, while it keeps this identity (a reloaded
// material gets new vars; a freed one's address may be reused).
struct Entry {
  void **params = nullptr;
  void *shader = nullptr;
  uint16_t nameSymbol = 0;
  uint8_t count = 0;
  Shading::Mode kind = Shading::Lit; // by shader: Lit (nothing to read), Unlit, SelfIllumBaseAlpha (capable)
  // Indices into the shader params (-1: the shader has no such parameter).
  int16_t flags = -1, color = -1, color2 = -1, tint = -1, mask = -1, maskScale = -1, envmapMask = -1,
          envmapMaskAlpha = -1, fresnel = -1, fresnelRange = -1, base = -1;
  // PBR inputs (pbr_rules), of the lit surface shaders.
  int16_t bump = -1, ssbump = -1, phong = -1, phongExponent = -1, phongExponentTexture = -1, baseAlphaPhongMask = -1,
          envmap = -1, envmapTint = -1, phongAlbedoTint = -1, blendTintByBaseAlpha = -1, phongBoost = -1,
          surfaceProp = -1;
  bool pbrCapable = false;
  std::string name, shaderName;
  bool decalShader = false, modulate = false, lightmapped = false; // by shader name
  // A texture var's name, by the ITexture last seen in it.
  struct TextureName {
    void *texture = nullptr;
    std::string name;
  } maskTexture, baseTexture, bumpTexture, exponentTexture, envmapMaskTexture;
};
std::unordered_map<void *, Entry> g_entries; // render thread
Shading g_current;                           // render thread
const Entry *g_currentEntry = nullptr;       // render thread
Bound g_bound;                               // render thread
// Sub-rectangle materials' own names, while they keep their name symbol.
struct SubRectName {
  uint16_t symbol = 0;
  std::string name;
};
std::unordered_map<void *, SubRectName> g_subRectNames; // render thread
const char *g_currentName = "";                         // render thread

struct Seen {
  std::string shader;
  Shading::Mode mode = Shading::Lit;
  float tint[3] = {};
  float maskScale = 1;
  std::string mask;
  uint32_t binds = 0;
};
struct Statistics {
  std::atomic<uint32_t> binds{0}, unlit{0}, baseAlpha{0}, mask{0}, maskAlpha{0};
  std::atomic<uint32_t> otherClass{0}, noParams{0}, foreignVar{0}, noMaskTexture{0}, subRects{0}, subRectNoPage{0},
      decals{0};
  std::mutex lock;
  std::map<std::string, uint32_t> shaders; // distinct materials by shader name, since launch
  std::map<std::string, Seen> seen;        // unlit and self-illuminated materials bound this interval
  size_t entries = 0;
} g_stats;

uint8_t varType(void *var) { return *(static_cast<const uint8_t *>(var) + kVarType) & 0xf; }

// A param the engine's getters can be called on.
void *paramAt(const Entry &e, int index) {
  if (index < 0 || index >= e.count)
    return nullptr;
  void *var = e.params[index];
  if (!var || *static_cast<uintptr_t *>(var) != g_materialSystem.base + kVarVtable) {
    g_stats.foreignVar.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  return var;
}

bool defined(void *var) { return var && varType(var) != kTypeUndefined; }

// The name of the texture a var holds ("" when it holds none).
const std::string &textureName(Entry::TextureName &cache, void *var) {
  static const std::string none;
  if (!var || varType(var) != kTypeTexture)
    return none;
  void *texture;
  std::memcpy(&texture, static_cast<const uint8_t *>(var) + kVarTexture, sizeof(texture));
  if (!texture || texture == reinterpret_cast<void *>(~uintptr_t(0)))
    return none;
  if (texture != cache.texture) {
    // An ITexture of the material system (its vtable is in that module).
    const uintptr_t vtable = *static_cast<uintptr_t *>(texture);
    if (!g_materialSystem.inside(vtable, sizeof(void *)) ||
        !g_materialSystem.inside(*reinterpret_cast<uintptr_t *>(vtable), 1))
      return none;
    cache.name = lower(reinterpret_cast<NameFn>(*reinterpret_cast<void **>(vtable))(texture));
    cache.texture = texture;
  }
  return cache.name;
}

std::string textureNameOfTexture(void *texture) {
  if (!texture || texture == reinterpret_cast<void *>(~uintptr_t(0)) || !g_materialSystem.base)
    return {};
  // An ITexture of the material system (its vtable is in that module).
  const uintptr_t vtable = *static_cast<uintptr_t *>(texture);
  if (!g_materialSystem.inside(vtable, sizeof(void *)) ||
      !g_materialSystem.inside(*reinterpret_cast<uintptr_t *>(vtable), 1))
    return {};
  const char *name = reinterpret_cast<NameFn>(*reinterpret_cast<void **>(vtable))(texture);
  return name ? lower(name) : std::string();
}

void resolve(Entry &e, void *material) {
  e.name = lower(g_materialName(material));
  const char *shader = g_shaderName(material);
  e.shaderName = shader ? shader : "";
  e.kind = shaderMode(e.shaderName.c_str());
  {
    // "DecalModulate_dx9", "DecalBaseTimesLightmapAlphaBlendSelfIllum_DX9", ...
    const std::string shaderLower = lower(e.shaderName.c_str());
    e.decalShader = shaderLower.compare(0, 5, "decal") == 0;
    e.modulate = shaderLower.compare(0, 13, "decalmodulate") == 0;
    for (const char *world : {"lightmappedgeneric", "worldvertextransition", "lightmapped_4wayblend", "decalbasetimeslightmap"})
      e.lightmapped |= shaderLower.compare(0, std::strlen(world), world) == 0;
    e.pbrCapable = pbr::capable(shaderLower);
  }
  {
    std::lock_guard<std::mutex> lock(g_stats.lock);
    ++g_stats.shaders[e.shaderName];
  }
  struct Wanted {
    const char *name;
    int16_t Entry::*index;
  };
  static const Wanted wanted[] = {
      {"$flags", &Entry::flags},
      {"$color", &Entry::color},
      {"$color2", &Entry::color2},
      {"$selfillumtint", &Entry::tint},
      {"$selfillummask", &Entry::mask},
      {"$selfillummaskscale", &Entry::maskScale},
      {"$envmapmask", &Entry::envmapMask},
      {"$selfillum_envmapmask_alpha", &Entry::envmapMaskAlpha},
      {"$selfillumfresnel", &Entry::fresnel},
      {"$selfillumfresnelminmaxexp", &Entry::fresnelRange},
      {"$basetexture", &Entry::base},
      {"$bumpmap", &Entry::bump},
      {"$ssbump", &Entry::ssbump},
      {"$phong", &Entry::phong},
      {"$phongexponent", &Entry::phongExponent},
      {"$phongexponenttexture", &Entry::phongExponentTexture},
      {"$basemapalphaphongmask", &Entry::baseAlphaPhongMask},
      {"$envmap", &Entry::envmap},
      {"$envmaptint", &Entry::envmapTint},
      {"$phongalbedotint", &Entry::phongAlbedoTint},
      {"$blendtintbybasealpha", &Entry::blendTintByBaseAlpha},
      {"$phongboost", &Entry::phongBoost},
      {"$surfaceprop", &Entry::surfaceProp},
  };
  for (int i = 0; i < e.count; ++i) {
    void *var = paramAt(e, i);
    const char *name = var ? g_varName(var) : nullptr;
    if (!name || name[0] != '$')
      continue;
    for (const Wanted &w : wanted)
      if (e.*w.index < 0 && _stricmp(name, w.name) == 0)
        e.*w.index = int16_t(i);
  }
}

void vec3(const Entry &e, int index, float out[3]) {
  void *var = paramAt(e, index);
  if (!defined(var))
    return;
  const float *v = g_varVec(var);
  for (int c = 0; c < 3; ++c)
    if (std::isfinite(v[c]))
      out[c] = v[c];
}

void read(Entry &e, Shading &out) {
  void *flagsVar = paramAt(e, e.flags);
  const int flags = flagsVar ? g_varInt(flagsVar) : 0;
  if (e.kind == Shading::Unlit) {
    out.mode = Shading::Unlit;
    out.vertexColor = (flags & kFlagVertexColor) != 0;
    out.vertexAlpha = (flags & kFlagVertexAlpha) != 0;
    // $color x $color2, gamma values the shaders linearize.
    float color[3] = {1, 1, 1}, color2[3] = {1, 1, 1};
    vec3(e, e.color, color);
    vec3(e, e.color2, color2);
    for (int c = 0; c < 3; ++c)
      out.tint[c] = std::pow(std::max(color[c] * color2[c], 0.0f), 2.2f);
    return;
  }
  // $selfillum_envmapmask_alpha (a float; the shaders have it without
  // $selfillum): 8 x the envmap mask's alpha, overbright above 1/8.
  void *alphaVar = paramAt(e, e.envmapMaskAlpha);
  const bool maskAlpha = !(flags & kFlagSelfIllum) && defined(alphaVar) && g_varFloat(alphaVar) != 0;
  if (!(flags & kFlagSelfIllum) && !maskAlpha)
    return;
  vec3(e, e.tint, out.tint); // linear: the shaders multiply the linear albedo by it
  for (float &c : out.tint)
    c = std::max(c, 0.0f);
  if (maskAlpha) {
    const std::string &name = textureName(e.maskTexture, paramAt(e, e.envmapMask));
    if (name.empty()) {
      g_stats.noMaskTexture.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    out.mode = Shading::SelfIllumMaskAlpha;
    out.mask = name;
    out.maskScale = 8;
    return;
  }
  out.mode = Shading::SelfIllumBaseAlpha;
  if (void *maskVar = paramAt(e, e.mask); defined(maskVar)) {
    const std::string &name = textureName(e.maskTexture, maskVar);
    if (name.empty()) {
      // The shader samples the mask instead of the base alpha: without its
      // texture nothing is known to glow.
      g_stats.noMaskTexture.fetch_add(1, std::memory_order_relaxed);
      out.mode = Shading::Lit;
      return;
    }
    out.mode = Shading::SelfIllumMask;
    out.mask = name;
    if (void *scale = paramAt(e, e.maskScale); defined(scale)) {
      const float value = g_varFloat(scale);
      if (std::isfinite(value) && value > 0)
        out.maskScale = value;
    }
  }
  if (void *fresnel = paramAt(e, e.fresnel); defined(fresnel) && g_varInt(fresnel) != 0) {
    float range[3] = {0, 1, 1};
    vec3(e, e.fresnelRange, range);
    out.maskScale *= fresnelShare(range[0], range[1], range[2]);
    if (!(out.maskScale > 0))
      out.mode = Shading::Lit;
  }
}

// The material's PBR inputs, as its shader reads them now.
void readPbr(Entry &e, int flags, Shading &out) {
  auto number = [&](int index) {
    void *var = paramAt(e, index);
    return defined(var) ? g_varFloat(var) : 0.0f;
  };
  pbr::Inputs in;
  in.shader = lower(e.shaderName.c_str());
  in.base = out.base;
  in.bump = textureName(e.bumpTexture, paramAt(e, e.bump));
  in.envmapMask = textureName(e.envmapMaskTexture, paramAt(e, e.envmapMask));
  in.phongExponentTexture = textureName(e.exponentTexture, paramAt(e, e.phongExponentTexture));
  in.ssbump = number(e.ssbump) != 0;
  in.phong = number(e.phong) != 0;
  in.envmap = defined(paramAt(e, e.envmap));
  in.baseAlphaPhongMask = number(e.baseAlphaPhongMask) != 0;
  in.baseAlphaEnvmapMask = (flags & kFlagBaseAlphaEnvmapMask) != 0;
  in.normalAlphaEnvmapMask = (flags & kFlagNormalAlphaEnvmapMask) != 0;
  in.envmapSphere = (flags & kFlagEnvmapSphere) != 0;
  in.envmapMode = (flags & kFlagEnvmapMode) != 0;
  in.phongExponent = number(e.phongExponent);
  if (defined(paramAt(e, e.envmapTint))) {
    in.hasEnvmapTint = true;
    vec3(e, e.envmapTint, in.envmapTint);
  }
  in.phongAlbedoTint = number(e.phongAlbedoTint) != 0;
  in.blendTintByBaseAlpha = number(e.blendTintByBaseAlpha) != 0;
  if (defined(paramAt(e, e.color2))) {
    in.hasColor2 = true;
    vec3(e, e.color2, in.color2);
  }
  in.mwb = pbr::mwbBase(in.base);
  if (void *boost = paramAt(e, e.phongBoost); defined(boost))
    in.phongBoost = g_varFloat(boost);
  if (void *prop = paramAt(e, e.surfaceProp); defined(prop) && varType(prop) == kTypeString)
    in.surfaceProp = lower(g_varString(prop));
  in.luminance = bsp::textureLuminance;
  out.pbr = pbr::convert(in);
}

} // namespace

bool install(const void *const *shaderApiVtable) {
  static std::atomic<bool> done{false};
  if (done.exchange(true))
    return g_installed;
  char v[8] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_EMISSIVE", v, sizeof(v)) && v[0] == '0') {
    g_status = "disabled (OPENREMIX_PROXY_EMISSIVE=0)";
    return false;
  }
  if (!g_materialSystem.open(GetModuleHandleA("materialsystem.dll"))) {
    g_status = "no materialsystem.dll";
    return false;
  }
  HMODULE shaderApi = nullptr;
  if (!shaderApiVtable ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(shaderApiVtable), &shaderApi) ||
      !g_shaderApi.open(shaderApi)) {
    g_status = "no shader API module";
    return false;
  }
  if (!g_shaderApi.inside(reinterpret_cast<uintptr_t>(shaderApiVtable), (kSlotBind + 1) * sizeof(void *)) ||
      reinterpret_cast<uintptr_t>(shaderApiVtable[kSlotBind]) != g_shaderApi.base + kBind) {
    g_status = "slot mismatch";
    g_detail = "IShaderAPI::Bind";
    return false;
  }
  struct Slot {
    uintptr_t vtable;
    unsigned slot;
    uintptr_t rva;
    const char *name;
  };
  const Slot slots[] = {
      {kMaterialVtable, kSlotGetName, kGetName, "CMaterial::GetName"},
      {kMaterialVtable, kSlotParamCount, kParamCount, "CMaterial::ShaderParamCount"},
      {kMaterialVtable, kSlotParams, kParams, "CMaterial::GetShaderParams"},
      {kMaterialVtable, kSlotShaderName, kShaderName, "CMaterial::GetShaderName"},
      {kVarVtable, kVarGetName, kVarName, "CMaterialVar::GetName"},
      {kVarVtable, kVarGetString, kVarString, "CMaterialVar::GetStringValue"},
      {kVarVtable, kVarIsDefined, kVarDefined, "CMaterialVar::IsDefined"},
      {kVarVtable, kVarGetInt, kVarInt, "CMaterialVar::GetIntValueInternal"},
      {kVarVtable, kVarGetFloat, kVarFloat, "CMaterialVar::GetFloatValueInternal"},
      {kVarVtable, kVarGetVec, kVarVec, "CMaterialVar::GetVecValueInternal"},
  };
  for (const Slot &s : slots) {
    const uintptr_t entry = s.vtable + s.slot * sizeof(void *);
    if (entry + sizeof(void *) > g_materialSystem.size ||
        *reinterpret_cast<uintptr_t *>(g_materialSystem.base + entry) != g_materialSystem.base + s.rva) {
      g_status = "slot mismatch";
      g_detail = s.name;
      return false;
    }
  }
  for (const Expected &expected : kExpected)
    if (!(expected.shaderApi ? g_shaderApi : g_materialSystem).bytesAt(expected.rva, expected.bytes)) {
      g_status = "code mismatch";
      g_detail = expected.what;
      return false;
    }
  auto at = [](uintptr_t rva) { return reinterpret_cast<void *>(g_materialSystem.base + rva); };
  g_materialName = reinterpret_cast<NameFn>(at(kGetName));
  g_shaderName = reinterpret_cast<NameFn>(at(kShaderName));
  g_varName = reinterpret_cast<NameFn>(at(kVarName));
  g_varString = reinterpret_cast<NameFn>(at(kVarString));
  g_varInt = reinterpret_cast<IntFn>(at(kVarInt));
  g_varFloat = reinterpret_cast<FloatFn>(at(kVarFloat));
  g_varVec = reinterpret_cast<VecFn>(at(kVarVec));
  // Sub-rectangle materials: their page is read instead. On another layout
  // they stay unread, as before.
  {
    auto slot = [](unsigned index) {
      const uintptr_t entry = kSubRectVtable + index * sizeof(void *);
      return entry + sizeof(void *) <= g_materialSystem.size
                 ? *reinterpret_cast<uintptr_t *>(g_materialSystem.base + entry) - g_materialSystem.base
                 : 0;
    };
    if (slot(kSlotGetName) == kSubRectName && slot(kSlotInPage) == kSubRectInPage && slot(kSlotPage) == kSubRectPage &&
        g_materialSystem.bytesAt(kSubRectName, "48 83 C1 14 E9") && g_materialSystem.bytesAt(kSubRectInPage, "B0 01 C3") &&
        g_materialSystem.bytesAt(kSubRectPage, "48 8B 41 08 C3")) {
      g_subRectName = reinterpret_cast<NameFn>(at(kSubRectName));
      g_subRects = true;
    }
  }
  g_status = g_subRects ? "ok (IShaderAPI::Bind read, sub-rectangle materials through their page)"
                        : "ok (IShaderAPI::Bind read; CMaterialSubRect layout not recognized: decal atlases unread)";
  g_installed = true;
  return true;
}

void onBind(void *material) {
  g_current = Shading{};
  g_currentEntry = nullptr;
  g_bound = Bound{};
  g_currentName = "";
  if (!g_installed.load(std::memory_order_relaxed) || !material)
    return;
  g_stats.binds.fetch_add(1, std::memory_order_relaxed);
  // A sub-rectangle of an atlas: its page is the material the draw uses.
  const char *subRectName = nullptr;
  if (g_subRects.load(std::memory_order_relaxed) &&
      *static_cast<uintptr_t *>(material) == g_materialSystem.base + kSubRectVtable) {
    void *page;
    uint16_t symbol;
    std::memcpy(&page, static_cast<const uint8_t *>(material) + kSubRectPageOffset, sizeof(page));
    std::memcpy(&symbol, static_cast<const uint8_t *>(material) + kSubRectNameOffset, sizeof(symbol));
    if (!page) {
      g_stats.subRectNoPage.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (g_subRectNames.size() > 4096)
      g_subRectNames.clear();
    SubRectName &own = g_subRectNames[material];
    if (own.name.empty() || own.symbol != symbol) {
      own.symbol = symbol;
      own.name = lower(g_subRectName(material));
    }
    subRectName = own.name.c_str();
    material = page;
    g_stats.subRects.fetch_add(1, std::memory_order_relaxed);
  }
  // CMaterial only (its vars are its own).
  if (*static_cast<uintptr_t *>(material) != g_materialSystem.base + kMaterialVtable) {
    g_stats.otherClass.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto *bytes = static_cast<const uint8_t *>(material);
  void **params;
  void *shader;
  uint16_t nameSymbol;
  std::memcpy(&params, bytes + kMaterialParams, sizeof(params));
  std::memcpy(&shader, bytes + kMaterialShader, sizeof(shader));
  std::memcpy(&nameSymbol, bytes + kMaterialName, sizeof(nameSymbol));
  const uint8_t count = bytes[kMaterialCount];
  if (!params || !count || !shader) {
    g_stats.noParams.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (g_entries.size() > 32768)
    g_entries.clear();
  Entry &e = g_entries[material];
  if (e.params != params || e.count != count || e.shader != shader || e.nameSymbol != nameSymbol) {
    e = Entry{};
    e.params = params;
    e.count = count;
    e.shader = shader;
    e.nameSymbol = nameSymbol;
    resolve(e, material);
  }
  g_currentEntry = &e;
  g_currentName = subRectName ? subRectName : e.name.c_str();
  {
    void *flagsVar = paramAt(e, e.flags);
    const int flags = flagsVar ? g_varInt(flagsVar) : 0;
    g_bound.known = true;
    g_bound.flags = flags;
    g_bound.decal = (flags & kFlagDecal) != 0;
    g_bound.decalShader = e.decalShader;
    g_bound.modulate = e.modulate;
    g_bound.lightmapped = e.lightmapped;
    g_bound.vertexColor = (flags & kFlagVertexColor) != 0;
    g_bound.vertexAlpha = (flags & kFlagVertexAlpha) != 0;
    g_bound.translucent = (flags & kFlagTranslucent) != 0;
    g_bound.additive = (flags & kFlagAdditive) != 0;
    g_bound.alphaTest = (flags & kFlagAlphaTest) != 0;
    g_bound.subRect = subRectName != nullptr;
    g_bound.shader = e.shaderName.c_str();
    if (g_bound.decal || g_bound.decalShader)
      g_stats.decals.fetch_add(1, std::memory_order_relaxed);
  }
  if (e.kind != Shading::Lit)
    read(e, g_current);
  // Draws are matched to the material by its base texture as well
  // (remix::onMaterialBound): some reach D3D while another one is bound.
  if (e.base >= 0)
    if (void *var = e.params[e.base]; var && *static_cast<uintptr_t *>(var) == g_materialSystem.base + kVarVtable)
      g_current.base = textureName(e.baseTexture, var);
  if (e.pbrCapable && pbr::enabled()) {
    void *flagsVar = paramAt(e, e.flags);
    readPbr(e, flagsVar ? g_varInt(flagsVar) : 0, g_current);
  }
  remix::onMaterialBound(g_current, e.name.c_str());
  if (g_current.mode == Shading::Lit)
    return;
  switch (g_current.mode) {
  case Shading::Unlit: g_stats.unlit.fetch_add(1, std::memory_order_relaxed); break;
  case Shading::SelfIllumBaseAlpha: g_stats.baseAlpha.fetch_add(1, std::memory_order_relaxed); break;
  case Shading::SelfIllumMask: g_stats.mask.fetch_add(1, std::memory_order_relaxed); break;
  default: g_stats.maskAlpha.fetch_add(1, std::memory_order_relaxed); break;
  }
  std::lock_guard<std::mutex> lock(g_stats.lock);
  g_stats.entries = g_entries.size();
  if (g_stats.seen.size() < 256 || g_stats.seen.count(e.name)) {
    Seen &seen = g_stats.seen[e.name];
    seen.shader = e.shaderName;
    seen.mode = g_current.mode;
    std::copy_n(g_current.tint, 3, seen.tint);
    seen.maskScale = g_current.maskScale;
    seen.mask = g_current.mask;
    ++seen.binds;
  }
}

const Shading &current() { return g_current; }

const char *currentName() { return g_currentName; }

const Bound &bound() { return g_bound; }

bool currentIsLitOpaque() {
  const Entry *e = g_currentEntry;
  if (!e)
    return false;
  bool lit = false;
  for (const char *shader : {"lightmappedgeneric", "vertexlitgeneric", "worldvertextransition", "lightmapped_4wayblend"})
    lit |= _strnicmp(e->shaderName.c_str(), shader, std::strlen(shader)) == 0;
  if (!lit)
    return false;
  void *flags = paramAt(*e, e->flags);
  return flags && !(g_varInt(flags) & kFlagAdditive);
}

std::string currentTextureName(const char *name) {
  const Entry *e = g_currentEntry;
  if (!e)
    return {};
  for (int i = 0; i < e->count; ++i) {
    void *var = paramAt(*e, i);
    if (!var || varType(var) != kTypeTexture)
      continue;
    const char *varName = g_varName(var);
    if (!varName || _stricmp(varName, name) != 0)
      continue;
    void *texture;
    std::memcpy(&texture, static_cast<const uint8_t *>(var) + kVarTexture, sizeof(texture));
    return textureNameOfTexture(texture);
  }
  return {};
}

bool currentFloat(const char *name, float &out) {
  const Entry *e = g_currentEntry;
  if (!e)
    return false;
  for (int i = 0; i < e->count; ++i) {
    void *var = paramAt(*e, i);
    if (!var)
      continue;
    const uint8_t type = varType(var);
    if (type != kTypeFloat && type != kTypeVector && type != kTypeInt)
      continue;
    const char *varName = g_varName(var);
    if (!varName || _stricmp(varName, name) != 0)
      continue;
    const float value = g_varFloat(var); // a vector's first component, an int as a float
    if (!std::isfinite(value))
      return false;
    out = value;
    return true;
  }
  return false;
}

bool currentVector(const char *name, float out[3]) {
  const Entry *e = g_currentEntry;
  if (!e)
    return false;
  for (int i = 0; i < e->count; ++i) {
    void *var = paramAt(*e, i);
    if (!var || varType(var) != kTypeVector)
      continue;
    const char *varName = g_varName(var);
    if (!varName || _stricmp(varName, name) != 0)
      continue;
    const float *v = g_varVec(var);
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
      return false;
    std::copy_n(v, 3, out);
    return true;
  }
  return false;
}

void describeCurrent(FILE *out, const char *indent) {
  const Entry *e = g_currentEntry;
  if (!e) {
    fprintf(out, "%s(material not read)\n", indent);
    return;
  }
  Entry::TextureName texture;
  for (int i = 0; i < e->count; ++i) {
    void *var = paramAt(*e, i);
    if (!var)
      continue;
    const uint8_t type = varType(var);
    const char *name = g_varName(var);
    if (!name || type == kTypeUndefined)
      continue;
    if (type == kTypeFloat) {
      fprintf(out, "%s%s %g\n", indent, name, g_varFloat(var));
    } else if (type == kTypeInt) {
      fprintf(out, "%s%s %d\n", indent, name, g_varInt(var));
    } else if (type == kTypeVector) {
      const float *v = g_varVec(var);
      fprintf(out, "%s%s [%g %g %g %g]\n", indent, name, v[0], v[1], v[2], v[3]);
    } else if (type == kTypeTexture) {
      fprintf(out, "%s%s texture %s\n", indent, name, textureName(texture, var).c_str());
    }
  }
}

void dumpStatistics(FILE *out) {
  const char *detail = g_detail.load();
  fprintf(out, "engine materials: %s%s%s\n", g_status.load(), *detail ? ": " : "", detail);
  if (!g_installed)
    return;
  static ULONGLONG last = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  const double seconds = now > last ? double(now - last) / 1000 : 1;
  last = now;
  const double baseAlpha = g_stats.baseAlpha.exchange(0) / seconds, mask = g_stats.mask.exchange(0) / seconds,
               maskAlpha = g_stats.maskAlpha.exchange(0) / seconds;
  fprintf(out,
          "   binds per second %.0f: unlit %.1f, self-illuminated %.1f (base alpha %.1f, $selfillummask %.1f, envmap "
          "mask alpha %.1f) | not read: other material classes %.1f, without params %.1f, foreign vars %.1f, mask "
          "var without a texture %.1f\n"
          "   decal materials bound per second ($decal or a Decal* shader) %.1f | sub-rectangle materials (decal "
          "atlases) %.1f, read through their page: %s; without a page %.1f\n",
          double(g_stats.binds.exchange(0)) / seconds, double(g_stats.unlit.exchange(0)) / seconds,
          baseAlpha + mask + maskAlpha, baseAlpha, mask, maskAlpha, double(g_stats.otherClass.exchange(0)) / seconds,
          double(g_stats.noParams.exchange(0)) / seconds, double(g_stats.foreignVar.exchange(0)) / seconds,
          double(g_stats.noMaskTexture.exchange(0)) / seconds, double(g_stats.decals.exchange(0)) / seconds,
          double(g_stats.subRects.exchange(0)) / seconds, g_subRects ? "yes" : "no (layout not recognized)",
          double(g_stats.subRectNoPage.exchange(0)) / seconds);
  std::lock_guard<std::mutex> lock(g_stats.lock);
  fprintf(out, "   materials seen since launch, by shader (%zu known now):", g_stats.entries);
  for (const auto &[shader, count] : g_stats.shaders)
    fprintf(out, " %s %u", shader.empty() ? "(none)" : shader.c_str(), count);
  fprintf(out, "\n");
  // The most bound first.
  std::vector<const std::pair<const std::string, Seen> *> order;
  for (const auto &item : g_stats.seen)
    order.push_back(&item);
  std::sort(order.begin(), order.end(),
            [](const auto *a, const auto *b) { return a->second.binds > b->second.binds; });
  static const char *const modes[] = {"lit", "unlit", "selfillum base alpha", "selfillum mask", "selfillum envmap mask alpha"};
  for (size_t i = 0; i < order.size() && i < 24; ++i) {
    const Seen &s = order[i]->second;
    fprintf(out, "     %s (%s): %s, tint %.2f %.2f %.2f", order[i]->first.c_str(), s.shader.c_str(), modes[s.mode],
            s.tint[0], s.tint[1], s.tint[2]);
    if (!s.mask.empty())
      fprintf(out, ", mask %s x %.2f", s.mask.c_str(), s.maskScale);
    fprintf(out, ", %.1f binds/s\n", double(s.binds) / seconds);
  }
  if (order.size() > 24)
    fprintf(out, "     ... and %zu more\n", order.size() - 24);
  g_stats.seen.clear();
}

std::string textureNameOf(void *texture) { return textureNameOfTexture(texture); }

} // namespace proxy::engine_materials
