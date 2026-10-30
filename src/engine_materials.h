#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "pbr_rules.h"

// What the Source material of a draw does with its base texture besides
// being lit: unlit shaders show it as it is, self-illuminated materials
// ($selfillum) show the masked part of it as it is. Read from the engine's
// material at IShaderAPI::Bind (the material system binds a material, after
// its proxies ran, before the draws that use it), so values that material
// proxies animate ($selfillumtint following the player's weapon colour, $color
// flickers) are current for every draw.
namespace proxy::engine_materials {

struct Shading {
  enum Mode : uint8_t {
    Lit,                // nothing emitted
    Unlit,              // the whole surface: texture x tint (x vertex colour)
    SelfIllumBaseAlpha, // $selfillum: masked by the base texture's alpha
    SelfIllumMask,      // $selfillummask: masked by that texture's colour
    SelfIllumMaskAlpha, // $selfillum_envmapmask_alpha: by the $envmapmask's alpha
  };
  Mode mode = Lit;
  bool vertexColor = false, vertexAlpha = false; // $vertexcolor, $vertexalpha (unlit)
  // The VMT marks an unlit material as luminous ($selfillum on UnlitGeneric,
  // which the shader ignores): BSP materials only.
  bool luminous = false;
  float tint[3] = {1, 1, 1}; // linear multiplier of the emitted colour
  float maskScale = 1;       // emitted share = mask * maskScale (above one: overbright)
  // The surface's albedo times the texture's (sRGB, as the texture's values):
  // ShatteredGlass's edge pieces (their mask x the glass's colour). No
  // opacity: a material opacity below one makes the runtime shade a glass
  // sheet (transmission from the colour), not the draw's alpha blend.
  float albedo[3] = {1, 1, 1};
  std::string mask;          // mask texture ("models/x/y_mask"), SelfIllumMask / MaskAlpha
  std::string base;          // $basetexture's name ("" when unknown), also of Lit materials
  pbr::Material pbr;         // its PBR conversion (pbr_rules), of the lit surface shaders
};

// The shading a VMT's shader name selects: Unlit for the unlit shaders,
// SelfIllumBaseAlpha for the shaders that implement $selfillum (the caller
// checks the material's flags and masks), else Lit.
Shading::Mode shaderMode(const char *shaderName);

// Emitted share of a $selfillumfresnel material, averaged over the directions
// a surface is seen from ($selfillumfresnelminmaxexp).
float fresnelShare(float minimum, float maximum, float exponent);

// Validates the engine's material classes and IShaderAPI::Bind (the real
// shader API's vtable). False: nothing is read, every draw is Lit.
bool install(const void *const *shaderApiVtable);

// The name of an ITexture of the material system, lower case ("" when the
// pointer is not one, or before install). Main thread.
std::string textureNameOf(void *texture);

// IShaderAPI::Bind(IMaterial *), after the engine's (render thread).
void onBind(void *material);

// The bound material's shading (render thread; valid until the next onBind).
const Shading &current();
// Its name, for statistics ("" when unknown): of the material bound, which
// for decals is often a sub-rectangle of an atlas ("decals/concrete/
// shot1_subrect", a CMaterialSubRect: everything else is read from its page,
// "decals/decals_mod2x", the material the draw is made with).
const char *currentName();

// What the bound material says about being a decal (MaterialVarFlags_t, and
// its shader), for remix::onDraw.
struct Bound {
  bool known = false;       // a material was read (else nothing below is set)
  bool decal = false;       // $decal
  bool decalShader = false; // a shader named Decal* (DecalModulate, DecalBaseTimesLightmapAlphaBlendSelfIllum)
  bool modulate = false;    // DecalModulate: dst x src x 2
  bool lightmapped = false; // LightmappedGeneric and the other world shaders
  bool vertexColor = false, vertexAlpha = false, translucent = false, additive = false, alphaTest = false;
  bool subRect = false;     // bound through a CMaterialSubRect
  int flags = 0;
  const char *shader = "";
};
const Bound &bound();
// The bound material is a lit surface shader (LightmappedGeneric,
// VertexLitGeneric, WorldVertexTransition, Lightmapped_4WayBlend) without
// $additive: drawn additively, it is the engine's flashlight pass.
bool currentIsLitOpaque();

// A numeric parameter of the bound material by name ("$overbrightfactor": a
// float or int, or a vector's first component). False when the material was
// not read or does not define it.
bool currentFloat(const char *name, float &out);
// A vector parameter of the bound material by name ("$color"): its three
// components. False when the material was not read or does not define it.
bool currentVector(const char *name, float out[3]);
// The name of the texture the bound material's parameter `name` ("$detail")
// holds, lower case ("" when none, or not a texture).
std::string currentTextureName(const char *name);
// The bound material's defined parameters, one per line (dev dumps).
void describeCurrent(FILE *out, const char *indent);

void dumpStatistics(FILE *out);

} // namespace proxy::engine_materials
