> This project uses AI-generated code

# Garry's Mod RTX Remixed 2
This is currently a proxy `shaderapidx9.dll` for Garry's Mod that wraps the
Source shader API (and later D3D9) to submit the game's rendering to
[OpenRemix](https://github.com/OpenRenderCollective/OpenRemix) through the Remix C API. 

This removes the need to force the engine to run in the fixed function pipeline, so all shader-based systems are supported and are translated when applicable. OpenRemix has its own rasterization pass for sprites and other effects that would not work well in path-tracing and can be integrated seamlessly.

RTX Remix mods from the old version of the project will be supported, and also any lua stools like the Remix API lights, etc.

Multiplayer works out of the box since everything lives in the wrapper.

Other source game support is planned

## Build
This is primarily built on Linux, Windows support is largely untested.

    meson setup --cross-file cross/x86_64-w64-mingw32.txt _build
    meson compile -C _build

Deploy: rename `bin/win64/shaderapidx9.dll` to `shaderapidx9_orig.dll` and copy
`_build/shaderapidx9.dll` in its place.

## Launch options
The Video options' **Advanced...** button is replaced by **OpenRemix
Settings...**, which opens OpenRemix's settings panel (also Alt+X, the shortcut set in the panel's header). The
settings the Advanced page held are set on the command line instead (Steam:
Properties, Launch Options), on one line:

    -nod3d9ex +mat_hdr_level 0 +r_rootlod 0 +mat_reducefillrate 1 +r_waterforceexpensive 0
    +r_waterforcereflectentities 0 +r_shadowrendertotexture 0 +r_flashlightdepthtexture 0
    +mat_colorcorrection 0 +mat_antialias 1 +mat_aaquality 0 +mat_trilinear 0 +mat_forceaniso 1
    +mat_vsync 0 +mat_motion_blur_enabled 0

As the Advanced page: model detail High, shader detail Low, water Simple
reflections, shadows Low, colour correction off, no antialiasing, bilinear
filtering, no vertical sync, no motion blur, Windows Aero extensions (Direct3D
9Ex) off, HDR off. Multicore rendering is on by default (`mat_queue_mode -1`).
Texture detail Very High (`mat_picmip -1`) is set by the proxy, as the engine
drops a negative value after a `+` command (`OPENREMIX_PROXY_PICMIP` gives
another value, or `keep` leaves the saved one). Do not add `-dxlevel`: it resets
the saved video settings on every launch (DirectX 9.0+, 95, is the saved
level). `OPENREMIX_PROXY_ADVANCED_VIDEO=1` keeps the Advanced button.
