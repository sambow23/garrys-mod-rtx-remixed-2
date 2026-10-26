#include "source_lights.h"
#include "source_rendering.h"
#include "styled_lights.h"
// Standalone diagnostic host for the production BSP loader. It never injects
// into Source or writes installed content. Static VMTs only; no material proxies.
#include "bsp_world.h"
#include "replay_settings.h"
#include "probe_receivers.h"
#include "replay_timing.h"
#include "replay_host_profile.h"
#include "replay_sky.h"
#include "replay_readiness.h"
#include "replay_report.h"
#include <remix/remix_bake_ext.h>
#include <remix/remix_reflection_ext.h>
#include <cstring>
#include <remix/remix_native_ext.h>
#include <remix/remix_usd_ext.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace {
using proxy::engine_materials::Shading;
using Clock = std::chrono::steady_clock;

bool authoredOverridePresent(const char *map) {
  const char *override = std::getenv("OPENREMIX_PROXY_CUBEMAP_BOUNDS");
  const char *root = std::getenv("OPENREMIX_PROXY_CONTENT_ROOT");
  const auto path = override && *override ? std::filesystem::u8path(override)
      : std::filesystem::u8path(root ? root : "") / "garrysmod/data/openremix/cubemaps" /
          (std::string(map) + ".cubemaps.bin");
  return std::filesystem::exists(path);
}

bool reflectionsReady(const remixapi_BakeStatisticsEXT &statistics, const char *map) {
  if (statistics.structSize < sizeof(statistics)) {
    if (authoredOverridePresent(map))
      throw std::runtime_error("Runtime lacks authored reflection readiness statistics");
    return true;
  }
  if (statistics.reflectionFailed)
    throw std::runtime_error("Authored reflection capture failed; refusing an incomplete measurement");
  return !statistics.reflectionRequested || (!statistics.reflectionPending && statistics.reflectionActive);
}

void check(remixapi_ErrorCode result, const char *operation) {
  if (result != REMIXAPI_ERROR_CODE_SUCCESS)
    throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(unsigned(result)));
}

