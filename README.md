> This project uses AI-generated code

# Garry's Mod RTX Remixed 2
This is currently a proxy `shaderapidx9.dll` for Garry's Mod that wraps the
Source shader API (and later D3D9) to submit the game's rendering to
[OpenRemix](https://github.com/OpenRenderCollective/OpenRemix) through the Remix C API. 

This removes the need to force the engine to run in the fixed function pipeline, so all shader-based systems are supported and are translated when applicable. OpenRemix has its own rasterization pass for sprites and other effects that would not work well in path-tracing and can be integrated seamlessly.

RTX Remix mods from the old version of the project will be supported, and also any lua stools like the Remix API lights, etc.

Multiplayer works out of the box since everything lives in the wrapper.

Other source game support is planned

Source normal maps, Phong parameters and reflection masks are converted to PBR
when available. Materials without that information explicitly use roughness 1
in Forward+ and keep diffuse-only shading in path tracing. Authored PBR values,
water and shared USD replacements retain their own roughness and metalness.
The conversion reuses existing textures; it does not generate missing surface detail.

## Forward+ development handoff

Forward+ development uses `forward-plus-renderer` in this repository and OpenRemix.
Start with the native renderer's
[current handoff](https://github.com/OpenRenderCollective/OpenRemix/blob/forward-plus-renderer/docs/features/forward-plus-handoff.md),
[lighting architecture](https://github.com/OpenRenderCollective/OpenRemix/blob/forward-plus-renderer/docs/architecture/forward-lighting.md)
and [roadmap](https://github.com/OpenRenderCollective/OpenRemix/blob/forward-plus-renderer/docs/features/forward-plus-roadmap.md).
The handoff distinguishes pushed source from the older playable installation.

Source shader names, VMT interpretation, entity classification and light-style
translation belong in this adapter. Lighting providers and the standard material
shading interface belong in the game-agnostic native renderer. Opt-in adapter
material programs and HLSL/single-file shader authoring are planned, not implemented;
the existing PBR conversion and shared replacement system remain the normal path.

## Build
See the [fresh-clone setup](https://github.com/OpenRenderCollective/OpenRemix/blob/forward-plus-renderer/docs/features/forward-plus-setup.md)
for toolchains, optional SDKs and native runtime builds. Linux cross-build requires
MinGW-w64 GCC, Meson/Ninja and `glslangValidator` on PATH. With sibling clones and
SDL dependencies built by OpenRemix's tracked Linux dependency script:

```sh
native=$(realpath ../OpenRemix)
meson setup --cross-file cross/x86_64-w64-mingw32.txt _build \
  "-Dopenremix_include=$native/public/include" \
  "-Dsdl_include=$native/_work/deps/linux/prefix/include"
meson compile -C _build
```

The tracked convenience helper runs the same Linux cross-build with checked header
paths and tools:

```sh
python3 tools/build_proxy.py --dry-run
python3 tools/build_proxy.py --jobs 4
```

Defaults use the sibling `OpenRemix` clone and its `_work/deps/linux/prefix/include`.
Use `--native-root`, `--sdl-include` and `--build` for another layout. It can be
called from any working directory, downloads nothing and does not deploy.

Deploy: rename `bin/win64/shaderapidx9.dll` to `shaderapidx9_orig.dll` and copy
`_build/shaderapidx9.dll` in its place.

## Isolated live-game testing (Windows)

`tools/stage_game.py` prepares a separate `gm_construct`/`gm_flatgrass` test installation. It
copies selected engine binaries, base VPK packages, loose scripts/assets and the
built renderer into a new directory. Allow several GB of disk space. Existing
destinations are refused, and installed files are never replaced or linked.
The source installation must already contain `shaderapidx9_orig.dll`.

```powershell
# Replace the game path with your own installation.
python tools/stage_game.py --source 'C:/path/to/GarrysMod' `
  --destination _work/staging/gmod-forwardplus `
  --proxy _work/build/forwardplus/shaderapidx9.dll `
  --runtime ../OpenRemix/_work/build/windows/src/native/remix.dll `
  --sdl ../OpenRemix/_work/deps/windows/prefix/bin/SDL3.dll
```

This layout supports the minimal native runtime without OpenUSD; builds with
additional runtime dependencies need those DLLs supplied separately. It uses the
installed engine and loose scripts, so it does not establish a pristine Steam
baseline. Addons, native Lua modules, Workshop content and user configuration
directories are excluded from copying.

The first captured sky remains the static map-load lighting environment. Its
RGBA8 readback can differ by sparse capture noise between launches.
The wrapper stores a reference under the bake cache's `sky/` directory and reuses
its actual pixels only when every RGB difference is at most four levels, alpha is
unchanged, and the mean RGB difference is at most 1/8 of a level. Larger changes,
dimensions or lighting-scale changes invalidate the reference. The runtime still
hashes the exact image used for transport. The live sky remains animated.
`OPENREMIX_PROXY_BAKE_SKY_CACHE=0` disables reference reuse. Cache files include
dimensions, format, scale and a payload checksum; malformed files fall back to
the current capture. `REMIX_BAKE_CACHE_DIR` controls the shared cache location.
For controlled renderer studies, `OPENREMIX_PROXY_BAKE_SKY_REFERENCE` names an explicit `.sky`
file. This diagnostic uses its exact pixels regardless of current capture noise, after checking
shape, format, lighting scale and checksum. It never rewrites the file. An invalid reference
keeps the bake sky unavailable and reports the error; it does not silently use live pixels.
Unset this variable for ordinary play. It affects the bake environment, not the animated live sky.
The stage chooses the stock Kleiner player/hands on spawn, so account preferences
for unavailable Workshop models cannot add missing-model geometry to the capture.
This fixture does not change the account's model preference.

Launch from the staged directory with `REMIX_CONFIG_DIR` set to its
`runtime-config`, `OPENREMIX_PROXY_CONTENT_ROOT` set to the stage,
`OPENREMIX_PROXY_BACKEND=forwardplus`, and `OPENREMIX_PROXY_MODS=0`:

```powershell
./gmod_win64.exe -game garrysmod -insecure -noaddons -noworkshop -novid `
  -windowed -w 960 -h 540 -condebug -conclearlog +sv_lan 1 +map gm_construct
```

The proxy diagnostics report backend selection and bake progress. Optional
`OPENREMIX_PROXY_LOG_ENGINE_ERRORS=1` also records engine startup error messages
before forwarding them to the engine's normal error handler.

`OPENREMIX_PROXY_QUERY_PROFILE=1` adds event, occlusion and other D3D9 query
rates to `bin/win64/shaderapi_proxy_slots.txt`: issued queries, polls, pending
results, failures, flush requests and time inside `GetData`. This is opt-in
instrumentation, not a performance baseline. Time excludes the engine's sleeps
between polls and does not measure query lifetime. Query arguments and driver
results remain unchanged; separate resource vtables retain their own methods.

`OPENREMIX_PROXY_FRAME_PROFILE=<new CSV path>` records host frame intervals and
completed native CPU/GPU timings. Appended focus fields distinguish the Source
host window, any foreground window in the game process, and the native output's
focus/minimized/drawable state. Reject unfocused or minimized intervals when
comparing gameplay performance; a failed `windowStatus` is unknown state. GPU
readbacks can repeat across host frames, so use `gpuNew` for GPU timing summaries.
The appended `cpuStatus` and `bakeStatus` retain each statistics API return code.
A failed query is unknown; zero-filled CPU/bake fields are not evidence of a zero-cost
frame, absent lighting or a rebake.
`gpuTruncated` reports omitted timing ranges. `probeGatherAvailable` distinguishes a
recorded selected-probe visibility preparation range from ordinary reuse, and
`probeGatherMs` records its inclusive GPU time. A missing range is not a measured
zero-cost gather; reject unavailable or truncated observations before drawing that
conclusion. As with other pass timers, neighbouring GPU stages can overlap.
The profiler observes focus without changing it and performs no window/API calls
or file writes when disabled. A forced process stop may leave a truncated final
CSV record; discard that trailing fragment rather than treating it as a frame.

Static bake readiness follows materials referenced by retained brush, overlay,
terrain and prop geometry. Unused BSP material-table entries and engine-owned sky
geometry do not block publication. Required unresolved materials still block it;
unsupported shader inputs need an explicit bake representation rather than a
placeholder marked as ready.

### Persistent testing installation

`tools/deploy_testing.py` installs a complete Windows runtime package into an
isolated host and updates it without resetting game settings, mods or bake caches.
Create a local `_work/testing-install.json` with absolute paths, for example:

```json
{
  "source": "E:/SteamLibrary/steamapps/common/GarrysMod",
  "target": "E:/SteamLibrary/steamapps/common/GarrysModWithRTX2",
  "proxy": "D:/garrys-mod-rtx-remixed-2/_work/build/forwardplus/shaderapidx9.dll",
  "package": "D:/OpenRemix/_work/packages/your-verified-runtime",
  "native_repo": "D:/OpenRemix",
  "mods": "D:/your-generated-mods"
}
```

The optional `mods` directory seeds a private copy on the first deployment only.
Build the package with OpenRemix's `scripts/windows/package-runtime.py`; its
manifest and every dependency hash are checked before deployment. Run:

```powershell
python tools/deploy_testing.py --settings _work/testing-install.json
python tools/deploy_testing.py --settings _work/testing-install.json --verify
```

The initial target must be absent or contain only `RTXLauncher.Avalonia.exe`
bootstrap files. The base host includes Construct and Flatgrass; copy additional
maps/content into its own `garrysmod` directory. The generated `Play OpenRemix.cmd`
launches Forward+ with local configuration, cache, USD replacements and packaged
dependencies. It accepts game arguments, for example `+map gm_construct`.
`Update OpenRemix.cmd` reruns the deployment using the local settings file.
It deploys built files; it does not compile source. Updates refuse a running test
host, back up replaced files under `openremix-backups`, and record installed hashes
and source revisions in `openremix-install.json`. Unrelated files are preserved.

For continued renderer development on this workstation, use `GarrysModWithRTX2`
as the persistent testing target. After validating a new runtime, select its
package in `_work/testing-install.json` and redeploy when this host is closed.
The launcher does not install forced cameras, moving test props or quit timers.


## Standalone BSP diagnostics (Windows)

`replay.width = 1920` and `replay.height = 1080` select a fixed client/render
resolution; each dimension accepts integers from 64 through 4096. The default
is 640 x 360. The camera aspect ratio follows the requested dimensions. The
host fails if Windows cannot provide the requested client size; GPU timing
reports also record the actual render and output sizes for verification.
The diagnostic window permits sizes larger than the desktop for 4K captures.

Optional replay settings `replay.measureFrames = 240` and
`replay.warmupFrames = 600` collect a bounded steady-state GPU timing window.
`replay.warmupSeconds = 5` also requires five seconds of warmup (default: three).
The host waits for the selected backend, requested denoiser/TAA, complete materials
and (for Forward+) an active bake with any authored cubemap edit fully published.
It also requires `remixapi_GetPreparationStatisticsEXT`: loading and preparation must
finish, all requested pipelines must complete, and compilation failures reject capture.
Preparation restarting after warmup rejects the run; a continuous preparation wait is
bounded to five minutes. An adjacent `.preparation.json` records the final readiness.
Older runtimes without this independent export are rejected for measurement/camera paths.
Manual fixed-frame runs remain available with a warning that preparation is unverifiable.
For offline validation with the older 1.3.280 layer, explicitly setting
`rtx.native.lowLatency = 0` avoids its observed Reflex external-timeline tracking warning.
Record that scope; it does not change game defaults or establish pacing/performance acceptance.
Use a newer validation layer to validate Reflex itself; do not suppress general sync diagnostics.
Reflection capture failures reject the measurement. Older runtimes without the optional
bake-statistics readiness tail cannot verify an authored sidecar and are rejected for that case.
Camera paths use the same reflection readiness gate, and completed measurements request a fresh
probe report so diagnostics describe the final image rather than initial diffuse publication.
`replay.waitForBake = 0` permits explicit
unbaked experiments. Warmup accepts distinct completed GPU frames; duplicate or
unavailable results are ignored. Backend, bake-hash and resolution changes restart
the window. Both the frame and elapsed-time warmup requirements must be met;
actual warmup can exceed the requested frame count. Warmup must be at least 16
frames, both counts are capped at 10000, and warmup time is bounded to 0–60 seconds.
Measurement mode removes the normal replay's artificial pause between frames.

The output basename receives `.timings.csv` raw samples and `.timings.json` mean,
median, nearest-rank p95, resolution, bake identity and per-pass summaries. Pass
inclusive/self times can overlap through nesting; do not sum the pass rows into
a frame total. The full-frame GPU duration is reported separately. Reports include
host observation timestamps; GPU query results trail execution by several frames.
When the runtime exports CPU diagnostics, measurement mode also writes `.host.csv`.
It records host submission/Present wall time and unique completed CPU Present
snapshots, including record, fence, setup, release and raster preparation times.
Rows carry readiness, backend, bake identity and CPU attempt IDs. Startup rows are
included: select ready rows inside the `.timings.json` observation interval for
steady-state comparisons. CPU snapshots and delayed GPU results are independent;
do not join their rows by position or sum nested CPU phases. Setup may include
handoff/waiting rather than renderer-thread computation. The CSV refuses an
existing output file and leaves installed game files untouched.

These are GPU timings for the static replay, not game FPS or a quality-matched
comparison between rendering techniques. The host exits after the window and a
short screenshot drain; the command-line frame limit remains a timeout bound.
Incomplete runs fail without emitting a successful timing summary. Use a fresh
output basename for each run and preserve its settings alongside the reports.

The `bsp-replay` target reads installed Source content through the same BSP,
VPK, VMT and VTF loaders as the proxy, without replacing any game DLLs. It
opens a preview (640 x 360 by default) at the map's first player spawn, selects Forward+,
publishes complete static bake inputs, prints bake progress, and captures a
frame near the end of the requested frame budget. An optional final settings
file accepts `key = value` lines (and `#` comments):

```powershell
bsp-replay.exe runtime/remix.dll "E:/SteamLibrary/steamapps/common/GarrysMod" gm_flatgrass output.png 1800
```

To exercise map transitions without restarting the renderer, provide the first
stage's frame budget and settings file, then append up to seven triples of
`map output.png settings.cfg`. Each stage unloads its BSP, lights, materials and sky
before the next load; the window and runtime stay alive. Outputs must be distinct.
The same frame budget applies to each stage. Runtime configuration carries between
stages, so each settings file should explicitly set the options under comparison.
Each stage retains its normal readiness, capture and diagnostic checks; this is a
static relay transition test, not live engine `changelevel` or gameplay timing.

Keep the runtime filename `remix.dll`: the production BSP adapter resolves
optional extensions by that module name. For comparison builds, use separate
directories rather than renaming the DLL; replay rejects renamed runtimes.

The runtime's SDL DLL must be on `PATH`. Use an isolated `REMIX_CONFIG_DIR`
(or OpenRemix's `tests/isolated_config.py`) to preserve your normal profiles.
The content root is read-only; `OPENREMIX_PROXY_CONTENT_ROOT` supplies that
same override when embedding the loader in another host. Search paths are
initialized once per process.

Standalone replays can mount other installed Source games through
`OPENREMIX_PROXY_CONTENT_MOUNTS`, a semicolon-separated list of absolute game
content directories (for example, `E:/SteamLibrary/steamapps/common/Half-Life 2 Deathmatch/hl2mp`).
Each root supplies loose files and its original `*_dir.vpk` archives without
copying assets. Listed roots are searched in order, after loose addons and
before stock GMod archives; loose files precede archives within each root.
Set the list before starting replay. This is useful when a map references
game-specific materials that GMod's shared content does not contain.

This is a static map diagnostic, with static VMT shading and textures. Engine
material proxies, gameplay entities, particles, game UI and the engine's sky
rendering are absent. Static sky lighting is supported: the relay loads the map's six skybox
textures into a linear-filtered dome for rendering and baking. LDR, floating-point/integer HDR and
single-texture RGBS HDR skies are supported. Procedural painted skies require a validated frozen
live capture through `OPENREMIX_PROXY_BAKE_SKY_REFERENCE`. Animated sky proxies and the legacy
three-texture HDR encoding remain unsupported and are reported explicitly.
`replay.styledLights=1` includes authored controllable
lights at `replay.lightStyleValue`; it does not simulate engine lightstyle animation.
`replay.freezeAnimation=1` captures animated textures at frame zero; otherwise
they remain unresolved. Missing
assets and incomplete static props keep baking pending; direct lighting can
still render. A successful screenshot alone does not establish that baking
finished: check the logged bake state (`3`, active `1`). Increase the frame
budget when a map is still baking. Rounded, nearly collinear BSP fan slivers can
have reversed lightmap winding. The importer preserves valid contiguous triangle
ranges as surface charts and leaves only those tiny slivers on probes, without
changing render or shadow geometry. The repair permits at most 0.0001 luxel of UV
altitude; substantial folds and invalid mappings retain whole-face probe fallback.
Native overlap validation remains unchanged.


The replay reads the BSP world's sky name through the same loose/VPK/map-pak search paths as
other assets. Plain LDR faces retain an sRGB dome with linear-light interpolation. HDR materials
prefer `$hdrcompressedtexture` (linear RGB times alpha times eight), then `$hdrbasetexture`;
the latter supports half/float RGB(A), Source's sixteen-times integer HDR scale, or sRGB LDR
formats. HDR and colour-multiplied faces use an immutable linear float dome, preserving values
above one for both presentation and frozen baking. This requires a runtime supporting the new
linear float texture upload formats. VTF image resource offsets are respected for float/integer
HDR; malformed, animated or unsupported HDR declarations are reported rather than silently
falling back to a different sky. The legacy three-texture HDR encoding, dynamic material proxies
and server sky overrides are outside this static replay.

The HDR encoding/scaling follows Valve's published
[sky shader](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/materialsystem/stdshaders/sky_hdr_dx9.cpp)
and [RGBS decode](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/materialsystem/stdshaders/sky_hdr_compressed_rgbs_ps2x.fxc).
Sky tint is applied in linear light before dome sampling; this is not engine material-proxy playback.

For image-quality studies, start from `tools/replay_quality.cfg`, add a fixed camera and
asset/mod paths, and record exposure, SSR and sky-reference settings explicitly. This uses the
runtime's normal 256-sample, three-bounce transport and 32768-probe budget. The preset also
enables static mesh lightmaps, matching the current runtime default; dynamic objects retain
probe lighting. A cheap smoke-test configuration is not a quality reference: sparse probes can
leak direct sun, and one-bounce cubemap transport can miss indirect ceiling illumination even at
higher face resolution.
Separate transport convergence from resolution, parallax and material-response changes. Verify
final readiness, actual probe density and completed authored captures before comparing images.
The preset does not disable Reflex; offline validation with the older layer needs the explicit
scope described below. Finite-sample path-traced images also require convergence checks.

The final replay log includes actual tracing samples, bounces, accumulation frames,
sample sequence and scene generation when the runtime exposes tracing statistics.
An accumulation setting alone does not establish that a reference image converged;
check the actual history and shader preparation before accepting comparisons.

For procedural skies such as Construct's `painted`, set
`OPENREMIX_PROXY_BAKE_SKY_REFERENCE` to an existing frozen live `.sky` capture.
The replay validates its dimensions, format, scale and checksum, reads it without
modifying it, and uses it for the visible and baked sky. Invalid explicit references
fail the run. `OPENREMIX_PROXY_SKY_DOME_SCALE` defaults to 1 and must match the
reference's recorded scale. Without a usable sky, the replay prints a lighting
fidelity warning; the background fallback is not an in-game lighting reference.
The quality preset sets `replay.requireSky=1`, so a missing sky fails before
rendering or baking instead of producing a misleading quality capture. Quick
diagnostic runs retain the default `replay.requireSky=0` fallback.

Merged 3D-skybox props are clipped by triangle against playable BSP regions,
like the merged sky world geometry. Testing only a prop's origin cannot prevent
a large tree card from extending into a room. Clipped base meshes retain their
model-space transform, material attributes and original replacement identity.
Untouched placements share the original mesh. Temporary full vertex attributes
are released after loading; changed placements own their meshes until map unload.

Static props preserve both alpha testing and ordinary alpha blending. An
alpha-tested material may also set `$translucent`; texture alpha still selects
cutout coverage, following Source's `EvaluateBlendRequirements`. Constant or
vertex alpha can independently require blending. Additive effects and unsupported
material operations remain with the engine and keep complete baking pending.
The native transport samples source-over sheets as thin coverage, including
vertex-alpha terrain layers. BSP water opts into smooth optical bake boundaries:
constant refraction/absorption is captured while normal-map ripples stay live.
This does not add primary water rendering or refractive caustics to Forward+.
Water replacements automatically match the water material's normal-map upload
hashes, with the older `dev/dev_monitor` compatibility keys as fallbacks. Both
legacy and current hashes of the supported mip variants are included. SDK
material IDs remain distinct, so bodies that share a normal map can keep their
own source fog properties. `OPENREMIX_PROXY_WATER_KEY=<hex>` explicitly selects
one key; `0` disables those automatic water replacement keys.
For load-time bake experiments, a replay settings file can raise
`rtx.native.forward.bakeBatch` to `1024`; retain isolated configuration so this
experiment does not become a game preference.


Replay settings can select a reproducible inspection view in Source world units:

```ini
replay.position = -1792 -1536 -80
replay.angles = 0 0 0
replay.fov = 65
rtx.native.forward.bakeBatch = 1024
```

Positions override the eye directly; angles are Source pitch/yaw/roll in degrees.
For repeatable moving-view checks, add ordered camera keys instead:

```ini
replay.cameraKey = 0 -2300 -1600 -80 0 0 0
replay.cameraKey = 128 -1700 -1600 -80 0 0 0
replay.cameraCaptureInterval = 4
```

Each key contains a frame index, eye XYZ and pitch/yaw/roll. The first index must
be zero; indices increase through at most 1024, with at most 64 keys. Positions
interpolate linearly and angles take the shortest route. Motion starts after a
completed bake, authored-reflection publication, material readiness and `replay.warmupFrames`
consecutive ready frames, including renderer preparation. A changed bake or restarted preparation
stops the run. The tool saves a numbered image every
`replay.cameraCaptureInterval` path frames (default 32) and at the endpoint, a final ordinary
output image, and an adjacent
`.path.csv` trace. With `REMIX_CONFIG_DIR` set, it requests a fresh endpoint probe
report and fails if that report does not arrive during the drain. Check its
`authoredReflections` state before interpreting authored coverage; main-bake
readiness alone does not prove reflection-only publication has finished.
The interval must be an integer in [1,1024]; at most 64 path screenshots are allowed, including
the endpoint. Increase the interval for longer paths. Dense captures drain a previous pending
readback with a bounded retry, without advancing the camera or adding Present calls. Capture
readbacks can stall the host, so path-frame motion is not a real-time speed or FPS measurement.
This bounded diagnostic is separate from fixed-view GPU
measurements: `replay.measureFrames` must be zero and `replay.waitForBake` must
remain enabled. It does not simulate collisions, gameplay or material proxies.

Diagnostic spheres (`replay.probeReceiver = x y z radius`) default to matte,
world-fixed receivers. For reflection checks, use `replay.receiverRoughness = .15`
and `replay.receiverMetallic = 1`. Both scalars accept [0,1]. With
`replay.receiversFollowCamera = 1`, the sphere coordinates become camera-relative
forward/right/up offsets; for example `replay.probeReceiver = 64 0 -8 16` keeps a
reflective sphere ahead of a moving camera. These are ordinary dynamic world
instances, with world depth and shadows, excluded from the frozen static bake.
They are diagnostic objects, not viewmodels or authored map replacements.
`replay.receiverSegments` controls reference-sphere tessellation: an even integer in [8,256],
default 32 longitude segments and half as many latitude rings. Use 128 for close-up lighting
comparisons to reduce low-poly self-shadow terminator artifacts before attributing differences
to probe reconstruction. The chosen detail is logged; the default preserves existing fixtures.

For a dense local irradiance reference, set `replay.probeBounds` to six finite
Source coordinates: minimum XYZ followed by maximum XYZ. Each maximum must be
greater than its minimum. The replay retains the complete geometry, occluders,
surface charts and lights, but allocates the probe grid within this domain and
keeps only reflection seeds inside it. Combine it with a smaller
`rtx.native.forward.bakeSpacing` in an isolated configuration and cache directory.
Lighting outside the local probe domain is not a valid comparison. This is a
diagnostic reference, not automatic adaptive placement or a gameplay setting.
Set `replay.mods = path/to/mods` to exercise the runtime's shared USD material
replacements, including generated PBR packs. Relative paths resolve beside the
settings file; the runtime must be built with USD support. The replay preserves
the original upload's legacy material key even though its decoded textures have
temporary SDK IDs. The loader's normal mod ordering and lazy texture loading apply.
If `REMIX_CONFIG_DIR` is set, a completed bake also produces an adjacent
`output.probes.json` containing the actual published capture origins, projection
boxes and influence boxes. The runtime's `probes.request` diagnostic generates
this file without GPU readback. Reports and screenshots must be newly written;
existing output files cannot establish a successful replay.

Use `tools/probe_bounds.py` to inspect those published boxes against horizontal
or vertical sections of the map's world brushes (requires Python and matplotlib):

```sh
python tools/probe_bounds.py map.bsp output.probes.json section.png --axis z --slice 64
```

Add `--probe 0` to select a capture or `--receiver x y z` to mark a surface point.
Coordinates are Source units. The image's companion JSON records input hashes and
the selected captures. Use a fresh output path for each inspection. Props, moving
brush models and displaced terrain are omitted; these sections help inspect box
placement but do not establish reflection quality or visibility through doors.

The BSP's playable `env_cubemap` sample positions seed geometry-constrained
reflection fitting through the optional version-4 bake descriptor. Solid origins
are refused, duplicate fits are merged, and remaining capacity uses automatic
placement from empty BSP leaves, with hardware checks against the complete
static geometry. This keeps room placement independent of a coarsened irradiance
grid and avoids capturing from inside props.
Hints retain authored order; `rtx.native.forward.reflectionProbes` remains the
capture budget (maximum 64), so a low budget cannot cover every room.

Experimental receiver assignments: `OPENREMIX_PROXY_CUBEMAP_RECEIVER_BINDINGS=1` connects
resolved VMT `$envmap` capture origins to exact, unique origins in an accepted authored
sidecar. This requires a paired native runtime supporting scene-scoped material reflection
bindings and frame-snapshotted binding-only updates. The adapter requires
`remixapi_GetReflectionBindingVersionEXT` to report version 1 or newer; absent/older
contracts disable explicit bindings with a diagnostic while preserving ordinary volume
selection. This query does not establish hardware or capture readiness. The feature is off by default. Opaque imported
materials use stable VMT-specific variants even when they share a base texture; changing a
volume ID or removing the sidecar updates receiver metadata without replacing their meshes
or diffuse bake inputs. Existing masks, PBR conversion and replacement keys stay in the
material chain. Owned optical, blended and modulated imports are excluded. Missing or
ambiguous assignments use ordinary volume selection. The switch does not import raw
Black Mesa projection boxes, derive dynamic influence bounds, or establish visual parity
with the original renderer. Live host qualification remains required before deployment.
Authored bounds persist across same-map retained geometry refreshes; actual map teardown
clears the request.


For an automatic-placement experiment, launch the replay with
`OPENREMIX_PROXY_REFLECTION_HINTS=0`. The BSP loader then supplies no authored
cubemap sites; the native fitter derives candidates from empty leaves, validates
them with hardware rays, and uses the same boundary/detail constraints. The
published report identifies the resulting captures. Current image-backed Forward+ defaults to
64 captures with 128-square faces. `rtx.native.forward.reflectionResolution` accepts powers of
two from 8 through 512; smaller capture budgets and face sizes remain available. Inspect the
published bounds instead of assuming the budget covers every room.
This toggle is a diagnostic option and does not edit the map or installed game.

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
