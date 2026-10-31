#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <windows.h>

#include <cstdio>

// D3D9 interception below the original shader API: patches the original
// module's Direct3DCreate9 import and then vtable entries of the objects it
// creates. Every draw the game issues passes through here.
namespace proxy::d3d {

// Rewrites `module`'s import table entry for dll!name; returns what it held
// (nullptr when the module does not import it).
void *patchModuleImport(HMODULE module, const char *dll, const char *name, void *hook);

// Hooks `module`'s import of d3d9!Direct3DCreate9. Call once, before the
// shader API creates its device.
bool install(HMODULE module);

// Whether render target 0 is the back buffer (render thread): the views the
// proxy takes draws from.
bool backBufferBound();
// The render-target texture render target 0 is (nullptr: the back buffer or
// another surface). Render thread.
const void *boundTargetTexture();
// The scene samples this render-target texture (a scope's lens showing an
// addon's 2D vignette): its 2D draws are rasterized by D3D and its contents
// read back (remix::onTargetPixels) while it stays wanted.
void requestTargetContents(const void *texture);

// A render-target texture's size and D3DFORMAT (false: not one).
bool targetTexture(const void *texture, uint32_t &width, uint32_t &height, uint32_t &format);
// A shader object's D3D9 bytecode (nullptr: not seen at creation).
std::shared_ptr<const std::vector<uint32_t>> shaderCode(const void *shader);

// Writes per-interval draw statistics and resets the interval counters.
void dumpStatistics(FILE *out);

} // namespace proxy::d3d