void requestPathScreenshot(remixapi_Interface &api, const std::string &path) {
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  for (;;) {
    const auto result = api.RequestPresentedScreenshot(path.c_str());
    if (result != REMIXAPI_ERROR_CODE_ALREADY_EXISTS) {
      check(result, "Request path screenshot");
      return;
    }
    if (Clock::now() >= deadline) throw std::runtime_error("Previous path screenshot did not drain in 10 seconds");
    // Request waits for the previous CPU frame; PumpEvents can then retire its
    // fenced readback. Do not advance the camera or add extra Present calls.
    check(api.PumpEvents(), "Drain path screenshot");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::filesystem::file_time_type lastWrite(const std::filesystem::path &path) {
  std::error_code error;
  const auto time = std::filesystem::last_write_time(path, error);
  return error ? std::filesystem::file_time_type::min() : time;
}

// Materials and images outlive the retained BSP instances. Missing inputs stay
// unresolved and are reported once, rather than silently replaced for a bake.
class Materials {
public:
  explicit Materials(remixapi_Interface &api, bool reportHashes, bool freezeAnimation)
      : api(api), reportHashes(reportHashes), freezeAnimation(freezeAnimation) {}
  ~Materials() {
    for (auto &[key, material] : materials)
      api.DestroyMaterial(material.handle);
    for (auto &[name, texture] : textures)
      if (texture.handle)
        api.DestroyTexture(texture.handle);
  }
  bool complete() const { return failures.empty(); }
  proxy::bsp::MaterialRef lookup(uint64_t sourceHash, const Shading *shading, bool nearest) {
    std::string base;
    if (!proxy::bsp::materialSource(sourceHash, base)) {
      fail("texture hash " + std::to_string(sourceHash), "no VMT base texture");
      return {};
    }
    // Include every static shading field: two VMTs may share their albedo.
    std::ostringstream key;
    key.precision(9);
    key << base << ':' << sourceHash << ':' << nearest;
    if (shading) {
      const auto &s = *shading;
      if (s.reflectionReceiverKey) key << ":receiver:" << s.reflectionReceiverKey;
      key << ':' << int(s.mode) << ':' << s.mask << ':' << s.maskScale << ':' << s.luminous;
      for (float c : s.tint)
        key << ':' << c;
      key << ':' << s.pbr.active << ':' << s.pbr.normal << ':' << s.pbr.ssbump << ':' << s.pbr.roughness
          << ':' << s.pbr.metallic;
      for (const auto &m : s.pbr.maps)
        key << ':' << m.texture << ':' << m.channel << ':' << m.curve << ':' << m.invert << ':' << m.exponent
            << ':' << m.bias << ':' << m.scale;
    }
    const auto found = materials.find(key.str());
    const uint64_t sceneId = shading ? shading->reflectionSceneId : 0;
    const uint64_t volumeId = shading ? shading->reflectionVolumeId : 0;
    if (found != materials.end() && found->second.sceneId == sceneId && found->second.volumeId == volumeId)
      return {found->second.handle, {}};
    const auto albedo = image(base);
    if (albedo.empty())
      return {};
    std::wstring normal, maps[2], mask;
    if (shading) {
      normal = image(shading->pbr.normal);
      mask = image(shading->mask);
      for (int i = 0; i < 2; ++i)
        maps[i] = image(shading->pbr.maps[i].texture);
      if ((!shading->pbr.normal.empty() && normal.empty()) || (!shading->mask.empty() && mask.empty()))
        return {};
      for (int i = 0; i < 2; ++i)
        if (!shading->pbr.maps[i].texture.empty() && maps[i].empty())
          return {};
    }
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.roughnessConstant = 1; // Match live plain-material exports.
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    opaque.useDrawCallAlphaState = 1;
    // The decoded replay image has a temporary SDK texture ID. Material packs
    // still identify the original Source upload by its dxvk texture hash.
    remixapi_MaterialInfoReplacementHashEXT replacement{};
    replacement.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_REPLACEMENT_HASH_EXT;
    replacement.materialHash = proxy::bsp::textureReplacementHash(base, sourceHash);
    if (reportHashes)
      std::fprintf(stderr, "Material source %016llX, replacement %016llX: %s\n",
                   static_cast<unsigned long long>(sourceHash),
                   static_cast<unsigned long long>(replacement.materialHash), base.c_str());
    opaque.pNext = &replacement;
    remixapi_MaterialInfoEmissionEXT emission = {};
    emission.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT;
    emission.pNext = &replacement;
    remixapi_MaterialInfoSelfIlluminationEXT self = {};
    self.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
    self.pNext = &emission;
    remixapi_MaterialInfoPbrEXT pbr = {};
    pbr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
    pbr.enabled = 1;
    remixapi_MaterialPbrRemapEXT remap = {};
    remap.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_REMAP_EXT;
    remixapi_MaterialNormalEncodingEXT encoding = {};
    encoding.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT;
    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = found != materials.end() ? found->second.hash : nextHash++;
    info.albedoTexture = albedo.c_str();
    info.normalTexture = normal.empty() ? nullptr : normal.c_str();
    info.emissiveTexture = mask.empty() ? nullptr : mask.c_str();
    info.filterMode = nearest ? 0 : 1;
    info.wrapModeU = info.wrapModeV = 1;
    if (shading) {
      const auto &s = *shading;
      if (s.mode != Shading::Lit) {
        opaque.pNext = &self;
        emission.suppressLightGeneration = s.mode == Shading::Unlit && !s.luminous;
        self.maskScale = s.maskScale;
        self.mask = s.mode == Shading::SelfIllumBaseAlpha ? REMIXAPI_SELF_ILLUMINATION_MASK_ALBEDO_ALPHA_EXT
                    : s.mode == Shading::SelfIllumMaskAlpha
                        ? REMIXAPI_SELF_ILLUMINATION_MASK_EMISSIVE_ALPHA_EXT
                        : REMIXAPI_SELF_ILLUMINATION_MASK_NONE_EXT;
        info.emissiveIntensity = 1;
        info.emissiveColorConstant = {s.tint[0], s.tint[1], s.tint[2]};
      }
      if (s.pbr.active) {
        remap.pNext = opaque.pNext;
        pbr.pNext = &remap;
        opaque.pNext = &pbr;
        opaque.roughnessConstant = s.pbr.roughness;
        opaque.metallicConstant = s.pbr.metallic;
        opaque.roughnessTexture = maps[0].empty() ? nullptr : maps[0].c_str();
        opaque.metallicTexture = maps[1].empty() ? nullptr : maps[1].c_str();
        remixapi_MaterialPbrChannelEXT *channels[] = {&remap.roughness, &remap.metallic};
        for (int i = 0; i < 2; ++i) {
          const auto &m = s.pbr.maps[i];
          *channels[i] = {m.channel, m.invert, m.curve, m.exponent, m.bias, m.scale};
        }
        if (!normal.empty()) {
          encoding.encoding = s.pbr.ssbump ? 4 : 2;
          encoding.pNext = opaque.pNext;
          opaque.pNext = &encoding;
        }
      }
    }
    if (!shading || !shading->pbr.active) {
      pbr.enabled = 0;
      pbr.pNext = opaque.pNext;
      opaque.pNext = &pbr;
    }
    remixapi_MaterialReflectionBindingEXT reflection{};
    if (shading && shading->reflectionReceiverKey) {
      reflection = {REMIXAPI_STRUCT_TYPE_MATERIAL_REFLECTION_BINDING_EXT, opaque.pNext,
                    shading->reflectionSceneId, shading->reflectionVolumeId};
      opaque.pNext = &reflection;
    }
    remixapi_MaterialHandle handle = nullptr;
    check(api.CreateMaterial(&info, &handle), "CreateMaterial");
    materials[key.str()] = {handle, info.hash, sceneId, volumeId};
    return {handle, {}};
  }

private:
  struct Texture {
    remixapi_TextureHandle handle = nullptr;
    std::wstring path;
  };
  remixapi_Interface &api;
  bool reportHashes;
  bool freezeAnimation;
  uint64_t nextHash = 0x7265706c00000001ull;
  std::map<std::string, Texture> textures;
  struct Material { remixapi_MaterialHandle handle; uint64_t hash, sceneId, volumeId; };
  std::map<std::string, Material> materials;
  std::set<std::string> failures;
  void fail(const std::string &name, const std::string &reason) {
    if (failures.insert(name).second)
      std::fprintf(stderr, "Incomplete asset: %s (%s)\n", name.c_str(), reason.c_str());
  }
  std::wstring image(const std::string &name) {
    if (name.empty())
      return {};
    if (auto found = textures.find(name); found != textures.end())
      return found->second.path;
    auto &texture = textures[name];
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::string error;
    if (!proxy::bsp::textureImage(name, width, height, rgba, error, freezeAnimation)) {
      fail(name, error);
      return {};
    }
    remixapi_TextureInfoGenerateMipsEXT mips = {};
    mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
    mips.generate = 1;
    remixapi_TextureInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.pNext = &mips;
    info.hash = nextHash++;
    info.width = width;
    info.height = height;
    info.depth = info.mipLevels = 1;
    info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    info.data = rgba.data();
    info.dataSize = rgba.size();
    check(api.CreateTexture(&info, &texture.handle), "CreateTexture");
    wchar_t path[32];
    swprintf(path, 32, L"0x%llX", static_cast<unsigned long long>(info.hash));
    texture.path = path;
    return texture.path;
  }
};
Materials *currentMaterials = nullptr;
proxy::bsp::MaterialRef lookup(uint64_t hash, const Shading *shading, const std::vector<uint64_t> *, bool nearest) {
  return currentMaterials->lookup(hash, shading, nearest);
}

proxy::lighting::SourceLight sourceLight(const proxy::bsp::Light &source) {
  proxy::lighting::SourceLight light = {};
  light.type = source.type;
  std::copy_n(source.position, 3, light.position);
  std::copy_n(source.color, 3, light.color);
  std::copy_n(source.direction, 3, light.direction);
  light.attenuation0 = source.attenuation[0];
  light.attenuation1 = source.attenuation[1];
  light.attenuation2 = source.attenuation[2];
  light.thetaDot = source.thetaDot;
  light.phiDot = source.phiDot;
  return light;
}

void replay(remixapi_Interface &api, HMODULE library, HWND window, const char *map, const char *output,
            unsigned frames, const proxy::replay::Settings &settings) {
  Materials materials(api, !settings.mods.empty(), settings.freezeAnimation);
  if (settings.freezeAnimation)
    std::fprintf(stderr, "Diagnostic scope: animated textures frozen at frame zero; no engine playback/proxies\n");
  currentMaterials = &materials;
  proxy::replay::Sky sky(api);
  struct SceneLifetime {
    remixapi_Interface &api;
    std::vector<remixapi_LightHandle> lights;
    proxy::lighting::StyledLights styled;
    ~SceneLifetime() {
      proxy::bsp::load(api, "");
      styled.reset(api);
      for (auto handle : lights)
        api.DestroyLight(handle);
      currentMaterials = nullptr;
    }
  } scene{api, {}, {}};
  if (!proxy::bsp::load(api, map, settings.styledLights, true))
    throw std::runtime_error("BSP load failed");
  sky.load(proxy::bsp::skyboxImages(), map);
  if (settings.requireSky && !sky.light())
    throw std::runtime_error("Quality replay requires a usable sky; provide the map's sky textures or "
                             "OPENREMIX_PROXY_BAKE_SKY_REFERENCE for procedural skies");
  std::array<float, 3> origin{}, angles{};
  if (!proxy::bsp::playerStart(origin.data(), angles.data()) && !settings.position && settings.cameraPath.empty())
    throw std::runtime_error("map has no player spawn and no replay.position override");
  origin[2] += 64;
  auto params = proxy::replay::camera(settings, origin, angles);
  if (!settings.cameraPath.empty())
    params = proxy::replay::pathCamera(settings, 0);
  std::printf("Camera %.3f %.3f %.3f; FOV %.3f\n", params.position.x, params.position.y, params.position.z,
              params.fovYInDegrees);
  // Match the live wrapper's camera medium; otherwise an underwater replay
  // incorrectly evaluates the water boundary as if the eye were in air.
  remixapi_CameraMediumEXT medium = {};
  medium.sType = REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_EXT;
  const float eye[] = {params.position.x, params.position.y, params.position.z};
  float waterColor[3]{1, 1, 1}, waterDistance = 0;
  if (proxy::bsp::waterAt(eye, waterColor, waterDistance)) {
    medium.inside = 1;
    medium.transmittanceColor = {waterColor[0], waterColor[1], waterColor[2]};
    medium.transmittanceMeasurementDistance = waterDistance;
  }
  params.pNext = &medium;
  std::printf("Camera medium: %s; measurement distance %.3f\n", medium.inside ? "water" : "air",
              medium.transmittanceMeasurementDistance);
  remixapi_CameraInfo camera = {};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &params;
  auto &lights = scene.lights;
  std::vector<remixapi_LightHandle> bakeLights;
  if (sky.light()) bakeLights.push_back(sky.light());
  std::vector<remixapi_BakeStyledLightEXT> styledLights;
  for (const auto &source : proxy::bsp::lights()) {
    const auto authored = sourceLight(source);
    auto current = authored;
    if (source.style)
      for (auto &value : current.color) value *= settings.lightStyleValue;
    const auto handle = proxy::lighting::createLight(api, current, source.hash);
    if (!handle) throw std::runtime_error("Map light creation failed");
    lights.push_back(handle);
    if (source.style) {
      const auto captured = scene.styled.capture(api, authored, source.hash, source.style, 1, 1, settings.lightStyleValue);
      if (!captured.authoredLight) throw std::runtime_error("Styled bake light creation failed");
      styledLights.push_back(captured);
    } else bakeLights.push_back(handle);
  }
  if (settings.styledLights)
    std::printf("Diagnostic styled lights: %zu at multiplier %.3f; immutable full-on bake definitions\n",
                styledLights.size(), settings.lightStyleValue);
  const auto statistics = reinterpret_cast<PFN_remixapi_GetBakeStatisticsEXT>(
      GetProcAddress(library, "remixapi_GetBakeStatisticsEXT"));
  const auto backendStatistics = reinterpret_cast<PFN_remixapi_GetBackendCapabilitiesEXT>(
      GetProcAddress(library, "remixapi_GetBackendCapabilitiesEXT"));
  const auto preparationStatistics = reinterpret_cast<PFN_remixapi_GetPreparationStatisticsEXT>(
      GetProcAddress(library, "remixapi_GetPreparationStatisticsEXT"));
  const auto gpuStatistics = reinterpret_cast<PFN_remixapi_GetGpuPassTimingsEXT>(
      GetProcAddress(library, "remixapi_GetGpuPassTimingsEXT"));
  const auto denoisingStatistics = reinterpret_cast<PFN_remixapi_GetDenoisingStatisticsEXT>(
      GetProcAddress(library, "remixapi_GetDenoisingStatisticsEXT"));
  proxy::replay::TimingWindow measurement(settings.measureFrames, settings.warmupFrames,
                                          settings.warmupSeconds);
  if (!preparationStatistics && (measurement.enabled() || !settings.cameraPath.empty()))
    throw std::runtime_error("Runtime lacks preparation readiness; cannot verify reference or measurement captures");
  if (!preparationStatistics)
    std::fprintf(stderr, "Runtime lacks preparation readiness; fixed-frame output may still be preparing\n");
  bool preparationReadyState = !preparationStatistics;
  remixapi_PreparationStatisticsEXT finalPreparation{};
  Clock::time_point preparationPendingSince{};
  proxy::replay::HostProfile hostProfile(library, output, measurement.enabled());
  if (measurement.enabled()) {
    if (!backendStatistics || !gpuStatistics || (settings.waitForBake && !statistics))
      throw std::runtime_error("Runtime lacks GPU measurement statistics");
    check(api.SetConfigVariable("rtx.native.gpuPassTimers", "1"), "Enable GPU timestamps");
  }
  const char *profile = std::getenv("REMIX_CONFIG_DIR");
  const std::filesystem::path diagnostics =
      profile ? std::filesystem::path(profile) : std::filesystem::path();
  proxy::replay::FreshReport probeReport(diagnostics / "probes.json", diagnostics / "probes.request");
  proxy::replay::FreshReport pipelineReport(diagnostics / "pipelines.txt", diagnostics / "pipelines.request");
  bool requestedPipelines = false;
  bool requestedProbes = false;
  bool requestedPathReport = false;
  unsigned measurementEnd = ~0u;
  uint32_t lastState = ~0u;
  bool reportedPendingInputs = false;
  uint32_t pathWarmup = 0, pathFrame = 0;
  uint64_t pathHash = 0;
  unsigned pathEnd = ~0u;
  bool pathReady = false;
  std::ofstream pathTrace;
  std::map<std::string, std::filesystem::file_time_type> pathSnapshots;
  if (!settings.cameraPath.empty()) {
    if (!statistics) throw std::runtime_error("Camera paths require bake statistics");
    auto trace = std::filesystem::path(output);
    trace.replace_extension("path.csv");
    pathTrace.open(trace);
    if (!pathTrace) throw std::runtime_error("Cannot open camera path trace");
    pathTrace.exceptions(std::ios::failbit | std::ios::badbit);
    pathTrace.precision(9);
    pathTrace << "pathFrame,x,y,z,screenshot\n";
  }
  const auto start = Clock::now();
  proxy::replay::ProbeReceivers receivers(api, settings.probeReceivers,
                                         settings.receiverRoughness, settings.receiverMetallic,
                                         settings.receiverSegments);
  for (unsigned frame = 0; frame < frames; ++frame) {
    const auto frameStart = Clock::now();
    MSG message;
    while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (pathReady && pathEnd == ~0u) {
      params = proxy::replay::pathCamera(settings, pathFrame);
      params.pNext = &medium;
      const float movingEye[] = {params.position.x, params.position.y, params.position.z};
      std::fill_n(waterColor, 3, 1.f);
      waterDistance = 0;
      medium.inside = proxy::bsp::waterAt(movingEye, waterColor, waterDistance);
      medium.transmittanceColor = {waterColor[0], waterColor[1], waterColor[2]};
      medium.transmittanceMeasurementDistance = waterDistance;
      std::string snapshot;
      if (proxy::replay::capturePathFrame(settings, pathFrame)) {
        const auto target = std::filesystem::path(output);
        snapshot = (target.parent_path() / (target.stem().string() + "-path-" +
                    std::to_string(pathFrame) + ".png")).string();
        pathSnapshots.emplace(snapshot, lastWrite(snapshot));
        requestPathScreenshot(api, snapshot);
      }
      pathTrace << pathFrame << ',' << params.position.x << ',' << params.position.y << ','
                << params.position.z << ',' << snapshot << '\n';
      if (pathFrame == settings.cameraPath.back().frame) pathEnd = frame;
      else ++pathFrame;
    }
    check(api.SetupCamera(&camera), "SetupCamera");
    if (sky.light()) check(api.DrawLightInstance(sky.light()), "Draw replay sky");
    for (auto handle : lights)
      check(api.DrawLightInstance(handle), "DrawLightInstance");
    proxy::bsp::submit(lookup);
    receivers.draw(settings.receiversFollowCamera ? &params : nullptr);
    // The production publisher owns readiness checks. Calling it while assets
    // resolve also reports the actual reason, rather than hiding it behind this
    // diagnostic host's generic "waiting for retained map inputs" state.
    proxy::bsp::publishBakeScene(bakeLights, styledLights, settings.probeBounds ? &*settings.probeBounds : nullptr);
    if (preparationReadyState && (frame == frames - 3 ||
        (measurementEnd != ~0u && frame == measurementEnd + 1) ||
        (pathEnd != ~0u && frame == pathEnd + 32)))
      check(api.RequestPresentedScreenshot(output), "RequestPresentedScreenshot");
    check(api.Present(nullptr), "Present");
    const double hostMs = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
    if (preparationStatistics) {
      remixapi_PreparationStatisticsEXT preparation{};
      preparation.structSize = sizeof(preparation);
      check(preparationStatistics(&preparation), "GetPreparationStatistics");
      finalPreparation = preparation;
      preparationReadyState = proxy::replay::preparationReady(preparation);
      if (preparationReadyState) preparationPendingSince = {};
      else {
        if (preparationPendingSince == Clock::time_point{}) preparationPendingSince = Clock::now();
        if (Clock::now() - preparationPendingSince > std::chrono::minutes(5))
          throw std::runtime_error("Renderer preparation did not complete within five minutes");
        if (frame % 120 == 0)
          std::printf("Preparation waiting: pending %u, loading %u, pipelines %llu/%llu\n",
                      preparation.pending, preparation.loading,
                      static_cast<unsigned long long>(preparation.pipelinesCompleted),
                      static_cast<unsigned long long>(preparation.pipelinesRequested));
      }
      if (!preparationReadyState && (pathReady || measurementEnd != ~0u))
        throw std::runtime_error("Renderer preparation restarted during the completed capture window");
    }
    remixapi_BakeStatisticsEXT stats = {};
    stats.structSize = sizeof(stats);
    if (statistics) {
      check(statistics(&stats), "GetBakeStatistics");
      if (measurement.enabled() && settings.waitForBake && stats.state == REMIXAPI_BAKE_FAILED_EXT)
        throw std::runtime_error(std::string("Map bake failed: ") + stats.status);
      if (stats.active && profile && !requestedProbes) {
        probeReport.request();
        requestedProbes = true;
      }
      if (stats.state != lastState || frame % 120 == 0) {
        std::printf("Frame %u, %.2fs: bake %u, active %u, progress %u/%u; %s\n", frame,
                    std::chrono::duration<double>(Clock::now() - start).count(), stats.state, stats.active,
                    stats.completed, stats.probes, stats.status);
        if (stats.structSize >= sizeof(stats) && stats.reflectionRequested)
          std::printf("Authored reflection readiness: pending %u, active %u, failed %u\n",
                      stats.reflectionPending, stats.reflectionActive, stats.reflectionFailed);
        std::fflush(stdout);
        lastState = stats.state;
      }
    }
    if (!settings.cameraPath.empty()) {
      if (stats.state == REMIXAPI_BAKE_FAILED_EXT)
        throw std::runtime_error(std::string("Camera path bake failed: ") + stats.status);
      if (!pathReady) {
        pathWarmup = preparationReadyState && stats.active && materials.complete() && reflectionsReady(stats, map)
                         ? pathWarmup + 1 : 0;
        pathReady = pathWarmup >= settings.warmupFrames;
        if (pathReady) pathHash = stats.activeHash;
      } else if (!stats.active || stats.activeHash != pathHash)
        throw std::runtime_error("Bake changed during camera path");
      if (profile && pathEnd != ~0u && frame > pathEnd && !requestedPathReport) {
        // The first report describes initial bake publication. Reflection-only
        // authoring can publish later without changing the diffuse bake hash.
        // Request a fresh endpoint report instead of attaching that old state
        // to images recorded after movement and authoring have completed.
        probeReport.request();
        requestedProbes = requestedPathReport = true;
      }
      if (pathEnd != ~0u && frame >= pathEnd + 60) {
        if (profile && (!requestedPathReport ||
            !probeReport.ready()))
          throw std::runtime_error("Fresh endpoint reflection report did not arrive during path drain");
        break;
      }
    }
    if (measurement.enabled() && !measurement.complete()) {
      remixapi_BackendCapabilitiesEXT backend{};
      backend.structSize = sizeof(backend);
      check(backendStatistics(&backend), "GetBackendCapabilities");
      remixapi_GpuPassTimingsEXT timing{};
      timing.structSize = sizeof(timing);
      check(gpuStatistics(&timing), "GetGpuPassTimings");
      bool ready = preparationReadyState && materials.complete() && backend.activeBackend == backend.requestedBackend &&
                   (backend.activeBackend != 2 || !settings.waitForBake || stats.active);
      if (backend.activeBackend == 2 && settings.waitForBake)
        ready &= reflectionsReady(stats, map);
      if (backend.requestedBackend == 1) {
        if (!denoisingStatistics)
          throw std::runtime_error("Runtime lacks denoising readiness statistics");
        remixapi_DenoisingStatisticsEXT denoising{};
        denoising.structSize = sizeof(denoising);
        check(denoisingStatistics(&denoising), "GetDenoisingStatistics");
        const bool temporalReady = denoising.activeDenoiser == denoising.requestedDenoiser &&
                                   denoising.activeTaa == denoising.requestedTaa;
        ready &= temporalReady;
        if (!temporalReady && frame % 120 == 0)
          std::printf("Temporal readiness: denoiser active %u / requested %u, TAA active %u / requested %u\n",
                      unsigned(denoising.activeDenoiser), unsigned(denoising.requestedDenoiser),
                      unsigned(denoising.activeTaa), unsigned(denoising.requestedTaa));
      }
      if (!ready && frame % 120 == 0)
        std::printf("Measurement waiting: backend %u requested %u; %s\n", backend.activeBackend,
                    backend.requestedBackend, backend.unavailableReason);
      if (!ready && !reportedPendingInputs &&
          std::chrono::duration<double>(Clock::now() - start).count() > 5) {
        proxy::bsp::dumpStatistics(stdout);
        std::fflush(stdout);
        reportedPendingInputs = true;
      }
      hostProfile.sample(hostMs, ready, backend.activeBackend,
                         backend.activeBackend == 2 ? stats.activeHash : 0);
      measurement.observe(ready, backend.activeBackend, backend.activeBackend == 2 ? stats.activeHash : 0,
                          timing);
      if (measurement.complete()) {
        measurementEnd = frame;
        if (profile) {
          // Reflection-only edits publish after the diffuse bake without changing
          // its hash. Match the final image to a fresh report, not initial bake state.
          probeReport.request();
          requestedProbes = true;
          pipelineReport.request();
          requestedPipelines = true;
        }
        if (const char *prefix = std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX")) {
          std::ofstream request(std::string(prefix) + ".frame.request");
          if (!request)
            std::fprintf(stderr, "Cannot request the optional frame input dump\n");
        }
        std::printf("GPU measurement complete: %zu distinct frames after %u warmup frames\n",
                    measurement.samples(), measurement.warmedFrames());
      }
    }
    const unsigned drain = std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX") ? 8 : 3;
    if (measurementEnd != ~0u && frame >= measurementEnd + drain) {
      // Runtime file requests are serviced every 30 presents. A short timing
      // window can finish before that poll; drain the requested probe report
      // as well as the screenshot, without including either in measurements.
      const bool pendingProbes = profile && requestedProbes &&
                                 !probeReport.ready();
      const bool pendingPipelines = requestedPipelines && !pipelineReport.ready();
      if (!pendingProbes && !pendingPipelines)
        break;
      if (frame - measurementEnd >= 60)
        throw std::runtime_error("Requested runtime diagnostic reports did not arrive within 60 presents");
    }
    // Restrict the diagnostic delay to the asynchronous CPU/CUDA filtering
    // stage. Transport scheduling and the finished-lighting timing window keep
    // their normal submission cadence. The installed game is unaffected.
    if (!stats.active && settings.denoiseFrameDelayMs &&
        std::strstr(stats.status, "Denoising surface lightmaps"))
      std::this_thread::sleep_for(std::chrono::milliseconds(settings.denoiseFrameDelayMs));
    else if (!measurement.enabled())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!settings.cameraPath.empty() && (pathEnd == ~0u || frames <= pathEnd + 60))
    throw std::runtime_error("Replay frame budget ended before the camera path drained");
  for (const auto &[snapshot, previous] : pathSnapshots)
    if (lastWrite(snapshot) == previous)
      throw std::runtime_error("Camera path produced no fresh screenshot: " + snapshot);
  if (measurement.enabled())
    measurement.write(output, map);
  // Requested accumulation is not evidence that history survived scene updates.
  // Keep the final submitted tracing state beside replay receipts so a noisy
  // reference cannot be mistaken for a converged multi-frame image.
  const auto tracingStatistics = reinterpret_cast<PFN_remixapi_GetTracingStatisticsEXT>(
      GetProcAddress(library, "remixapi_GetTracingStatisticsEXT"));
  if (tracingStatistics) {
    remixapi_TracingStatisticsEXT tracing{};
    tracing.structSize = sizeof(tracing);
    check(tracingStatistics(&tracing), "GetTracingStatistics");
    std::printf("Final tracing state: samples=%u bounces=%u accumulation=%u frames=%llu "
                "sequence=%llu sceneGeneration=%llu\n",
                tracing.samplesPerPixel, tracing.indirectBounces, tracing.accumulationEnabled,
                static_cast<unsigned long long>(tracing.accumulationCount),
                static_cast<unsigned long long>(tracing.sampleSequence),
                static_cast<unsigned long long>(tracing.sceneGeneration));
  }
  if (preparationStatistics) {
    std::filesystem::path report(output);
    report.replace_extension(".preparation.json");
    std::ofstream(report) << "{\n  \"pending\": " << finalPreparation.pending
        << ",\n  \"loading\": " << finalPreparation.loading
        << ",\n  \"failed\": " << finalPreparation.failed
        << ",\n  \"pipelinesRequested\": " << finalPreparation.pipelinesRequested
        << ",\n  \"pipelinesCompleted\": " << finalPreparation.pipelinesCompleted
        << ",\n  \"pipelinesFailed\": " << finalPreparation.pipelinesFailed << "\n}\n";
  }
  if (profile && requestedProbes && probeReport.ready()) {
    auto report = std::filesystem::path(output);
    report.replace_extension(".probes.json");
    std::filesystem::copy_file(diagnostics / "probes.json", report,
                               std::filesystem::copy_options::overwrite_existing);
    std::printf("Published reflection probe report: %s\n", report.string().c_str());
  }
  if (requestedPipelines && pipelineReport.ready()) {
    auto report = std::filesystem::path(output);
    report.replace_extension(".pipelines.txt");
    std::filesystem::copy_file(diagnostics / "pipelines.txt", report,
                               std::filesystem::copy_options::overwrite_existing);
    std::printf("Published pipeline timing report: %s\n", report.string().c_str());
  }
  proxy::bsp::dumpStatistics(stdout);
  if (!settings.mods.empty()) {
    const auto diagnostic = reinterpret_cast<decltype(&remixapi_GetUSDModsDiagnosticEXT)>(
        GetProcAddress(library, "remixapi_GetUSDModsDiagnosticEXT"));
    char message[2048]{};
    if (!diagnostic) throw std::runtime_error("Runtime lacks USD mod diagnostics");
    check(diagnostic(message, sizeof(message)), "USD mod diagnostics");
    if (message[0]) throw std::runtime_error(std::string("USD mod load: ") + message);
    std::printf("USD mod directory: %s (no loader diagnostic)\n", settings.mods.c_str());
  }
  std::printf("Static VMT replay only: engine material proxies, entities, particles and UI are absent.\n");
}
} // namespace

namespace {
LRESULT CALLBACK replayWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_GETMINMAXINFO) {
    // Diagnostic captures may exceed the desktop size. Settings bound each
    // client dimension to 4096; leave space here for the non-client frame.
    auto &limits = *reinterpret_cast<MINMAXINFO *>(lparam);
    limits.ptMaxTrackSize = {8192, 8192};
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 5 || (argc > 7 && ((argc - 7) % 3 || argc > 28))) {
    std::fprintf(stderr,
                 "Usage: bsp-replay path/to/remix.dll content-root map output.png [frames=600] [settings.cfg]\n"
                 "       Optional: append up to seven (map output.png settings.cfg) stages in the same renderer\n");
    return 2;
  }
  SetEnvironmentVariableA("OPENREMIX_PROXY_CONTENT_ROOT", argv[2]);
  const auto path = std::filesystem::absolute(argv[1]).wstring();
  // The production BSP adapter resolves optional extensions through this module
  // name. Renamed comparison DLLs otherwise silently omit static bake inputs.
  if (_wcsicmp(std::filesystem::path(path).filename().c_str(), L"remix.dll") != 0) {
    std::fprintf(stderr, "BSP replay requires the runtime filename remix.dll; use separate directories for comparison builds\n");
    return 2;
  }
  SetDllDirectoryW(std::filesystem::path(path).parent_path().c_str());
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = replayWindowProcedure;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"OpenRemixBspReplay";
  if (!RegisterClassW(&windowClass))
    return 1;
  HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"BSP replay", WS_OVERLAPPEDWINDOW, 0, 0, 640,
                                360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!window)
    return 1;
  remixapi_Interface api = {};
  remixapi_HMODULE library = nullptr;
  int result = 0;
  struct Stage {
    const char *map, *settings;
    std::string output;
    std::filesystem::file_time_type previous;
  };
  std::vector<Stage> stages;
  auto appendStage = [&](const char *map, const char *output, const char *settings) {
    const auto absolute = std::filesystem::absolute(output).lexically_normal().string();
    stages.push_back({map, settings, absolute, lastWrite(absolute)});
  };
  appendStage(argv[3], argv[4], argc > 6 ? argv[6] : nullptr);
  for (int i = 7; i < argc; i += 3)
    appendStage(argv[i], argv[i + 1], argv[i + 2]);
  for (size_t i = 0; i < stages.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (_stricmp(stages[i].output.c_str(), stages[j].output.c_str()) == 0) {
        std::fprintf(stderr, "Replay stages need distinct screenshot destinations\n");
        DestroyWindow(window);
        return 2;
      }
    }
  }
  try {
    check(remixapi_lib_loadRemixDllAndInitialize(path.c_str(), &api, &library), "Load runtime");
    remixapi_StartupInfo startup = {};
    startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
    startup.hwnd = window;
    check(api.Startup(&startup), "Startup");
    check(api.SetConfigVariable("rtx.native.backend", "forwardplus"), "Select Forward+");
    check(api.SetConfigVariable("rtx.native.pathtrace.overrideLight", "0"), "Disable profile sun");
    proxy::source_rendering::lightingDefaults([&](const char *key, const char *value) {
      check(api.SetConfigVariable(key, value), "Set Source lighting defaults");
    });
    for (size_t i = 0; i < stages.size(); ++i) {
      const auto &stage = stages[i];
      std::printf("Replay stage %zu/%zu: %s\n", i + 1, stages.size(), stage.map);
      const auto settings = stage.settings ? proxy::replay::configure(api, stage.settings) : proxy::replay::Settings{};
      if (!settings.mods.empty()) {
        const auto configure = reinterpret_cast<decltype(&remixapi_ConfigureUSDModsEXT)>(
            GetProcAddress(library, "remixapi_ConfigureUSDModsEXT"));
        if (!configure) throw std::runtime_error("Runtime lacks USD mod loading");
        remixapi_USDModsOptionsEXT mods{};
        mods.structSize = sizeof(mods); mods.version = 1; mods.directory = settings.mods.c_str();
        mods.hostMetersPerUnit = .01; mods.hostUpAxis = 2; mods.watch = 0;
        check(configure(&mods), "Configure USD mods");
      }
      RECT bounds{0, 0, LONG(settings.width), LONG(settings.height)};
      if (!AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE) ||
          !SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                        SWP_NOMOVE | SWP_NOZORDER))
        throw std::runtime_error("Cannot size replay window");
      RECT client{};
      if (!GetClientRect(window, &client) || client.right != LONG(settings.width) ||
          client.bottom != LONG(settings.height))
        throw std::runtime_error("Replay client size differs from requested resolution: " +
                                 std::to_string(client.right) + "x" + std::to_string(client.bottom));
      ShowWindow(window, SW_SHOWNOACTIVATE);
      std::printf("Replay target %u x %u\n", settings.width, settings.height);
      replay(api, library, window, stage.map, stage.output.c_str(), argc > 5 ? std::max(4, std::atoi(argv[5])) : 600,
             settings);
      std::printf("Replay stage %zu/%zu complete: %s\n", i + 1, stages.size(), stage.map);
    }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    result = 1;
  }
  if (library)
    remixapi_lib_shutdownAndUnloadRemixDll(&api, library);
  for (const auto &stage : stages) {
    if (!result && lastWrite(stage.output) == stage.previous) {
      std::fprintf(stderr, "Replay produced no screenshot: %s\n", stage.output.c_str());
      result = 1;
    }
  }
  DestroyWindow(window);
  return result;
}
