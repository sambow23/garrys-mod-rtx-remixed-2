#include "sky_mesh_clip.h"
#include "cubemap_overrides.h"
#include "cubemap_receivers.h"
#include "source_lights.h"
#include "bsp_world.h"
#include "bake_charts.h"
#include "bake_materials.h"
#include "refract_native.h"

#include "lzma_source.h"
#include "source_fs.h"
#include "texture_categories.h"
#include "texture_formats.h"
#include "sky_texture.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <windows.h>

#include <remix/remix_native_ext.h>
#include <remix/remix_bake_ext.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace proxy::bsp {
namespace {

// --- BSP (version 21, Left 4 Dead 2 lump order) -----------------------------
enum Lump {
  kEntities = 0,
  kPlanes = 1,
  kTexData = 2,
  kVertexes = 3,
  kVisibility = 4,
  kNodes = 5,
  kTexInfo = 6,
  kFaces = 7,
  kLeafs = 10,
  kLeafFaces = 16,
  kEdges = 12,
  kSurfEdges = 13,
  kModels = 14,
  kDispInfo = 26,
  kDispVerts = 33,
  kGameLump = 35,
  kLeafWaterData = 36,
  kPakfile = 40,
  kCubemaps = 42,
  kTexDataStringData = 43,
  kTexDataStringTable = 44,
  kOverlays = 45,
  kWorldLights = 15,
  kWorldLightsHdr = 54,
};
// texinfo_t::flags
constexpr int32_t kSurfSky2D = 0x2, kSurfSky = 0x4, kSurfTrans = 0x10, kSurfNoDraw = 0x80, kSurfHint = 0x100,
                  kSurfSkip = 0x200;

#pragma pack(push, 1)
struct Plane {
  float normal[3], dist;
  int32_t type;
};
struct Face {
  uint16_t plane;
  uint8_t side, onNode;
  int32_t firstEdge;
  int16_t numEdges, texInfo, dispInfo, fogVolume;
  uint8_t styles[4];
  int32_t lightOffset;
  float area;
  int32_t lightmapMins[2], lightmapSize[2], origFace;
  uint16_t numPrims, firstPrim;
  uint32_t smoothingGroups;
};
struct TexInfo {
  float textureVecs[2][4], lightmapVecs[2][4];
  int32_t flags, texData;
};
struct TexData {
  float reflectivity[3];
  int32_t nameId, width, height, viewWidth, viewHeight;
};
struct Node {
  int32_t plane, children[2];
  int16_t mins[3], maxs[3];
  uint16_t firstFace, numFaces;
  int16_t area, padding;
};
struct Leaf {
  int32_t contents;
  int16_t cluster, areaFlags;
  int16_t mins[3], maxs[3];
  uint16_t firstLeafFace, numLeafFaces, firstLeafBrush, numLeafBrushes;
  int16_t waterData, padding;
};
struct LeafWater {
  float surfaceZ, minZ;
  int16_t surfaceTexInfo, padding;
};
struct Model {
  float mins[3], maxs[3], origin[3];
  int32_t headNode, firstFace, numFaces;
};
struct DispInfo {
  float start[3];
  int32_t firstVert, firstTri, power, minTess;
  float smoothingAngle;
  int32_t contents;
  uint16_t face;
  uint8_t rest[176 - 38];
};
struct DispVert {
  float vector[3], dist, alpha;
};
struct Overlay {
  int32_t id;
  int16_t texInfo;
  uint16_t faceCountAndRenderOrder;
  int32_t faces[64];
  float u[2], v[2];
  float uvPoints[4][3]; // corners in the (U, V) basis; z holds U's components
  float origin[3], normal[3];
};
#pragma pack(pop)
static_assert(sizeof(Face) == 56 && sizeof(TexInfo) == 72 && sizeof(TexData) == 32 && sizeof(Node) == 32 &&
                  sizeof(Leaf) == 32 && sizeof(Model) == 48 && sizeof(DispInfo) == 176 && sizeof(DispVert) == 20 && sizeof(Overlay) == 352,
              "BSP layouts");

class Bsp {
public:
  std::vector<uint8_t> data;
  bool open(const std::wstring &path) {
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f)
      return false;
    _fseeki64(f, 0, SEEK_END);
    data.resize(size_t(_ftelli64(f)));
    _fseeki64(f, 0, SEEK_SET);
    const bool ok = fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok && parse();
  }
  // A map read through the file system (e.g. from a workshop addon).
  bool open(std::vector<uint8_t> bytes) {
    data = std::move(bytes);
    return parse();
  }
  bool parse() {
    int32_t version = 0;
    if (data.size() < 1036 || std::memcmp(data.data(), "VBSP", 4) != 0)
      return false;
    std::memcpy(&version, data.data() + 4, 4);
    if (version < 19 || version > 21)
      return false;
    // lump_t is {fileofs, filelen, version, fourCC} up to v20 (GMod maps).
    // Left 4 Dead 2's v21 puts the version first; other v21 maps (TF2, CS:GO
    // era) keep the standard order. As NikNaks: a first field of at most 8
    // in lump 0 (planes) is a version, not a file offset.
    int32_t first;
    std::memcpy(&first, data.data() + 8, 4);
    versionFirst = version == 21 && first >= 0 && first <= 8;
    // Compressed lumps ("LZMA": TF2's and other newer maps), decoded once.
    // One that does not decode reads as missing.
    unpacked.assign(64, {});
    for (int i = 0; i < 64; ++i) {
      size_t offset, length;
      if (span(i, offset, length) && length >= 4 && std::memcmp(data.data() + offset, "LZMA", 4) == 0) {
        compressed.push_back(i);
        if (!lzma::decodeSource(data.data() + offset, length, unpacked[size_t(i)]))
          unpacked[size_t(i)].clear();
      }
    }
    return true;
  }
  bool versionFirst = false;
  std::vector<std::vector<uint8_t>> unpacked; // decoded compressed lumps, by index
  std::vector<int> compressed;                // their indices
  // A lump's place in the file (as stored: compressed or not).
  bool span(int index, size_t &offset, size_t &length) const {
    int32_t header[4];
    std::memcpy(header, data.data() + 8 + index * 16, 16);
    const int32_t ofs = versionFirst ? header[1] : header[0], len = versionFirst ? header[2] : header[1];
    offset = size_t(ofs);
    length = size_t(len);
    return ofs >= 0 && len >= 0 && offset + length <= data.size();
  }
  template <typename T> std::pair<const T *, size_t> lump(int index) const {
    size_t offset, length;
    if (!span(index, offset, length))
      return {nullptr, 0};
    const uint8_t *at = data.data() + offset;
    if (length >= 4 && std::memcmp(at, "LZMA", 4) == 0) {
      const auto &decoded = unpacked[size_t(index)];
      at = decoded.data();
      length = decoded.size();
      if (!length)
        return {nullptr, 0};
    }
    if (length % sizeof(T))
      return {nullptr, 0};
    return {reinterpret_cast<const T *>(at), length / sizeof(T)};
  }
  int32_t lumpVersion(int index) const {
    int32_t v;
    std::memcpy(&v, data.data() + 8 + index * 16 + (versionFirst ? 0 : 8), 4);
    return v;
  }
};

// --- Materials (VMT) and texture hashes (VTF) --------------------------------
struct KeyValues {
  std::string shader;
  std::map<std::string, std::string> values;                 // top level, lowercase keys
  std::map<std::string, std::map<std::string, std::string>> blocks; // top-level sub-blocks (patch insert/replace)
};

std::string lower(std::string s) {
  for (auto &c : s)
    c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A material's texture file. Source drops an extension the VMT spells out
// ("$basetexture" "models/props_mvm/sack_flat.vtf").
std::string vtfPath(const std::string &name) {
  std::string path = lower(name);
  if (path.size() > 4 && path.compare(path.size() - 4, 4, ".vtf") == 0)
    path.resize(path.size() - 4);
  return "materials/" + path + ".vtf";

}

bool parseKeyValues(const std::string &text, KeyValues &out) {
  std::vector<std::string> tokens;
  for (size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
    } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n')
        ++i;
    } else if (c == '{' || c == '}') {
      tokens.emplace_back(1, c);
      ++i;
    } else if (c == '"') {
      const size_t end = text.find('"', i + 1);
      tokens.push_back(text.substr(i + 1, (end == std::string::npos ? text.size() : end) - i - 1));
      tokens.back().insert(0, 1, '\x01'); // quoted: never a brace
      i = end == std::string::npos ? text.size() : end + 1;
    } else {
      size_t end = i;
      while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != '{' &&
             text[end] != '}' && text[end] != '"')
        ++end;
      tokens.push_back(text.substr(i, end - i));
      i = end;
    }
  }
  auto plain = [](std::string t) { return !t.empty() && t[0] == '\x01' ? t.substr(1) : t; };
  if (tokens.size() < 2 || tokens[1] != "{")
    return false;
  out.shader = lower(plain(tokens[0]));
  size_t i = 2;
  // Skips a block whose '{' was consumed.
  auto skip = [&]() {
    for (int depth = 1; i < tokens.size() && depth > 0; ++i)
      depth += tokens[i] == "{" ? 1 : tokens[i] == "}" ? -1 : 0;
  };
  while (i < tokens.size() && tokens[i] != "}") {
    const std::string key = lower(plain(tokens[i++]));
    if (i >= tokens.size())
      break;
    if (tokens[i] == "{") {
      ++i;
      auto &block = out.blocks[key];
      while (i < tokens.size() && tokens[i] != "}") {
        const std::string k = lower(plain(tokens[i++]));
        if (i >= tokens.size())
          break;
        if (tokens[i] == "{") {
          // A nested block: its name, and its values as "name.key" ("proxies"
          // { "animatedtexture" { "animatedtextureframerate" "30" } }).
          ++i;
          block.emplace(k, std::string());
          while (i < tokens.size() && tokens[i] != "}") {
            const std::string kk = lower(plain(tokens[i++]));
            if (i >= tokens.size())
              break;
            if (tokens[i] == "{") {
              ++i;
              skip();
            } else {
              block[k + "." + kk] = plain(tokens[i++]);
            }
          }
          ++i;
        } else {
          block[k] = plain(tokens[i++]);
        }
      }
      ++i;
    } else {
      out.values[key] = plain(tokens[i++]);
    }
  }
  return true;
}

// A material's top-level values, with patch materials (cubemap and water
// variants the compiler writes into the pakfile) applied onto their include.
bool loadMaterial(const std::string &name, KeyValues &out, int depth = 0) {
  std::vector<uint8_t> bytes;
  std::string path = lower(name);
  if (path.rfind("materials/", 0) != 0)
    path = "materials/" + path;
  if (path.size() < 4 || path.compare(path.size() - 4, 4, ".vmt") != 0)
    path += ".vmt";
  if (depth > 4 || !fs::read(path, bytes))
    return false;
  KeyValues kv;
  if (!parseKeyValues(std::string(bytes.begin(), bytes.end()), kv))
    return false;
  // Hardware conditionals (">=dx90_20b" { ... }): every "at least" level holds
  // here, as on any DX9 hardware; "<" blocks are for older hardware.
  for (auto &[condition, block] : kv.blocks)
    if (condition.size() > 3 && condition[0] == '>' && condition.find("dx") != std::string::npos)
      for (auto &[k, v] : block)
        kv.values[k] = v;
  if (kv.shader != "patch") {
    out = std::move(kv);
    return true;
  }
  if (!loadMaterial(kv.values["include"], out, depth + 1))
    return false;
  for (const char *section : {"insert", "replace"})
    for (auto &[k, v] : kv.blocks[section])
      out.values[k] = v;
  return true;
}

// dxvk hashes of a texture's three largest mips as D3D would receive them
// (packed rows; the engine may skip top mips by quality settings). Formats the
// proxy does not upload yield none.
std::vector<uint64_t> textureHashes(const std::string &path, bool engine, bool legacy = false, uint32_t frame = 0) {

  const auto read = [engine](const std::string &file, std::vector<uint8_t> &out, uint64_t offset, uint64_t length) {
    return engine ? fs::engineReadFile(file, out, offset, length) : fs::read(file, out, offset, length);
  };
  std::vector<uint8_t> header;
  uint64_t total = 0;
  if (!(engine ? fs::engineSize(path, total) : fs::size(path, total)) || !read(path, header, 0, 80) || header.size() < 64 ||
      std::memcmp(header.data(), "VTF", 4) != 0)
    return {};
  uint16_t width, height, frames;
  uint32_t flags;
  int32_t format;
  std::memcpy(&width, header.data() + 16, 2);
  std::memcpy(&height, header.data() + 18, 2);
  std::memcpy(&flags, header.data() + 20, 4);
  std::memcpy(&frames, header.data() + 24, 2);
  std::memcpy(&format, header.data() + 52, 4);
  const uint8_t mips = header[56];
  uint32_t blockBytes = 0, pixelBytes = 0;
  // D3D9 has no 24-bit or RGBA-order formats: Source widens/swizzles these
  // to B8G8R8A8 or B8G8R8X8 before the upload the proxy hashes. X is filled
  // with 0xff or 0 depending on the conversion path: both are candidates.
  int convert = 0; // 0 none, 2 RGB888, 3 BGR888, 1 RGBA8888, 4 ABGR8888
  switch (format) {
  case 0: // RGBA8888
    pixelBytes = 4;
    convert = 1;
    break;
  case 1: // ABGR8888
    pixelBytes = 4;
    convert = 4;
    break;
  case 2: // RGB888
  case 3: // BGR888
    pixelBytes = 3;
    convert = format;
    break;
  case 13: // DXT1
  case 20: // DXT1_ONEBITALPHA
    blockBytes = 8;
    break;
  case 14: // DXT3
  case 15: // DXT5
    blockBytes = 16;
    break;
  case 12: // BGRA8888
  case 16: // BGRX8888
    pixelBytes = 4;
    break;
  default:
    return {};
  }
  const uint32_t faces = (flags & 0x4000) ? 6 : 1; // environment maps
  // High-resolution mips are stored smallest first and end the file.
  std::vector<uint64_t> hashes;
  uint64_t end = total;
  for (uint32_t mip = 0; mip < std::min<uint32_t>(3, mips); ++mip) {
    const uint32_t w = std::max(1u, uint32_t(width) >> mip), h = std::max(1u, uint32_t(height) >> mip);
    const uint64_t bytes = blockBytes ? uint64_t((w + 3) / 4) * ((h + 3) / 4) * blockBytes : uint64_t(w) * h * pixelBytes;
    const uint64_t block = bytes * std::max<uint16_t>(frames, 1) * faces;
    if (block > end)
      break;
    // Within a level the frames follow one another (each with its faces);
    // Source uploads each frame of an animated texture as its own texture.
    std::vector<uint8_t> level;
    const uint64_t at = end - block + uint64_t(frame % std::max<uint16_t>(frames, 1)) * faces * bytes;
    if (!read(path, level, at, bytes) || level.size() != bytes)
      break;
    if (!convert) {
      hashes.push_back(legacy ? XXH64(level.data(), level.size(), 0) : XXH3_64bits(level.data(), level.size()));
    } else {
      const size_t pixels = size_t(w) * h;
      std::vector<uint8_t> bgra(pixels * 4);
      for (int fill : {0xff, 0x00}) {
        for (size_t i = 0; i < pixels; ++i) {
          const uint8_t *s = level.data() + i * pixelBytes;
          uint8_t *d = bgra.data() + i * 4;
          if (convert == 4) {
            d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0];
            continue;
          }
          const bool rgbOrder = convert != 3;
          d[0] = rgbOrder ? s[2] : s[0];
          d[1] = s[1];
          d[2] = rgbOrder ? s[0] : s[2];
          d[3] = convert == 1 ? s[3] : uint8_t(fill);
        }
        hashes.push_back(legacy ? XXH64(bgra.data(), bgra.size(), 0) : XXH3_64bits(bgra.data(), bgra.size()));
        if (convert == 1 || convert == 4)
          break;
      }
    }
    end -= block;
  }
  return hashes;
}

// Of the index's copy and, when the engine has a different one (a workshop
// addon replacing a stock texture), of that as well.
std::vector<uint64_t> textureHashes(const std::string &name, uint32_t frame = 0) {
  const std::string path = vtfPath(name);
  std::vector<uint64_t> hashes = textureHashes(path, false, false, frame);
  for (uint64_t hash : textureHashes(path, true, false, frame))

    if (std::find(hashes.begin(), hashes.end(), hash) == hashes.end())
      hashes.push_back(hash);
  return hashes;
}

// Source water has no stable base texture. Generated packs key it by its
// normal map; older fixed-function packs used dev/dev_monitor for every body.
// Preserve both identities without tying SDK resource IDs to replacement IDs.
std::vector<uint64_t> waterReplacementKeys(const KeyValues &material) {
  char override[32] = {};
  if (GetEnvironmentVariableA("OPENREMIX_PROXY_WATER_KEY", override, sizeof(override))) {
    const auto key = uint64_t(std::strtoull(override, nullptr, 16));
    return key ? std::vector<uint64_t>{key} : std::vector<uint64_t>{};
  }
  std::vector<uint64_t> keys;
  auto append = [&](const std::string &name) {
    const auto path = vtfPath(name);
    for (const bool engine : {false, true})
      for (const bool legacy : {true, false})
        for (const auto key : textureHashes(path, engine, legacy))
          if (key && keys.size() < 32 && std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
  };
  if (const auto normal = material.values.find("$normalmap"); normal != material.values.end())
    append(normal->second);
  append("dev/dev_monitor");
  return keys;
}

// Whether a texture has an alpha channel as the engine sees it
// (ITexture::IsTranslucent: TEXTUREFLAGS_ONEBITALPHA | EIGHTBITALPHA).
bool textureHasAlpha(const std::string &name) {
  std::vector<uint8_t> header;
  uint32_t flags = 0;
  if (!fs::read(vtfPath(name), header, 0, 64) || header.size() < 24 ||

      std::memcmp(header.data(), "VTF", 4) != 0)
    return false;
  std::memcpy(&flags, header.data() + 20, 4);
  return (flags & 0x3000) != 0;
}

// Whether a texture specifies point sampling (TEXTUREFLAGS_POINTSAMPLE = 0x1).
bool texturePointSample(const std::string &name) {
  const std::string path = vtfPath(name);
  std::vector<uint8_t> header;
  if ((!fs::read(path, header, 0, 64) || header.size() < 24) &&
      (!fs::engineReadFile(path, header, 0, 64) || header.size() < 24))
    return false;
  if (std::memcmp(header.data(), "VTF", 4) != 0)
    return false;
  uint32_t flags = 0;
  std::memcpy(&flags, header.data() + 20, 4);
  return (flags & 0x1) != 0;
}

// A texture's mean colour, as stored (0..1, no gamma): its smallest mip, or
// the whole image of one without mips. False for a missing file or a format
// not read here.
bool textureMean(const std::string &name, float rgba[4]) {
  const std::string path = vtfPath(name);

  std::vector<uint8_t> header;
  uint64_t total = 0;
  if (!fs::size(path, total) || !fs::read(path, header, 0, 80) || header.size() < 64 ||
      std::memcmp(header.data(), "VTF", 4) != 0)
    return false;
  uint16_t width, height, frames;
  uint32_t flags;
  int32_t format;
  std::memcpy(&width, header.data() + 16, 2);
  std::memcpy(&height, header.data() + 18, 2);
  std::memcpy(&flags, header.data() + 20, 4);
  std::memcpy(&frames, header.data() + 24, 2);
  std::memcpy(&format, header.data() + 52, 4);
  const uint32_t mips = std::max<uint32_t>(header[56], 1);
  uint32_t blockBytes = 0, pixelBytes = 0;
  int order[4] = {0, 1, 2, 3}; // where r, g, b, a are in a pixel (-1: none)
  switch (format) {
  case 0: pixelBytes = 4; break;                                             // RGBA8888
  case 1: pixelBytes = 4; order[0] = 3; order[1] = 2; order[2] = 1; order[3] = 0; break; // ABGR8888
  case 2: pixelBytes = 3; order[3] = -1; break;                              // RGB888
  case 3: pixelBytes = 3; order[0] = 2; order[2] = 0; order[3] = -1; break;  // BGR888
  case 12: pixelBytes = 4; order[0] = 2; order[2] = 0; break;                // BGRA8888
  case 16: pixelBytes = 4; order[0] = 2; order[2] = 0; order[3] = -1; break; // BGRX8888
  case 13: case 20: blockBytes = 8; break;                                   // DXT1
  case 14: case 15: blockBytes = 16; break;                                   // DXT3, DXT5
  default: return false;
  }
  if (!width || !height || mips > 16)
    return false;
  const uint32_t faces = (flags & 0x4000) ? 6 : 1;
  auto bytesOf = [&](uint32_t mip) {
    const uint32_t w = std::max(1u, uint32_t(width) >> mip), h = std::max(1u, uint32_t(height) >> mip);
    return blockBytes ? uint64_t((w + 3) / 4) * ((h + 3) / 4) * blockBytes : uint64_t(w) * h * pixelBytes;
  };
  // The mips end the file, smallest first.
  uint64_t all = 0;
  for (uint32_t mip = 0; mip < mips; ++mip)
    all += bytesOf(mip) * std::max<uint16_t>(frames, 1) * faces;
  const uint32_t last = mips - 1;
  const uint32_t w = std::max(1u, uint32_t(width) >> last), h = std::max(1u, uint32_t(height) >> last);
  std::vector<uint8_t> level;
  if (all > total || bytesOf(last) > (64u << 20) || !fs::read(path, level, total - all, bytesOf(last)) ||
      level.size() != bytesOf(last))
    return false;
  if (blockBytes) {
    level = blockBytes == 8 ? formats::decodeBC1(level.data(), w, h)
            : format == 14  ? formats::decodeBC2(level.data(), w, h)
                            : formats::decodeBC3(level.data(), w, h);
    pixelBytes = 4;
  }
  double sum[4] = {};
  const size_t pixels = size_t(w) * h;
  for (size_t i = 0; i < pixels; ++i)
    for (int c = 0; c < 4; ++c)
      sum[c] += order[c] < 0 ? 255.0 : double(level[i * pixelBytes + size_t(order[c])]);
  for (int c = 0; c < 4; ++c)
    rgba[c] = float(sum[c] / (255.0 * double(pixels)));
  return true;
}

// "[r g b]" (floats) or "{r g b}" (0..255), or one number for all three.
bool parseColor(const std::string &text, float out[3]) {
  const size_t digits = text.find_first_of("-.0123456789");
  if (digits == std::string::npos)
    return false;
  float v[3];
  const int n = std::sscanf(text.c_str() + digits, "%f %f %f", &v[0], &v[1], &v[2]);
  if (n < 1)
    return false;
  const float scale = text.find('{') != std::string::npos ? 1.0f / 255 : 1.0f;
  for (int c = 0; c < 3; ++c)
    out[c] = (n >= 3 ? v[c] : v[0]) * scale;
  return true;
}

// What a VMT's material does with its base texture besides being lit: the
// engine's rules (engine_materials reads the same from live materials), with
// the VMT's static values; material proxies do not run for the BSP world.
// maskCandidates: the mask texture's hashes.
engine_materials::Shading materialShading(KeyValues &kv, std::vector<uint64_t> &maskCandidates) {
  using engine_materials::Shading;
  Shading out;
  auto number = [&](const char *k) { return kv.values.count(k) ? float(std::atof(kv.values[k].c_str())) : 0.0f; };
  const Shading::Mode kind = engine_materials::shaderMode(kv.shader.c_str());
  if (kind == Shading::Unlit) {
    out.mode = Shading::Unlit;
    out.luminous = number("$selfillum") != 0;
    float color[3] = {1, 1, 1}, color2[3] = {1, 1, 1};
    if (kv.values.count("$color"))
      parseColor(kv.values["$color"], color);
    if (kv.values.count("$color2"))
      parseColor(kv.values["$color2"], color2);
    for (int c = 0; c < 3; ++c)
      out.tint[c] = std::pow(std::max(color[c] * color2[c], 0.0f), 2.2f);
    return out;
  }
  if (kind != Shading::SelfIllumBaseAlpha || !kv.values.count("$basetexture"))
    return out;
  const bool vertexLit = kv.shader.rfind("vertexlitgeneric", 0) == 0;
  if (kv.values.count("$selfillumtint"))
    parseColor(kv.values["$selfillumtint"], out.tint);
  for (float &c : out.tint)
    c = std::max(c, 0.0f);
  if (number("$selfillum") != 0) {
    const bool fresnel = vertexLit && number("$selfillumfresnel") != 0;
    if (vertexLit && kv.values.count("$selfillummask")) {
      out.mode = Shading::SelfIllumMask;
      out.mask = lower(kv.values["$selfillummask"]);
      if (const float scale = number("$selfillummaskscale"); scale > 0)
        out.maskScale = scale;
    } else if (textureHasAlpha(kv.values["$basetexture"]) || fresnel) {
      // Without an alpha channel the shaders drop $selfillum.
      out.mode = Shading::SelfIllumBaseAlpha;
    }
    if (out.mode != Shading::Lit && fresnel) {
      float range[3] = {0, 1, 1};
      if (kv.values.count("$selfillumfresnelminmaxexp"))
        parseColor(kv.values["$selfillumfresnelminmaxexp"], range);
      out.maskScale *= engine_materials::fresnelShare(range[0], range[1], range[2]);
      if (!(out.maskScale > 0))
        out.mode = Shading::Lit;
    }
  } else if (vertexLit && number("$selfillum_envmapmask_alpha") != 0 && kv.values.count("$envmapmask")) {
    out.mode = Shading::SelfIllumMaskAlpha;
    out.mask = lower(kv.values["$envmapmask"]);
    out.maskScale = 8;
  }
  if (!out.mask.empty())
    maskCandidates = textureHashes(out.mask);
  return out;
}

// A VMT's PBR conversion (pbr_rules), from its static values.
pbr::Material materialPbr(KeyValues &kv) {
  auto text = [&](const char *k) {
    const auto it = kv.values.find(k);
    return it == kv.values.end() ? std::string() : lower(it->second);
  };
  auto number = [&](const char *k) {
    const auto it = kv.values.find(k);
    return it == kv.values.end() ? 0.0f : float(std::atof(it->second.c_str()));
  };
  pbr::Inputs in;
  in.shader = kv.shader;
  in.base = text("$basetexture");
  in.bump = text("$bumpmap");
  in.envmapMask = text("$envmapmask");
  in.phongExponentTexture = text("$phongexponenttexture");
  in.ssbump = number("$ssbump") != 0;
  in.phong = number("$phong") != 0;
  in.envmap = !text("$envmap").empty();
  in.baseAlphaPhongMask = number("$basemapalphaphongmask") != 0;
  in.baseAlphaEnvmapMask = number("$basealphaenvmapmask") != 0;
  in.normalAlphaEnvmapMask = number("$normalmapalphaenvmapmask") != 0;
  in.envmapSphere = number("$envmapsphere") != 0;
  in.envmapMode = number("$envmapmode") != 0;
  in.phongExponent = number("$phongexponent");
  if (kv.values.count("$envmaptint"))
    in.hasEnvmapTint = parseColor(kv.values["$envmaptint"], in.envmapTint);
  in.phongAlbedoTint = number("$phongalbedotint") != 0;
  in.blendTintByBaseAlpha = number("$blendtintbybasealpha") != 0;
  if (kv.values.count("$color2"))
    in.hasColor2 = parseColor(kv.values["$color2"], in.color2);
  const auto &proxies = kv.blocks["proxies"];
  in.mwb = pbr::mwbBase(in.base) || proxies.count("mwenvmaptint") || proxies.count("arc9envmaptint");
  if (kv.values.count("$phongboost"))
    in.phongBoost = number("$phongboost");
  in.surfaceProp = text("$surfaceprop");
  in.luminance = textureLuminance;
  return pbr::convert(in);
}

// --- World geometry -----------------------------------------------------------
struct Material {
  std::string name, baseTexture;
  std::optional<cubemaps::CaptureOrigin> captureReference;
  std::vector<uint64_t> candidates; // texture hashes, largest mip first
  // Unlit or self-illuminated (materialShading), and its mask's hashes.
  engine_materials::Shading shading;
  std::vector<uint64_t> maskCandidates;
  bool maskDropped = false; // the mask was never uploaded: lit instead
  uint32_t categories = 0;
  bool alphaTest = false, modulate = false, blended = false;
  float opacity = 1;
  bool noCull = false; // $nocull: Source draws both sides
  bool owned = false; // ref.handle created here (water)
  bool requiresOwned = false; // optical imports cannot fall back to opaque texture lookup
  std::string importFailure;
  // Water: a translucent body of this colour (linear) and measurement
  // distance (remixapi_CameraMediumEXT when the camera is in it).
  bool waterBody = false;
  float waterColor[3] = {}, waterDistance = 0;
  uint8_t alphaRef = 179;
  MaterialRef ref;
  bool resolved = false;
  bool pointSample = false;
};

struct Geometry {
  std::vector<remixapi_HardcodedVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<lightmaps::Chart> lightmaps;
};

// Faces sharing a spatial cell and draw state: one mesh (a surface per
// material) and one instance.
struct Group {
  bool sky = false, alphaTest = false;
  // skybox: 3D skybox geometry baked into world space (outside the map's
  // visibility partition). noCull: $nocull materials, seen from both sides.
  bool skybox = false, noCull = false;
  uint8_t alphaRef = 0;
  uint32_t categories = 0;
  uint64_t id = 0;
  std::map<uint32_t, Geometry> surfaces; // by material index
  remixapi_MeshHandle mesh = nullptr;
  uint32_t builtResolved = ~0u; // resolved materials when last built
};

// One strip of a studio model (LOD 0): the unit the engine draws, so its
// dxvk mesh hash (with the base texture's) keys mod mesh replacements.
struct PropPart {
  // (Its triangles wind clockwise seen from the front, like the map's faces:
  // CullStatistics::propAlong / propAgainst.)
  remixapi_MeshHandle mesh = nullptr;
  int material = -1;
  uint64_t replacementHash = 0;
  std::vector<remixapi_HardcodedVertex> loadVertices; // temporary, released after sky placement clipping
  std::vector<float> positions; // model space, for occluders()
  std::vector<uint32_t> indices;
};
struct PropModel {
  std::vector<PropPart> parts;
  bool engineParts = false; // translucent or ignored meshes the engine still draws
  bool missingBakeOccluders = false;
  uint32_t additiveEngineParts = 0; // camera-only layers do not hide the destination
  std::vector<std::string> engineMaterials;
};
struct Prop {
  float transform[3][4];
  const PropModel *model = nullptr;
  uint64_t id = 0;
  // Seen by the engine's 3D skybox view: drawn by the engine, not here.
  bool skybox = false;
  // A 3D skybox prop baked into world space (path-traced skybox).
  bool skyTraced = false;
};

// info_overlay decals, clipped onto their faces; merged per cell and blend.
struct OverlayGroup {
  bool modulate = false; // DecalModulate: multiplies the surface (DESTCOLOR, SRCCOLOR)
  bool sky = false;
  bool skybox = false, noCull = false; // as Group
  uint64_t id = 0;
  std::map<uint32_t, Geometry> surfaces; // by material index
  remixapi_MeshHandle mesh = nullptr;
  uint32_t builtResolved = ~0u;
};

// The second texture of a two-layer world material (WorldVertexTransition, or
// LightmappedGeneric with $basetexture2), which a displacement's per-vertex
// alpha blends over the first: result = lerp(base, base2, blend). See build().
struct SecondLayer {
  bool present = false;  // the material has a $basetexture2
  bool prepared = false; // material and blend modulation looked up
  int material = -1;    // g_world.materials index of the layer (-1: its texture has no hash)
  // $blendmodulatetexture: blend = smoothstep(g - r, g + r, alpha) per pixel
  // in the game, here per vertex with the texture's mean g (centre) and r
  // (half width). $maskedblending: the texture's g alone.
  bool modulate = false, masked = false;
  float centre = 0.5f, halfWidth = 0.5f;
};
struct LayerStatistics {
  uint32_t materials = 0, noHash = 0, fourWay = 0, modulated = 0, modulateUnread = 0;
  uint32_t displacements = 0, layered = 0, flat = 0, withoutTexture = 0;
  uint32_t baseTriangles = 0, secondTriangles = 0, blendedTriangles = 0;
  uint32_t brushFaces = 0; // two-layer materials on faces without a displacement (no alpha: the first texture)
  float alphaMin = 255, alphaMax = 0;
  std::string names;
};

// What the camera does not see of the map (remixapi_InstanceInfoCameraCullingEXT;
// submit()): orientation of what was built, and the instances marked.
struct CullStatistics {
  // Faces whose dface_t::side is set: their plane already faces the way they
  // do (the normal is not flipped). Fan triangles wound against their face
  // (slivers at collinear vertices), kept. Faces and displacement triangles
  // by which way they wind seen from the face's front.
  uint32_t sideFaces = 0, slivers = 0, clockwise = 0, counterClockwise = 0, degenerate = 0;
  uint32_t dispClockwise = 0, dispCounterClockwise = 0;
  // Overlay polygons: wound like their face as built, re-wound, and with a
  // normal pointing to the back of their face.
  uint32_t overlayKept = 0, overlayRewound = 0, overlayBehind = 0;
  // Static prop triangles: cross(b - a, c - a) along / against their vertex
  // normals (against: clockwise seen from the normals' side).
  uint32_t propAlong = 0, propAgainst = 0, propPartsAlong = 0;
  // Last submit(): instances with back faces hidden, with unseen clusters
  // hidden, two-sided ($nocull), and of the 3D skybox (always seen).
  uint32_t backFaces = 0, visibility = 0, twoSided = 0, skybox = 0, instances = 0;
};

// A water or Refract material's $normalmap, uploaded from its file (the engine's
// own upload is of the frame its proxy shows now): every frame of its largest
// mip, played through one dynamic texture at its AnimatedTexture rate.
struct SurfaceNormal {
  std::string name;
  float requestedFps = 0; // cache identity, including failed/static loads
  uint32_t width = 0, height = 0;
  std::vector<std::vector<uint8_t>> frames; // RGBA8 (kept only when animated)
  float fps = 0;
  uint64_t hash = 0;
  remixapi_TextureHandle texture = nullptr;
  int shown = 0;
};

struct World {
  const remixapi_Interface *api = nullptr;
  std::vector<std::unique_ptr<SurfaceNormal>> surfaceNormals; // shared by source name and playback rate
  std::vector<remixapi_TextureHandle> opticalTints;
  std::string map;
  std::vector<Material> materials;
  std::vector<uint32_t> bakeMaterials;
  uint64_t bakeMaterialVersion = ~0ull;
  size_t bakeMaterialCount = 0;
  std::unordered_map<std::string, int> materialIndex;
  std::map<std::pair<std::string, int>, PropModel> propModels; // by model path and skin
  std::vector<std::unique_ptr<PropModel>> clippedSkyModels; // stable per-placement ownership
  std::vector<Prop> props;
  // Point location at runtime (the camera of a view): BSP tree and leaf areas.
  std::vector<Node> nodes;
  std::vector<Plane> planes;
  std::vector<int16_t> leafAreas;
  std::vector<int> leafWater; // the water material a leaf is in (-1: none)
  std::vector<uint8_t> leafPlayable; // empty, in a cluster, outside the 3D skybox
  int skyArea = -1;
  std::map<std::array<int32_t, 5>, OverlayGroup> overlayGroups;
  // Blended second layers of displacements: decal sheets with per-vertex alpha.
  std::map<std::array<int32_t, 5>, OverlayGroup> layerGroups;
  std::unordered_map<int, SecondLayer> secondLayers; // by base material index
  LayerStatistics layers;
  CullStatistics cull;
  std::unordered_map<std::string, int> overlayMaterialIndex;
  uint32_t overlays = 0, overlayTriangles = 0, waterMaterials = 0, surfaceNormalCount = 0, surfaceNormalAnimated = 0,
           waterDownFaces = 0;
  std::string surfaceNormalFailure; // last surface normal map load failure
  // Prop placements by 64-unit cell of their origin, for matching engine draws.
  std::unordered_map<uint64_t, std::vector<uint32_t>> propCells;
  std::map<std::array<int32_t, 6>, Group> groups;
  bool lightStylesAvailable = false;
  bool hasPlayerStart = false;
  SkyboxImages skyboxImages;
  float playerOrigin[3] = {}, playerAngles[3] = {};
  bool skyCamera = false;
  float skyOrigin[3] = {};
  float skyScale = 16;
  // Path-traced 3D skybox (OPENREMIX_PROXY_SKY_TRACE, default on): its faces
  // and props baked into world space, p_world = (p_sky - skyOrigin) * scale,
  // minus what lands inside the playable map (low-detail stand-ins of it).
  bool skyTraced = false;
  float playableMins[3] = {}, playableMaxs[3] = {};
  float skyReach = 0; // farthest traced sky point from the playable bounds' centre
  uint32_t skyTracedTriangles = 0, skyStandInTriangles = 0, skyClippedTriangles = 0, skyTracedProps = 0,
           skyStandInProps = 0, skyCoplanarPieces = 0;
  uint64_t frames = 0;
  // Statistics.
  uint32_t faces = 0, displacements = 0, skyFaces = 0, skippedTranslucent = 0, skippedIgnored = 0, keptIgnored = 0, triangles = 0;
  uint32_t propModelsFailed = 0, propParts = 0, skyProps = 0;
  std::vector<std::string> failedModels;
  std::vector<Light> lights;
  std::vector<std::array<float, 3>> reflectionSeeds;
  Occluders occluders;
  bool occludersBuilt = false;
  // Bumped whenever the instances submit() draws change (meshes built,
  // materials resolved, props found in the 3D skybox); a bridge batch
  // recorded at one version is replayed while it lasts.
  uint64_t submitVersion = 0, batchVersion = ~0ull, stableVersion = ~0ull, stableSince = 0;
  uint64_t receiverRevision = 0; // diagnostic binding generation; not geometry/bake membership
  uint64_t receiverDocumentRevision = ~0ull;
  size_t receiverMaterialCount = 0;
  bool batchValid = false;
  // The same instances as the runtime's scene instances (remix_native_ext.h):
  // added once per version and drawn by one call a frame.
  std::vector<uint64_t> sceneInstances;
  std::vector<uint64_t> bakeInstances; // world-space transport; excludes sky-camera polygons
  std::vector<remixapi_BakeSurfaceChartEXT> bakeCharts;
  uint64_t sceneVersion = ~0ull;
  bool sceneValid = false;
  uint64_t bakeVersion = ~0ull, bakeLights = 0;
  int bakeStatus = -1; // -1 waiting/unsupported runtime; otherwise native API result
  const char *bakeReason = "waiting for retained map inputs";
  uint64_t replayedFrames = 0;
  uint32_t recordings = 0;
  uint32_t styledLights = 0, skyboxLights = 0, otherLights = 0, surfaceLights = 0;
  uint32_t styleDrivenLights = 0; // of lights: scaled by their lightstyle
  // func_lod brushes, baked like world faces (lodModels): the server stops
  // sending one outside the player's PVS, and the engine no longer draws it
  // although the camera (or a reflection) still sees it. The positions of
  // their baked faces' corners identify the engine's draws of them.
  std::unordered_set<uint64_t> lodCorners;
  uint32_t lodEntities = 0, lodFaces = 0, lodLeft = 0;
} g_world;
bool g_active = false;

// The static props' placements for providesProp(), which other threads call
// (l4d2-rtx's model hook on the game thread while the render thread may be
// loading a map): immutable once published.
struct PropIndex {
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
  std::vector<std::array<float, 12>> transforms;
};
std::shared_ptr<const PropIndex> g_propIndex;
std::mutex g_propIndexLock;

void publishPropIndex(std::shared_ptr<const PropIndex> index) {
  std::lock_guard<std::mutex> lock(g_propIndexLock);
  g_propIndex = std::move(index);
}

float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The PVS of a cluster (one bit per cluster), empty without visibility data.
std::vector<uint8_t> pvs(const Bsp &bsp, int cluster) {
  const auto [vis, bytes] = bsp.lump<uint8_t>(kVisibility);
  if (!vis || bytes < 4 || cluster < 0)
    return {};
  int32_t clusters;
  std::memcpy(&clusters, vis, 4);
  if (cluster >= clusters || size_t(8 + clusters * 8) > bytes)
    return {};
  int32_t offset;
  std::memcpy(&offset, vis + 4 + cluster * 8, 4);
  std::vector<uint8_t> out(size_t(clusters + 7) / 8, 0);
  size_t at = size_t(offset), c = 0;
  while (c < out.size() && at < bytes) {
    if (vis[at]) {
      out[c++] = vis[at++];
    } else if (at + 1 < bytes) {
      c += vis[at + 1];
      at += 2;
    } else {
      break;
    }
  }
  return out;
}

int leafAt(const Bsp &bsp, const float p[3]) {
  const auto [nodes, nodeCount] = bsp.lump<Node>(kNodes);
  const auto [planes, planeCount] = bsp.lump<Plane>(kPlanes);
  int index = 0;
  for (int guard = 0; index >= 0 && guard < 4096; ++guard) {
    if (size_t(index) >= nodeCount || size_t(nodes[index].plane) >= planeCount)
      return -1;
    const auto &plane = planes[nodes[index].plane];
    index = nodes[index].children[dot(plane.normal, p) - plane.dist >= 0 ? 0 : 1];
  }
  return index < 0 ? -1 - index : -1;
}

void parseMapCameras(const Bsp &bsp) {
  const auto [text, length] = bsp.lump<char>(kEntities);
  if (!text)
    return;
  const std::string entities(text, length);
  for (size_t open = entities.find('{'); open != std::string::npos; open = entities.find('{', open + 1)) {
    const size_t close = entities.find('}', open);
    if (close == std::string::npos)
      break;
    KeyValues kv;
    if (!parseKeyValues("entity " + entities.substr(open, close - open + 1), kv))
      continue;
    const auto classname = lower(kv.values["classname"]);
    if (classname == "worldspawn")
      g_world.skyboxImages.name = kv.values["skyname"];
    if (!g_world.hasPlayerStart && (classname == "info_player_start" || classname == "info_player_deathmatch" ||
                                    classname == "gmod_player_start")) {
      g_world.hasPlayerStart = std::sscanf(kv.values["origin"].c_str(), "%f %f %f", &g_world.playerOrigin[0],
                                         &g_world.playerOrigin[1], &g_world.playerOrigin[2]) == 3;
      std::sscanf(kv.values["angles"].c_str(), "%f %f %f", &g_world.playerAngles[0],
                  &g_world.playerAngles[1], &g_world.playerAngles[2]);
    }
    if (classname != "sky_camera" || g_world.skyCamera)
      continue;
    if (std::sscanf(kv.values["origin"].c_str(), "%f %f %f", &g_world.skyOrigin[0], &g_world.skyOrigin[1],
                    &g_world.skyOrigin[2]) == 3) {
      g_world.skyCamera = true;
      if (kv.values.count("scale"))
        g_world.skyScale = float(std::atof(kv.values["scale"].c_str()));
      if (!(g_world.skyScale > 0))
        g_world.skyScale = 16;
    }
  }
}

// func_lod brush models (indices into the models lump). One with an origin
// or angles (a brush model placed away from where it was compiled) is left
// to the engine.
std::vector<int32_t> lodModels(const Bsp &bsp, size_t modelCount) {
  std::vector<int32_t> out;
  const auto [text, length] = bsp.lump<char>(kEntities);
  if (!text)
    return out;
  const std::string entities(text, length);
  for (size_t open = entities.find('{'); open != std::string::npos; open = entities.find('{', open + 1)) {
    const size_t close = entities.find('}', open);
    if (close == std::string::npos)
      break;
    KeyValues kv;
    if (!parseKeyValues("entity " + entities.substr(open, close - open + 1), kv) ||
        lower(kv.values["classname"]) != "func_lod")
      continue;
    const std::string &model = kv.values["model"];
    const int index = model.size() > 1 && model[0] == '*' ? std::atoi(model.c_str() + 1) : 0;
    float origin[3] = {}, angles[3] = {};
    std::sscanf(kv.values["origin"].c_str(), "%f %f %f", &origin[0], &origin[1], &origin[2]);
    std::sscanf(kv.values["angles"].c_str(), "%f %f %f", &angles[0], &angles[1], &angles[2]);
    bool placed = false;
    for (int k = 0; k < 3; ++k)
      placed |= origin[k] != 0 || angles[k] != 0;
    if (index <= 0 || size_t(index) >= modelCount || placed) {
      ++g_world.lodLeft;
      continue;
    }
    out.push_back(index);
  }
  return out;
}

// A corner position, to the eighth of a unit.
uint64_t cornerKey(const float p[3]) {
  const int32_t q[3] = {int32_t(std::lround(p[0] * 8)), int32_t(std::lround(p[1] * 8)), int32_t(std::lround(p[2] * 8))};
  return XXH3_64bits(q, sizeof(q));
}

// Bounds-checked reads from a file image.
template <typename T> bool get(const std::vector<uint8_t> &b, size_t at, T &out) {
  if (at > b.size() || b.size() - at < sizeof(T))
    return false;
  std::memcpy(&out, b.data() + at, sizeof(T));
  return true;
}
int32_t i32(const std::vector<uint8_t> &b, size_t at) {
  int32_t v = 0;
  get(b, at, v);
  return v;
}
std::string cstring(const std::vector<uint8_t> &b, size_t at) {
  if (at >= b.size())
    return {};
  const auto *p = reinterpret_cast<const char *>(b.data() + at);
  return std::string(p, strnlen(p, b.size() - at));
}

int materialByName(const std::string &name, bool brush = false);
} // namespace
int areaAt(const float p[3]);
namespace {

// A static prop's model: body 0 of every body part at LOD 0, a part per VTX
// strip (vertices in strip-group order from the VVD, indices rebased), each
// with the dxvk mesh hash of the engine's draw of it.
bool loadPropModel(const std::string &path, int skin, PropModel &out) {
  std::vector<uint8_t> mdl, vvd, vtx;
  const std::string base = path.substr(0, path.size() - 4);
  if (!fs::read(path, mdl) || !fs::read(base + ".vvd", vvd) || !fs::read(base + ".dx90.vtx", vtx) ||
      mdl.size() < 240 || vvd.size() < 64 || vtx.size() < 36)
    return false;
  // studiohdr_t
  const int32_t numTextures = i32(mdl, 204), textureIndex = i32(mdl, 208), numCd = i32(mdl, 212),
                cdIndex = i32(mdl, 216), numSkinRef = i32(mdl, 220), numFamilies = i32(mdl, 224),
                skinIndex = i32(mdl, 228), numBodyParts = i32(mdl, 232), bodyPartIndex = i32(mdl, 236);
  std::vector<std::string> textures, cdPaths;
  for (int t = 0; t < numTextures; ++t) {
    const size_t at = size_t(textureIndex) + size_t(t) * 64; // mstudiotexture_t
    textures.push_back(cstring(mdl, at + i32(mdl, at)));
  }
  for (int c = 0; c < numCd; ++c)
    cdPaths.push_back(lower(cstring(mdl, size_t(i32(mdl, size_t(cdIndex) + c * 4)))));
  const int family = skin >= 0 && skin < numFamilies ? skin : 0;
  auto textureOf = [&](int material) -> int {
    int16_t ref = int16_t(material);
    if (material < numSkinRef)
      get(mdl, size_t(skinIndex) + (size_t(family) * numSkinRef + material) * 2, ref);
    return ref;
  };
  // VVD: LOD 0 vertices through the fixup table.
  const int32_t numFixups = i32(vvd, 48), fixupStart = i32(vvd, 52), vertexStart = i32(vvd, 56);
  std::vector<uint32_t> vertexMap;
  if (numFixups > 0) {
    for (int f = 0; f < numFixups; ++f) {
      const size_t at = size_t(fixupStart) + size_t(f) * 12;
      const int32_t lod = i32(vvd, at), source = i32(vvd, at + 4), count = i32(vvd, at + 8);
      if (lod >= 0 && source >= 0 && count >= 0)
        for (int32_t v = 0; v < count; ++v)
          vertexMap.push_back(uint32_t(source + v));
    }
  } else {
    for (int32_t v = 0; v < i32(vvd, 16); ++v)
      vertexMap.push_back(uint32_t(v));
  }
  // mstudiovertex_t (48 bytes): bone weights 16, position 12, normal 12, texcoord 8.
  auto vvdVertex = [&](uint32_t index, remixapi_HardcodedVertex &v, const uint8_t *&position) {
    if (index >= vertexMap.size())
      return false;
    const size_t at = size_t(vertexStart) + size_t(vertexMap[index]) * 48;
    if (at + 48 > vvd.size())
      return false;
    position = vvd.data() + at + 16;
    std::memcpy(v.position, position, 12);
    std::memcpy(v.normal, vvd.data() + at + 28, 12);
    std::memcpy(v.texcoord, vvd.data() + at + 40, 8);
    v.color = 0xffffffff;
    return true;
  };
  // VTX (OptimizedModel, packed): FileHeader 36, BodyPart 8, Model 8, ModelLOD 12,
  // Mesh 9, StripGroup 25, Strip 27, Vertex 9.
  const int32_t vtxBodyParts = i32(vtx, 28), vtxBodyPartOffset = i32(vtx, 32);
  bool complete = vtxBodyParts >= numBodyParts;
  for (int b = 0; b < numBodyParts && b < vtxBodyParts; ++b) {
    const size_t bodyPart = size_t(bodyPartIndex) + size_t(b) * 16; // mstudiobodyparts_t
    if (i32(mdl, bodyPart + 4) < 1)
      continue;
    const size_t model = bodyPart + i32(mdl, bodyPart + 12); // body 0: first model
    const int32_t numMeshes = i32(mdl, model + 72);
    const size_t meshIndex = model + i32(mdl, model + 76);
    const int32_t modelVertex = i32(mdl, model + 84) / 48;
    const size_t vtxBody = size_t(vtxBodyPartOffset) + size_t(b) * 8;
    const size_t vtxModel = vtxBody + i32(vtx, vtxBody + 4);
    const size_t vtxLod = vtxModel + i32(vtx, vtxModel + 4);
    const int32_t vtxMeshes = i32(vtx, vtxLod);
    const size_t vtxMeshOffset = vtxLod + i32(vtx, vtxLod + 4);
    if (vtxMeshes < numMeshes)
      complete = false;
    for (int m = 0; m < numMeshes && m < vtxMeshes; ++m) {
      const size_t mesh = meshIndex + size_t(m) * 116; // mstudiomesh_t
      const int32_t materialRef = i32(mdl, mesh), meshVertex = i32(mdl, mesh + 12);
      const int texture = textureOf(materialRef);
      if (texture < 0 || size_t(texture) >= textures.size()) {
        complete = false;
        continue;
      }
      int material = -1;
      std::string sourceMaterial;
      bool found = false;
      for (const auto &cd : cdPaths) {
        std::string name = cd + lower(textures[texture]);
        uint64_t bytes;
        if (fs::size("materials/" + name + ".vmt", bytes)) {
          material = materialByName(name);
          sourceMaterial = name;
          found = true;
          break;
        }
      }
      if (!found) {
        complete = false;
        continue;
      }
      // Translucent (blended, so still drawn by the engine) or ignored.
      if (material < 0) {
        out.engineParts = true;
        out.engineMaterials.push_back(sourceMaterial);
        KeyValues deferred;
        const bool additive = loadMaterial(sourceMaterial, deferred) &&
                              std::atoi(deferred.values["$additive"].c_str()) != 0;
        // Source additive draws remain in the live compatibility pass. Their
        // destination factor is one, so they do not remove visibility behind
        // them and need no frozen occluder. Unknown/ignored opaque parts still
        // block readiness: camera ownership alone says nothing about transport.
        out.additiveEngineParts += additive;
        out.missingBakeOccluders |= !additive;
        continue;
      }
      // Without a texture hash (format the proxy does not upload) the part
      // still draws, with the default material.
      const auto &candidates = g_world.materials[material].candidates;
      const uint64_t textureHash = candidates.empty() ? 0 : candidates[0];
      const size_t vtxMesh = vtxMeshOffset + size_t(m) * 9;
      const int32_t numGroups = i32(vtx, vtxMesh);
      const size_t groupOffset = vtxMesh + i32(vtx, vtxMesh + 4);
      for (int g = 0; g < numGroups; ++g) {
        const size_t group = groupOffset + size_t(g) * 25;
        const int32_t numVerts = i32(vtx, group), vertOffset = i32(vtx, group + 4), numIndices = i32(vtx, group + 8),
                      indexOffset = i32(vtx, group + 12), numStrips = i32(vtx, group + 16),
                      stripOffset = i32(vtx, group + 20);
        if (numVerts <= 0 || numIndices <= 0)
          continue;
        std::vector<remixapi_HardcodedVertex> vertices(static_cast<size_t>(numVerts));
        std::vector<const uint8_t *> positions(static_cast<size_t>(numVerts));
        bool ok = true;
        for (int v = 0; v < numVerts && ok; ++v) {
          uint16_t original = 0;
          ok = get(vtx, group + vertOffset + size_t(v) * 9 + 4, original) &&
               vvdVertex(uint32_t(modelVertex + meshVertex + original), vertices[v], positions[v]);
        }
        std::vector<uint16_t> indices(static_cast<size_t>(numIndices));
        for (int i = 0; i < numIndices && ok; ++i)
          ok = get(vtx, group + indexOffset + size_t(i) * 2, indices[i]) && indices[i] < numVerts;
        if (!ok) {
          complete = false;
          continue;
        }
        for (int st = 0; st < numStrips; ++st) {
          const size_t strip = group + stripOffset + size_t(st) * 27;
          const int32_t count = i32(vtx, strip), first = i32(vtx, strip + 4);
          uint8_t flags = 0;
          get(vtx, strip + 18, flags); // StripHeader_t::flags: 1 tri list, 2 tri strip
          if (count < 3 || first < 0 || first + count > numIndices || (flags & 2)) {
            complete = false;
            continue;
          }
          const auto begin = indices.begin() + first, end = begin + count;
          const auto [lo, hi] = std::minmax_element(begin, end);
          const uint16_t low = *lo, high = *hi;
          if (low == high)
            continue;
          // dxvk::geometryHash of the draw (dxvk_hash.cpp).
          std::vector<uint16_t> rebased(begin, end);
          for (auto &i : rebased)
            i = uint16_t(i - low);
          const uint64_t indexHash = XXH3_64bits(rebased.data(), rebased.size() * 2);
          std::vector<uint16_t> referenced(begin, end);
          std::sort(referenced.begin(), referenced.end());
          referenced.erase(std::unique(referenced.begin(), referenced.end()), referenced.end());
          uint64_t positionHash = 0;
          for (uint16_t i : referenced)
            positionHash = XXH3_64bits_withSeed(positions[i], 12, positionHash);
          const uint32_t descriptor[4] = {uint32_t(count), uint32_t(high - low + 1), 3, 0};
          uint64_t descriptorHash = 0;
          for (uint32_t field : descriptor)
            descriptorHash = XXH3_64bits_withSeed(&field, 4, descriptorHash);
          const uint64_t geometry = XXH64(&descriptorHash, 8, XXH64(&indexHash, 8, positionHash));

          std::vector<remixapi_HardcodedVertex> partVertices(vertices.begin() + low, vertices.begin() + high + 1);
          std::vector<uint32_t> partIndices(rebased.begin(), rebased.end());
          {
            // Which way the strip winds against its vertex normals.
            uint32_t along = 0, against = 0;
            for (size_t i = 0; i + 2 < partIndices.size(); i += 3) {
              const auto &a = partVertices[partIndices[i]], &b = partVertices[partIndices[i + 1]],
                         &c = partVertices[partIndices[i + 2]];
              const float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
              const float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
              const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
              float d = 0;
              for (int k = 0; k < 3; ++k)
                d += n[k] * (a.normal[k] + b.normal[k] + c.normal[k]);
              ++(d > 0 ? along : against);
            }
            g_world.cull.propAlong += along;
            g_world.cull.propAgainst += against;
            g_world.cull.propPartsAlong += along > against;
          }
          remixapi_MeshInfoSurfaceTriangles surface = {};
          surface.vertices_values = partVertices.data();
          surface.vertices_count = partVertices.size();
          surface.indices_values = partIndices.data();
          surface.indices_count = partIndices.size();
          remixapi_MeshInfo info = {};
          info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
          info.hash = geometry ^ textureHash;
          info.surfaces_values = &surface;
          info.surfaces_count = 1;
          PropPart part;
          part.material = material;
          part.replacementHash = geometry ^ textureHash;
          if (g_world.api->CreateMesh(&info, &part.mesh) != REMIXAPI_ERROR_CODE_SUCCESS) {
            complete = false;
            continue;
          }
          for (const auto &v : partVertices)
            part.positions.insert(part.positions.end(), {v.position[0], v.position[1], v.position[2]});
          if (g_world.skyTraced) part.loadVertices = std::move(partVertices);
          part.indices = std::move(partIndices);
          out.parts.push_back(std::move(part));
          ++g_world.propParts;
        }
      }
    }
  }
  // A partial model would replace the engine's whole prop (its draws are
  // dropped by placement): leave such models to the engine.
  if (!complete) {
    for (auto &part : out.parts)
      if (part.mesh)
        g_world.api->DestroyMesh(part.mesh);
    g_world.propParts -= uint32_t(out.parts.size());
    out.parts.clear();
  }
  return !out.parts.empty();
}

uint64_t propCell(const float origin[3]) {
  const int32_t c[3] = {int32_t(std::floor(origin[0] / 64)), int32_t(std::floor(origin[1] / 64)),
                        int32_t(std::floor(origin[2] / 64))};
  return (uint64_t(uint32_t(c[0]) & 0x1fffff) << 42) | (uint64_t(uint32_t(c[1]) & 0x1fffff) << 21) |
         uint64_t(uint32_t(c[2]) & 0x1fffff);
}

// Source AngleMatrix: rows of the model-to-world 3x4 (columns forward, left, up).
void angleMatrix(const float angles[3], const float origin[3], float m[3][4]) {
  constexpr float kRad = 3.14159265358979f / 180;
  const float sp = std::sin(angles[0] * kRad), cp = std::cos(angles[0] * kRad), sy = std::sin(angles[1] * kRad),
              cy = std::cos(angles[1] * kRad), sr = std::sin(angles[2] * kRad), cr = std::cos(angles[2] * kRad);
  m[0][0] = cp * cy;
  m[1][0] = cp * sy;
  m[2][0] = -sp;
  m[0][1] = sr * sp * cy - cr * sy;
  m[1][1] = sr * sp * sy + cr * cy;
  m[2][1] = sr * cp;
  m[0][2] = cr * sp * cy + sr * sy;
  m[1][2] = cr * sp * sy - sr * cy;
  m[2][2] = cr * cp;
  for (int r = 0; r < 3; ++r)
    m[r][3] = origin[r];
}

// OPENREMIX_PROXY_EMISSIVE=0: every material lit, as before.
bool emissiveEnabled() {
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_EMISSIVE", v, sizeof(v)) && v[0] == '0');
  }();
  return enabled;
}

// OPENREMIX_PROXY_WATER_NORMALS=0: water without its normal map (a flat surface).
bool waterNormalsEnabled() {
  static const bool on = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_WATER_NORMALS", v, sizeof(v)) && v[0] == '0');
  }();
  return on;
}

// Every frame of a VTF's largest mip as RGBA8 (R in the lowest byte), at
// most 64 MB of them. Else why not: a missing file, a format not read here.
const char *vtfFrames(const std::string &name, uint32_t &width, uint32_t &height,
                      std::vector<std::vector<uint8_t>> &out) {
  const std::string path = vtfPath(name);

  std::vector<uint8_t> header;
  uint64_t total = 0;
  if (!fs::size(path, total))
    return "no file";
  if (!fs::read(path, header, 0, 80) || header.size() < 64 || std::memcmp(header.data(), "VTF", 4) != 0)
    return "not read, or not a VTF";
  uint16_t w, h, frames;
  uint32_t flags;
  int32_t format;
  std::memcpy(&w, header.data() + 16, 2);
  std::memcpy(&h, header.data() + 18, 2);
  std::memcpy(&flags, header.data() + 20, 4);
  std::memcpy(&frames, header.data() + 24, 2);
  std::memcpy(&format, header.data() + 52, 4);
  frames = std::max<uint16_t>(frames, 1);
  uint32_t blockBytes = 0, pixelBytes = 0;
  int order[4] = {0, 1, 2, 3}; // where r, g, b, a are in a pixel (-1: none)
  switch (format) {
  case 0: pixelBytes = 4; break;                                             // RGBA8888
  case 1: pixelBytes = 4; order[0] = 3; order[1] = 2; order[2] = 1; order[3] = 0; break; // ABGR8888
  case 2: pixelBytes = 3; order[3] = -1; break;                              // RGB888
  case 3: pixelBytes = 3; order[0] = 2; order[2] = 0; order[3] = -1; break;  // BGR888
  case 12: pixelBytes = 4; order[0] = 2; order[2] = 0; break;                // BGRA8888
  case 16: pixelBytes = 4; order[0] = 2; order[2] = 0; order[3] = -1; break; // BGRX8888
  case 13: case 20: blockBytes = 8; break;                                   // DXT1
  case 14: case 15: blockBytes = 16; break;                                   // DXT3, DXT5
  default: return "a format not read here";
  }
  const uint64_t level = blockBytes ? uint64_t((w + 3) / 4) * ((h + 3) / 4) * blockBytes : uint64_t(w) * h * pixelBytes;
  const uint32_t faces = (flags & 0x4000) ? 6 : 1;
  // The largest mip ends the file: every frame (and face) of it.
  const uint64_t bytes = level * frames * faces;
  if (!w || !h || uint64_t(w) * h * 4 * frames > (64u << 20) || bytes > total)
    return "too large, or shorter than its header says";
  std::vector<uint8_t> data;
  if (!fs::read(path, data, total - bytes, bytes) || data.size() != bytes)
    return "its image not read";
  out.clear();
  for (uint32_t f = 0; f < frames; ++f) {
    const uint8_t *src = data.data() + level * f * faces;
    if (blockBytes) {
      out.push_back(blockBytes == 8 ? formats::decodeBC1(src, w, h)
                    : format == 14  ? formats::decodeBC2(src, w, h)
                                    : formats::decodeBC3(src, w, h));
      continue;
    }
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (size_t p = 0; p < size_t(w) * h; ++p)
      for (int c = 0; c < 4; ++c)
        rgba[p * 4 + c] = order[c] < 0 ? 255 : src[p * pixelBytes + size_t(order[c])];
    out.push_back(std::move(rgba));
  }
  width = w;
  height = h;
  return nullptr;
}

// The texture of a water or Refract material's $normalmap (nullptr: none), made once per
// name and playback rate. fps: its AnimatedTexture rate (0: the first frame only, as Source
// shows a $bumpframe nothing animates).
SurfaceNormal *surfaceNormal(const std::string &name, float fps) {
  // Nonpositive/invalid rates retain the existing nonanimated policy. Distinct
  // positive rates need distinct runtime textures: uploads mutate the texture.
  const float rate = std::isfinite(fps) && fps > 0 ? fps : 0;
  for (auto &known : g_world.surfaceNormals)
    if (known->name == name && known->requestedFps == rate)
      return known->texture ? known.get() : nullptr;
  auto normal = std::make_unique<SurfaceNormal>();
  normal->name = name;
  normal->requestedFps = rate;
  SurfaceNormal *made = normal.get();
  g_world.surfaceNormals.push_back(std::move(normal));
  std::vector<std::vector<uint8_t>> frames;
  const char *failure = g_world.api ? vtfFrames(name, made->width, made->height, frames) : "no runtime";
  if (failure || frames.empty()) {
    g_world.surfaceNormalFailure = name + ": " + (failure ? failure : "no frames");
    return nullptr;
  }
  const bool animated = frames.size() > 1 && rate > 0;
  made->hash = XXH3_64bits_withSeed(name.data(), name.size(), 0x77617465726e726dull) | 1;
  if (rate > 0)
    made->hash = XXH3_64bits_withSeed(&rate, sizeof(rate), made->hash) | 1;
  remixapi_TextureInfoDynamicEXT dynamic = {};
  dynamic.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT;
  dynamic.dynamic = 1;
  remixapi_TextureInfoGenerateMipsEXT mips = {};
  mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
  mips.generate = 1;
  mips.pNext = animated ? &dynamic : nullptr;
  remixapi_TextureInfo info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
  info.pNext = &mips;
  info.hash = made->hash;
  info.width = made->width;
  info.height = made->height;
  info.depth = info.mipLevels = 1;
  info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
  info.data = frames[0].data();
  info.dataSize = frames[0].size();
  if (g_world.api->CreateTexture(&info, &made->texture) != REMIXAPI_ERROR_CODE_SUCCESS) {
    made->texture = nullptr;
    g_world.surfaceNormalFailure = name + ": the runtime refused its texture";
    return nullptr;
  }
  ++g_world.surfaceNormalCount;
  if (animated) {
    made->fps = rate;
    made->frames = std::move(frames);
    ++g_world.surfaceNormalAnimated;
  }
  return made;
}

// Animated surface normals show the frame of the time (submit, every frame).
void animateSurfaceNormals() {
  static const auto update = reinterpret_cast<PFN_remixapi_UpdateTextureEXT>(
      reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_UpdateTextureEXT")));
  static const ULONGLONG start = GetTickCount64();
  const double seconds = double(GetTickCount64() - start) / 1000;
  for (auto &normal : g_world.surfaceNormals) {
    if (!update || !normal->texture || normal->frames.size() < 2)
      continue;
    const int frame = int(uint64_t(seconds * normal->fps) % normal->frames.size());
    if (frame == normal->shown)
      continue;
    const auto &pixels = normal->frames[size_t(frame)];
    if (update(normal->texture, 0, 0, normal->width, normal->height, REMIXAPI_FORMAT_R8G8B8A8_UNORM, pixels.data(),
               normal->width * 4) == REMIXAPI_ERROR_CODE_SUCCESS)
      normal->shown = frame;
  }
}

// Index into g_world.materials, or -1 for materials left to the engine
// (translucent: its blended draws are kept) or ignored by rtx.conf. brush:
// for the map's own faces, which rtx.conf does not remove (its ignore list
// was made for dxvk-remix and holds world textures; the faces would leave
// holes in the map).
void loadRefract(const KeyValues &kv, const std::string &identity, Material &material) {
  material.requiresOwned = true;
  refract::Description description;
  const auto proxies = kv.blocks.find("proxies");
  if (!refract::describe(kv.values, proxies == kv.blocks.end() ? refract::Values{} : proxies->second,
                         description, material.importFailure))
    return;
  material.noCull = description.noCull;
  if (!g_world.api) {
    material.importFailure = "Refract runtime unavailable";
    return;
  }
  auto frameCount = [&](const std::string &name, uint32_t &count) {
    if (name.empty())
      return true;
    std::vector<uint8_t> header;
    if (!fs::read(vtfPath(name), header, 0, 64) || header.size() < 64 ||
        std::memcmp(header.data(), "VTF", 4)) {
      material.importFailure = "Refract texture header unavailable: " + name;
      return false;
    }
    uint16_t frames = 0;
    uint32_t flags = 0;
    std::memcpy(&frames, header.data() + 24, sizeof(frames));
    std::memcpy(&flags, header.data() + 20, sizeof(flags));
    if (flags & 0x4000) {
      material.importFailure = "Refract requires a 2D texture: " + name;
      return false;
    }
    count = std::max<uint32_t>(frames, 1);
    return true;
  };
  refract::Resources resources;
  resources.hash = XXH3_64bits_withSeed(identity.data(), identity.size(), 0x7265667261637431ull) | 1;
  if (!frameCount(description.normalTexture, resources.normalFrames) ||
      !frameCount(description.animatedTexture, resources.proxyFrames) ||
      !frameCount(description.tintTexture, resources.tintFrames))
    return;
  if (resources.tintFrames != 1 || (!description.animatedTexture.empty() &&
      resources.normalFrames != resources.proxyFrames)) {
    material.importFailure = "Refract animation frame counts are unsupported";
    return;
  }
  wchar_t normalPath[32]{}, tintPath[32]{};
  if (!description.normalTexture.empty()) {
    auto *normal = surfaceNormal(description.normalTexture, description.framesPerSecond);
    if (!normal) {
      material.importFailure = "Refract normal unavailable: " + g_world.surfaceNormalFailure;
      return;
    }
    swprintf(normalPath, 32, L"0x%llX", static_cast<unsigned long long>(normal->hash));
    resources.normal = normalPath;
  }
  remixapi_TextureHandle tint = nullptr;
  if (!description.tintTexture.empty()) {
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> frames;
    const char *failure = vtfFrames(description.tintTexture, width, height, frames);
    std::vector<float> pixels;
    if (failure || frames.size() != 1 || !refract::passiveTint(frames[0], description.tint, pixels)) {
      material.importFailure = "Refract tint conversion failed: " + description.tintTexture;
      return;
    }
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = XXH3_64bits_withSeed(pixels.data(), pixels.size() * sizeof(float), resources.hash) | 1;
    info.width = width;
    info.height = height;
    info.depth = info.mipLevels = 1;
    info.format = REMIXAPI_FORMAT_R32G32B32A32_SFLOAT_EXT;
    info.data = pixels.data();
    info.dataSize = pixels.size() * sizeof(float);
    if (g_world.api->CreateTexture(&info, &tint) != REMIXAPI_ERROR_CODE_SUCCESS) {
      material.importFailure = "Runtime rejected Refract tint texture";
      return;
    }
    swprintf(tintPath, 32, L"0x%llX", static_cast<unsigned long long>(info.hash));
    resources.tint = tintPath;
  }
  std::vector<uint64_t> keys;
  for (const auto &name : {description.normalTexture, description.tintTexture}) {
    if (name.empty())
      continue;
    for (const bool engine : {false, true})
      for (const bool legacy : {false, true})
        for (auto key : textureHashes(vtfPath(name), engine, legacy))
          if (key && keys.size() < 32 && std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
  }
  remixapi_MaterialInfoReplacementHashEXT primary{};
  primary.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_REPLACEMENT_HASH_EXT;
  primary.materialHash = keys.empty() ? 0 : keys.front();
  remixapi_MaterialReplacementKeysEXT alternates{};
  alternates.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_REPLACEMENT_KEYS_EXT;
  alternates.pNext = &primary;
  alternates.count = uint32_t(keys.size());
  alternates.keys = keys.data();
  resources.replacementChain = keys.empty() ? nullptr : &alternates;
  const auto status = refract::create(g_world.api->CreateMaterial, description, resources,
                                     material.ref.handle, material.importFailure);
  if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
    material.resolved = material.owned = true;
    if (tint)
      g_world.opticalTints.push_back(tint);
  } else if (tint) {
    g_world.api->DestroyTexture(tint);
  }
}

int materialByName(const std::string &name, bool brush) {
  const std::string key = lower(name);
  const std::string cacheKey = brush ? "\x01" + key : key;
  if (auto it = g_world.materialIndex.find(cacheKey); it != g_world.materialIndex.end())
    return it->second;
  int &slot = g_world.materialIndex[cacheKey];
  slot = -1;
  Material m;
  m.name = name;
  KeyValues kv;
  const bool loaded = loadMaterial(m.name, kv);
  if (loaded) {
    const auto capture = kv.values.find("$envmap");
    if (capture != kv.values.end()) m.captureReference = cubemaps::captureOrigin(capture->second, g_world.map);
  }
  if (loaded && kv.shader == "refract") {
    loadRefract(kv, cacheKey, m);
    slot = int(g_world.materials.size());
    g_world.materials.push_back(std::move(m));
    return slot;
  }
  if (kv.shader == "water") {
    // Water: Source draws it from render targets (refraction, reflection),
    // which have no counterpart here. A translucent body (the runtime's thick
    // translucent material): Fresnel reflection, and below the surface the
    // bottom fading into the fog colour with depth, nearly gone at $fogend as
    // Source's water fog has it. OPENREMIX_PROXY_WATER_TRANSLUCENT=0: a smooth
    // sheet of its fog colour (linear) blended by an opacity from $fogend
    // (murky water, a short $fogend, nearly opaque).
    float fog[3] = {100, 110, 120};
    const std::string &color = kv.values["$fogcolor"];
    const size_t digits = color.find_first_of("0123456789");
    if (digits != std::string::npos)
      std::sscanf(color.c_str() + digits, "%f %f %f", &fog[0], &fog[1], &fog[2]);
    const float fogEnd = kv.values.count("$fogend") ? float(std::atof(kv.values["$fogend"].c_str())) : 128;
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {std::pow(std::clamp(fog[0], 0.0f, 255.0f) / 255, 2.2f),
                             std::pow(std::clamp(fog[1], 0.0f, 255.0f) / 255, 2.2f),
                             std::pow(std::clamp(fog[2], 0.0f, 255.0f) / 255, 2.2f)};
    opaque.opacityConstant = std::clamp(0.35f + 40.0f / std::max(fogEnd, 1.0f), 0.35f, 0.9f);
    opaque.roughnessConstant = 0.05f;
    opaque.alphaTestType = 7;
    const auto waterKeys = waterReplacementKeys(kv);
    remixapi_MaterialInfoReplacementHashEXT replacement = {};
    replacement.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_REPLACEMENT_HASH_EXT;
    replacement.materialHash = waterKeys.empty() ? 0 : waterKeys.front();
    remixapi_MaterialReplacementKeysEXT alternates{};
    alternates.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_REPLACEMENT_KEYS_EXT;
    alternates.pNext = &replacement;
    alternates.count = uint32_t(waterKeys.size());
    alternates.keys = waterKeys.data();
    opaque.pNext = waterKeys.empty() ? nullptr : &alternates;
    // Its $normalmap, sampled as the Water shader samples it: animated by an
    // AnimatedTexture proxy, moved by a TextureScroll proxy on $bumptransform,
    // and with $scroll1 set ("multi-texture") averaged with two more scrolling
    // samples ($scroll1, $scroll2; not with a $flowmap, which Source prefers).
    const auto &proxies = kv.blocks["proxies"];
    auto proxy = [&](const char *key) {
      const auto it = proxies.find(key);
      return it == proxies.end() ? std::string() : lower(it->second);
    };
    const float fps = proxy("animatedtexture.animatedtexturevar") == "$normalmap"
                          ? (proxies.count("animatedtexture.animatedtextureframerate")
                                 ? float(std::atof(proxy("animatedtexture.animatedtextureframerate").c_str()))
                                 : 15.0f)
                          : 0.0f;
    SurfaceNormal *normal = waterNormalsEnabled() && kv.values.count("$normalmap")
                              ? surfaceNormal(lower(kv.values["$normalmap"]), fps)
                              : nullptr;
    remixapi_MaterialNormalEncodingEXT encoding = {};
    encoding.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT;
    encoding.encoding = 5;
    encoding.pNext = opaque.pNext;
    remixapi_MaterialNormalScrollEXT scroll = {};
    scroll.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_SCROLL_EXT;
    scroll.pNext = &encoding;
    scroll.layers = 1;
    wchar_t normalPath[32];
    if (normal) {
      if (proxy("texturescroll.texturescrollvar") == "$bumptransform") {
        const float rate = float(std::atof(proxy("texturescroll.texturescrollrate").c_str()));
        const float angle = float(std::atof(proxy("texturescroll.texturescrollangle").c_str())) * 3.14159265f / 180;
        scroll.base[0] = rate * std::cos(angle);
        scroll.base[1] = rate * std::sin(angle);
      }
      float scroll1[3] = {}, scroll2[3] = {};
      if (kv.values.count("$scroll1"))
        parseColor(kv.values["$scroll1"], scroll1);
      if (kv.values.count("$scroll2"))
        parseColor(kv.values["$scroll2"], scroll2);
      if (std::fabs(scroll1[0]) > 0 && !kv.values.count("$flowmap")) {
        scroll.layers = 3;
        std::copy_n(scroll1, 2, scroll.layer1);
        std::copy_n(scroll2, 2, scroll.layer2);
      }
      opaque.pNext = &scroll;
      swprintf(normalPath, 32, L"0x%llX", static_cast<unsigned long long>(normal->hash));
    }
    static const bool translucentWater = [] {
      char v[8] = {};
      return !(GetEnvironmentVariableA("OPENREMIX_PROXY_WATER_TRANSLUCENT", v, sizeof(v)) && v[0] == '0');
    }();
    // The fog colour is what passes at the measurement distance: a distance
    // at which 5% of light (by luminance) is left at $fogend.
    remixapi_MaterialInfoTranslucentEXT translucent = {};
    translucent.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    translucent.pNext = opaque.pNext;
    translucent.refractiveIndex = 1.333f;
    translucent.transmittanceColor = opaque.albedoConstant;
    {
      const auto &c = opaque.albedoConstant;
      const float luminance = std::clamp(0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z, 1e-4f, 0.95f);
      translucent.transmittanceMeasurementDistance = std::max(fogEnd, 1.0f) * std::log(luminance) / std::log(0.05f);
    }
    // Baking captures the mean optical interface; animated ripple normals
    // remain on the live material and never become frozen texture inputs.
    remixapi_MaterialBakeBoundaryEXT boundary = {};
    boundary.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_BOUNDARY_EXT;
    boundary.pNext = &translucent;
    boundary.smoothBoundary = 1;
    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = translucentWater ? static_cast<void *>(&boundary) : &opaque;
    if (normal)
      info.normalTexture = normalPath;
    info.hash = XXH3_64bits(key.data(), key.size()) | 1;
    info.filterMode = 1;
    info.wrapModeU = info.wrapModeV = 1;
    if (g_world.api && g_world.api->CreateMaterial(&info, &m.ref.handle) == REMIXAPI_ERROR_CODE_SUCCESS) {
      m.resolved = m.owned = true;
      ++g_world.waterMaterials;
    }
    m.waterBody = true;
    // Both sides: a map's water has a downward face only in places (seen from
    // below Source draws the upward one with its $bottommaterial). One-sided,
    // light and shadow rays from under the water crossed it where it had one
    // and not elsewhere: sharp-edged dark patches on the bottom. Its downward
    // faces are left out (build): the upward ones serve both sides.
    m.noCull = true;
    std::memcpy(m.waterColor, &translucent.transmittanceColor, sizeof(m.waterColor));
    m.waterDistance = translucent.transmittanceMeasurementDistance;
    slot = int(g_world.materials.size());
    g_world.materials.push_back(std::move(m));
    return slot;
  }
  if (!kv.shader.empty()) {
    auto flag = [&](const char *k) { return std::atoi(kv.values[k].c_str()) != 0; };
    m.alphaTest = flag("$alphatest");
    // Source's EvaluateBlendRequirements excludes texture alpha when alpha
    // testing is enabled. Stock rooftop01a sets both flags and is a cutout,
    // not a blended prop. Independent alpha sources still require blending.
    const float opacity = kv.values.count("$alpha") ? float(std::atof(kv.values["$alpha"].c_str())) : 1.f;
    const int detailMode = std::atoi(kv.values["$detailblendmode"].c_str());
    const bool detailAlpha = kv.values.count("$detail") &&
                             (detailMode == 3 || detailMode == 8 || detailMode == 9);
    m.blended = opacity < 1 || (flag("$translucent") && !m.alphaTest);
    m.opacity = std::clamp(opacity, 0.f, 1.f);
    if (flag("$additive") || flag("$vertexalpha") || !std::isfinite(opacity) ||
        detailAlpha || (brush && m.blended)) {
      ++g_world.skippedTranslucent;
      return slot;
    }
    m.noCull = flag("$nocull");
    m.pointSample = flag("$pointsample") || flag("$point");
    if (kv.values.count("$alphatestreference")) {
      const float r = float(std::atof(kv.values["$alphatestreference"].c_str()));
      if (r > 0)
        m.alphaRef = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255 + 0.5f);
    }
    if (kv.values.count("$basetexture")) {
      m.baseTexture = lower(kv.values["$basetexture"]);
      m.candidates = textureHashes(m.baseTexture);
      if (!m.pointSample)
        m.pointSample = texturePointSample(m.baseTexture);
    }
    if (emissiveEnabled())
      m.shading = materialShading(kv, m.maskCandidates);
    if (pbr::enabled() && (m.shading.pbr = materialPbr(kv)).active)
      m.shading.base = lower(kv.values["$basetexture"]);
    // A texture is optional for a constant UnlitGeneric material. Lost Coast's
    // waterfog is only $color; waiting for a texture upload can never resolve it
    // and used to hold the entire map's bake in the loading state forever.
    if (kv.shader == "unlitgeneric" && kv.values["$basetexture"].empty() &&
        kv.blocks["proxies"].empty() && !m.blended && !m.alphaTest && !detailAlpha) {
      std::vector<uint64_t> unused;
      const auto shading = materialShading(kv, unused);
      remixapi_MaterialInfoSelfIlluminationEXT self = {};
      self.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
      self.maskScale = 1;
      remixapi_MaterialInfoEmissionEXT emission = {};
      emission.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT;
      emission.pNext = &self;
      emission.suppressLightGeneration = !shading.luminous;
      remixapi_MaterialInfoOpaqueEXT opaque = {};
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.pNext = &emission;
      opaque.albedoConstant = {1, 1, 1};
      opaque.opacityConstant = 1;
      opaque.alphaTestType = 7;
      remixapi_MaterialInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.pNext = &opaque;
      info.hash = XXH3_64bits(cacheKey.data(), cacheKey.size()) | 1;
      info.emissiveIntensity = 1;
      info.emissiveColorConstant = {shading.tint[0], shading.tint[1], shading.tint[2]};
      if (g_world.api && g_world.api->CreateMaterial(&info, &m.ref.handle) == REMIXAPI_ERROR_CODE_SUCCESS)
        m.resolved = m.owned = true;
    }
  }
  for (uint64_t h : m.candidates)
    m.categories |= categories::lookup(h);
  if (m.categories & (REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE | REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN)) {
    if (!brush) {
      ++g_world.skippedIgnored;
      return slot;
    }
    m.categories &= ~uint32_t(REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE | REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN);
    ++g_world.keptIgnored;
  }
  slot = int(g_world.materials.size());
  g_world.materials.push_back(std::move(m));
  return slot;
}

// Whether a world material (by its g_world.materials index) has a second
// layer; `use`: one of its displacements blends it in, so its material and
// its blend modulation are needed too.
SecondLayer secondLayer(int base, bool use) {
  auto found = g_world.secondLayers.find(base);
  if (found == g_world.secondLayers.end()) {
    SecondLayer out;
    KeyValues kv;
    bool world = false;
    if (loadMaterial(g_world.materials[base].name, kv))
      // LightmappedGeneric takes a $basetexture2 too, but does not blend it by
      // the displacement's alpha: gm_flatgrass paints alpha on FLATGRASS
      // (LightmappedGeneric) plates and the game shows the first texture there.
      for (const char *shader : {"worldvertextransition", "lightmapped_4wayblend"})
        world |= kv.shader.rfind(shader, 0) == 0;
    // OPENREMIX_PROXY_BSP_LAYERS=0: the first texture alone, as before.
    static const bool enabled = [] {
      char v[8] = {};
      return !(GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_LAYERS", v, sizeof(v)) && v[0] == '0');
    }();
    out.present = enabled && world && kv.values.count("$basetexture2") && !kv.values["$basetexture2"].empty();
    found = g_world.secondLayers.emplace(base, out).first;
  }
  if (!use || !found->second.present || found->second.prepared)
    return found->second;
  SecondLayer out = found->second;
  out.prepared = true;
  const std::string name = g_world.materials[base].name;
  KeyValues kv;
  loadMaterial(name, kv);
  auto &stats = g_world.layers;
  ++stats.materials;
  // Layers 3 and 4 of a 4-way blend need the multiblend lump (not written
  // for these maps): its second layer by alpha, as the shader falls back to.
  stats.fourWay += kv.shader.rfind("lightmapped_4wayblend", 0) == 0;
  Material m;
  m.name = name + " ($basetexture2 " + lower(kv.values["$basetexture2"]) + ")";
  m.baseTexture = lower(kv.values["$basetexture2"]);
  m.candidates = textureHashes(m.baseTexture);
  for (uint64_t h : m.candidates)
    m.categories |= categories::lookup(h);
  m.categories &= ~uint32_t(REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE | REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN);
  m.noCull = g_world.materials[base].noCull;
  m.pointSample = g_world.materials[base].pointSample || texturePointSample(kv.values["$basetexture2"]);
  if (m.candidates.empty()) {
    ++stats.noHash;
  } else {
    out.material = int(g_world.materials.size());
    g_world.materials.push_back(std::move(m));
  }
  if (kv.values.count("$blendmodulatetexture")) {
    float mean[4];
    if (textureMean(kv.values["$blendmodulatetexture"], mean)) {
      out.modulate = true;
      out.masked = std::atoi(kv.values["$maskedblending"].c_str()) != 0;
      out.centre = mean[1];
      out.halfWidth = mean[0];
      ++stats.modulated;
    } else {
      ++stats.modulateUnread;
    }
  }
  if (stats.names.size() < 700) {
    char line[400];
    std::snprintf(line, sizeof(line), "      %s (%s): $basetexture2 %s%s", name.c_str(), kv.shader.c_str(),
                  lower(kv.values["$basetexture2"]).c_str(), out.material < 0 ? " (no texture hash: not layered)" : "");
    stats.names += line;
    if (out.modulate) {
      std::snprintf(line, sizeof(line), ", $blendmodulatetexture mean centre %.2f half width %.2f%s", out.centre,
                    out.halfWidth, out.masked ? " ($maskedblending)" : "");
      stats.names += line;
    } else if (kv.values.count("$blendmodulatetexture")) {
      stats.names += ", $blendmodulatetexture not read (plain alpha)";
    }
    stats.names += "\n";
  }
  g_world.secondLayers[base] = out;
  return out;
}

proxy::cubemaps::Watcher g_cubemapOverrides;

constexpr uint64_t kBatchId = 0x42535057; // "BSPW"

// The runtime's scene instances (in process: resolved from remix.dll).
template <typename Fn> Fn sceneFunction(const char *name) {
  const HMODULE runtime = GetModuleHandleW(L"remix.dll");
  return runtime ? reinterpret_cast<Fn>(GetProcAddress(runtime, name)) : nullptr;
}

void invalidateBakeScene() {
  if (auto publish = sceneFunction<PFN_remixapi_SetBakeSceneEXT>("remixapi_SetBakeSceneEXT")) {
    if (g_world.map.empty()) {
      publish(nullptr);
    } else {
      remixapi_BakeSceneInfoEXT info = {};
      info.structSize = sizeof(info);
      info.version = REMIXAPI_BAKE_SCENE_VERSION_EXT;
      info.sceneId = XXH3_64bits(g_world.map.data(), g_world.map.size()) | 1;
      info.revision = g_world.submitVersion + 1;
      info.sceneScale = .0254f;
      info.state = REMIXAPI_BAKE_SCENE_LOADING_EXT;
      publish(&info);
    }
  }
  g_world.bakeVersion = ~0ull;
  g_world.bakeStatus = -1;
  g_world.bakeReason = "waiting for retained map inputs";
}

// Removes the map's scene instances; before their meshes go.
void dropSceneInstances() {
  if (g_world.bakeVersion != ~0ull)
    invalidateBakeScene();
  static const auto remove = sceneFunction<PFN_remixapi_RemoveSceneInstancesEXT>("remixapi_RemoveSceneInstancesEXT");
  if (remove && !g_world.sceneInstances.empty())
    remove(g_world.sceneInstances.data(), uint32_t(g_world.sceneInstances.size()));
  g_world.sceneInstances.clear();
  g_world.bakeInstances.clear();
  g_world.bakeCharts.clear();
  g_world.sceneValid = false;
}

void destroyMeshes() {
  // Only map teardown discards authored bounds. Same-map retained/material
  // refreshes can invalidate diffuse inputs without forgetting the sidecar.
  if (auto publish = sceneFunction<PFN_remixapi_SetReflectionVolumesEXT>("remixapi_SetReflectionVolumesEXT"))
    publish(nullptr);
  dropSceneInstances();
  g_world.sceneVersion = ~0ull;
  if (g_world.batchValid && g_world.api) {
    using BatchFn = remixapi_ErrorCode(REMIXAPI_CALL *)(uint64_t);
    if (auto drop = reinterpret_cast<BatchFn>(
            GetProcAddress(GetModuleHandleW(L"remix_bridge.dll"), "openremix_bridge_DeleteBatch")))
      drop(kBatchId);
  }
  for (auto &m : g_world.materials)
    if (m.owned && m.ref.handle && g_world.api)
      g_world.api->DestroyMaterial(m.ref.handle);
  for (auto &normal : g_world.surfaceNormals)
    if (normal->texture && g_world.api)
      g_world.api->DestroyTexture(normal->texture);
  g_world.surfaceNormals.clear();
  for (auto tint : g_world.opticalTints)
    if (g_world.api)
      g_world.api->DestroyTexture(tint);
  g_world.opticalTints.clear();
  for (auto &[key, group] : g_world.overlayGroups)
    if (group.mesh && g_world.api)
      g_world.api->DestroyMesh(group.mesh);
  for (auto &[key, group] : g_world.layerGroups)
    if (group.mesh && g_world.api)
      g_world.api->DestroyMesh(group.mesh);
  for (auto &[key, model] : g_world.propModels)
    for (auto &part : model.parts)
      if (part.mesh && g_world.api)
        g_world.api->DestroyMesh(part.mesh);
  for (auto &model : g_world.clippedSkyModels)
    for (auto &part : model->parts)
      if (part.mesh && g_world.api)
        g_world.api->DestroyMesh(part.mesh);
  for (auto &[key, group] : g_world.groups)
    if (group.mesh && g_world.api)
      g_world.api->DestroyMesh(group.mesh);
}

// Static props (game lump "sprp", version 9/10 placements of at least 72 bytes).
bool skyTraceEnabled() {
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_SKY_TRACE", v, sizeof(v)) && v[0] == '0');
  }();
  return enabled;
}

// A 3D skybox point in world space (where the main camera sees it).
void skyToWorld(const float in[3], float out[3]) {
  for (int k = 0; k < 3; ++k)
    out[k] = (in[k] - g_world.skyOrigin[k]) * g_world.skyScale;
}

// Whether a point lies in space the player can occupy (an empty leaf of the
// main map): skybox geometry there stands in for the map itself. Skybox
// scenery lands behind the map's sky brushes, in solid or outside space.
bool insidePlayable(const Bsp &bsp, const float p[3]) {
  const int leaf = leafAt(bsp, p);
  return leaf >= 0 && size_t(leaf) < g_world.leafPlayable.size() && g_world.leafPlayable[leaf];
}

// Clips a convex polygon against the map's BSP: the pieces outside playable
// space (solid, or beyond the map) go to `out`. `split` reports any cut or
// drop, so an untouched polygon can keep its original vertices.
struct SkyClipResult { bool split = false, removed = false; };
void clipOutsidePlayable(const Bsp &bsp, int index, std::vector<remixapi_HardcodedVertex> polygon,
                         std::vector<std::vector<remixapi_HardcodedVertex>> &out, SkyClipResult &result, int depth = 0) {
  const auto [nodes, nodeCount] = bsp.lump<Node>(kNodes);
  const auto [planes, planeCount] = bsp.lump<Plane>(kPlanes);
  if (polygon.size() < 3)
    return;
  if (index < 0) {
    const int leaf = -1 - index;
    if (size_t(leaf) < g_world.leafPlayable.size() && g_world.leafPlayable[leaf]) {
      result.split = result.removed = true;
      return;
    }
    out.push_back(std::move(polygon));
    return;
  }
  if (size_t(index) >= nodeCount || size_t(nodes[index].plane) >= planeCount || depth > 256) {
    out.push_back(std::move(polygon));
    return;
  }
  const Plane &plane = planes[nodes[index].plane];
  constexpr float kEpsilon = .01f;
  std::vector<float> d(polygon.size());
  bool front = false, back = false;
  for (size_t i = 0; i < polygon.size(); ++i) {
    d[i] = dot(plane.normal, polygon[i].position) - plane.dist;
    front |= d[i] > kEpsilon;
    back |= d[i] < -kEpsilon;
  }
  if (!back) // on the plane counts as in front, as leafAt does
    return clipOutsidePlayable(bsp, nodes[index].children[0], std::move(polygon), out, result, depth + 1);
  if (!front)
    return clipOutsidePlayable(bsp, nodes[index].children[1], std::move(polygon), out, result, depth + 1);
  result.split = true;
  auto lerp = [](const remixapi_HardcodedVertex &a, const remixapi_HardcodedVertex &b, float t) {
    remixapi_HardcodedVertex v = a;
    for (int k = 0; k < 3; ++k) {
      v.position[k] = a.position[k] + (b.position[k] - a.position[k]) * t;
      v.normal[k] = a.normal[k] + (b.normal[k] - a.normal[k]) * t;
    }
    for (int k = 0; k < 2; ++k)
      v.texcoord[k] = a.texcoord[k] + (b.texcoord[k] - a.texcoord[k]) * t;
    v.color = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      const float ca = float((a.color >> shift) & 255), cb = float((b.color >> shift) & 255);
      v.color |= uint32_t(std::lround(ca + (cb - ca) * t)) << shift;
    }
    const float length = std::sqrt(dot(v.normal, v.normal));
    if (length > 1e-6f)
      for (float &n : v.normal)
        n /= length;
    return v;
  };
  std::vector<remixapi_HardcodedVertex> frontPart, backPart;
  for (size_t i = 0; i < polygon.size(); ++i) {
    const size_t j = (i + 1) % polygon.size();
    if (d[i] >= 0)
      frontPart.push_back(polygon[i]);
    if (d[i] < 0)
      backPart.push_back(polygon[i]);
    if ((d[i] >= 0) != (d[j] >= 0)) {
      const auto v = lerp(polygon[i], polygon[j], d[i] / (d[i] - d[j]));
      frontPart.push_back(v);
      backPart.push_back(v);
    }
  }
  clipOutsidePlayable(bsp, nodes[index].children[0], std::move(frontPart), out, result, depth + 1);
  clipOutsidePlayable(bsp, nodes[index].children[1], std::move(backPart), out, result, depth + 1);
}

void noteSkyReach(const float p[3]) {
  float d2 = 0;
  for (int k = 0; k < 3; ++k) {
    const float c = .5f * (g_world.playableMins[k] + g_world.playableMaxs[k]);
    d2 += (p[k] - c) * (p[k] - c);
  }
  g_world.skyReach = std::max(g_world.skyReach, std::sqrt(d2));
}

// Shared original meshes remain available to placements which need no cut.
// A cut placement keeps its model transform and replacement hash; only its base
// mesh changes, so material/mesh replacement lookup retains the original identity.
const PropModel *clipSkyProp(const Bsp &bsp, const PropModel &source, const float (&transform)[3][4]) {
  std::vector<sky::ClippedMesh> parts;
  bool changed = false;
  for (const auto &part : source.parts) {
    parts.push_back(sky::clipPlacedMesh(part.loadVertices, part.indices, transform,
        [&](sky::Polygon triangle, std::vector<sky::Polygon> &pieces) {
          SkyClipResult result;
          clipOutsidePlayable(bsp, 0, std::move(triangle), pieces, result);
          return result.removed;
        }));
    changed |= parts.back().changed;
  }
  if (!changed) return &source;
  auto model = std::make_unique<PropModel>();
  model->engineParts = source.engineParts;
  model->missingBakeOccluders = source.missingBakeOccluders;
  model->additiveEngineParts = source.additiveEngineParts;
  model->engineMaterials = source.engineMaterials;
  for (size_t i = 0; i < parts.size(); ++i) {
    auto &geometry = parts[i];
    if (geometry.indices.empty()) continue;
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = geometry.vertices.data();
    surface.vertices_count = geometry.vertices.size();
    surface.indices_values = geometry.indices.data();
    surface.indices_count = geometry.indices.size();
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    auto hash = XXH3_64bits_withSeed(geometry.vertices.data(), geometry.vertices.size() * sizeof(sky::Vertex),
                                  source.parts[i].replacementHash);
    info.hash = XXH3_64bits_withSeed(geometry.indices.data(), geometry.indices.size() * sizeof(uint32_t), hash) | 1;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    PropPart part;
    part.material = source.parts[i].material;
    part.replacementHash = source.parts[i].replacementHash;
    if (g_world.api->CreateMesh(&info, &part.mesh) != REMIXAPI_ERROR_CODE_SUCCESS) {
      for (auto &made : model->parts) g_world.api->DestroyMesh(made.mesh);
      ++g_world.propModelsFailed; // refuse an incomplete map bake; engine retains fallback ownership
      std::fprintf(stderr, "Clipped sky prop mesh upload failed\n");
      return nullptr;
    }
    for (const auto &vertex : geometry.vertices)
      part.positions.insert(part.positions.end(), std::begin(vertex.position), std::end(vertex.position));
    part.indices = std::move(geometry.indices);
    model->parts.push_back(std::move(part));
  }
  if (model->parts.empty()) return nullptr;
  const auto *result = model.get();
  g_world.clippedSkyModels.push_back(std::move(model));
  return result;
}

template <typename InSky> void loadProps(const Bsp &bsp, const InSky &inSky) {
  // OPENREMIX_PROXY_BSP_PROPS=0 leaves static props to the engine.
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_PROPS", v, sizeof(v)) && v[0] == '0');
  }();
  if (!enabled)
    return;
  const auto &file = bsp.data;
  const auto [directory, bytes] = bsp.lump<uint8_t>(kGameLump);
  if (!directory || bytes < 4)
    return;
  int32_t count;
  std::memcpy(&count, directory, 4);
  size_t start = 0, length = 0;
  uint16_t version = 0;
  bool compressedProps = false;
  for (int32_t i = 0; i < count && size_t(4 + (i + 1) * 16) <= bytes; ++i) {
    int32_t id, offset, size;
    uint16_t flags;
    std::memcpy(&id, directory + 4 + i * 16, 4);
    std::memcpy(&flags, directory + 8 + i * 16, 2);
    std::memcpy(&version, directory + 10 + i * 16, 2);
    std::memcpy(&offset, directory + 12 + i * 16, 4);
    std::memcpy(&size, directory + 16 + i * 16, 4);
    // Flag 1: compressed (an LZMA blob there; size is the decoded size).
    if (id == 0x73707270 && offset > 0 && size > 0 && size_t(offset) < file.size() &&
        ((flags & 1) || size_t(offset) + size <= file.size())) {
      start = size_t(offset);
      length = size_t(size);
      compressedProps = (flags & 1) != 0;
      break;
    }
  }
  // sprp v4 (HL2-era) .. v12: origin 0, angles 12, model 24, skin 32 in all.
  if (!length || version < 4)
    return;
  std::vector<uint8_t> lump;
  if (compressedProps) {
    if (!lzma::decodeSource(file.data() + start, file.size() - start, lump))
      return;
  } else {
    lump.assign(file.begin() + start, file.begin() + start + length);
  }
  const int32_t names = i32(lump, 0);
  size_t at = 4 + size_t(std::max(names, 0)) * 128;
  const int32_t leaves = i32(lump, at);
  at += 4 + size_t(std::max(leaves, 0)) * 2;
  const int32_t props = i32(lump, at);
  at += 4;
  if (names <= 0 || props <= 0 || at > lump.size())
    return;
  const size_t stride = (lump.size() - at) / size_t(props);
  if (stride < 56) // v4 records are 56 bytes (GMod's gm_construct: v6, 64)
    return;
  for (int32_t p = 0; p < props; ++p) {
    const size_t rec = at + size_t(p) * stride;
    float origin[3], angles[3];
    uint16_t modelIndex = 0;
    int32_t skin = 0;
    if (!get(lump, rec, origin) || !get(lump, rec + 12, angles) || !get(lump, rec + 24, modelIndex) ||
        !get(lump, rec + 32, skin) || modelIndex >= names)
      continue;
    const std::string path = lower(cstring(lump, 4 + size_t(modelIndex) * 128));
    auto [it, fresh] = g_world.propModels.try_emplace({path, skin});
    if (fresh && !loadPropModel(path, skin, it->second)) {
      ++g_world.propModelsFailed;
      g_world.failedModels.push_back(path);
    }
    if (it->second.parts.empty())
      continue;
    // 3D skybox props stay with the engine: they are seen only through the
    // sky camera, which the engine's sky view already draws in full.
    Prop prop;
    angleMatrix(angles, origin, prop.transform);
    if (inSky(origin)) {
      ++g_world.skyProps;
      if (!g_world.skyTraced)
        continue;
      float moved[3];
      skyToWorld(origin, moved);
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
          prop.transform[r][c] *= g_world.skyScale;
        prop.transform[r][3] = moved[r];
      }
      try {
        prop.model = clipSkyProp(bsp, it->second, prop.transform);
      } catch (const std::exception &error) {
        ++g_world.propModelsFailed;
        std::fprintf(stderr, "Sky prop clipping refused %s: %s\n", path.c_str(), error.what());
        continue;
      }
      if (!prop.model) {
        ++g_world.skyStandInProps;
        continue;
      }
      noteSkyReach(moved);
      ++g_world.skyTracedProps;
      prop.skyTraced = true;
    } else prop.model = &it->second;
    prop.id = XXH3_64bits_withSeed(&p, sizeof(p), 0x70726f70ull) | 1;
    g_world.propCells[propCell(origin)].push_back(uint32_t(g_world.props.size()));
    g_world.props.push_back(prop);
  }
  // The API copied vertices at CreateMesh. Retain only the existing compact
  // position/index occluders; full attributes were needed solely during loading.
  for (auto &[key, model] : g_world.propModels)
    for (auto &part : model.parts)
      std::vector<remixapi_HardcodedVertex>().swap(part.loadVertices);
}

// A face's triangles in world space (displacements tessellated).
std::vector<std::array<std::array<float, 3>, 3>> faceTriangles(const Bsp &bsp, int32_t f) {
  const auto [vertexes, vertexCount] = bsp.lump<std::array<float, 3>>(kVertexes);
  const auto [edges, edgeCount] = bsp.lump<std::array<uint16_t, 2>>(kEdges);
  const auto [surfEdges, surfEdgeCount] = bsp.lump<int32_t>(kSurfEdges);
  const auto [faces, faceCount] = bsp.lump<Face>(kFaces);
  const auto [dispInfos, dispInfoCount] = bsp.lump<DispInfo>(kDispInfo);
  const auto [dispVerts, dispVertCount] = bsp.lump<DispVert>(kDispVerts);
  std::vector<std::array<std::array<float, 3>, 3>> out;
  if (f < 0 || size_t(f) >= faceCount)
    return out;
  const Face &face = faces[f];
  std::vector<std::array<float, 3>> corners;
  for (int e = 0; e < face.numEdges; ++e) {
    const size_t se = size_t(face.firstEdge + e);
    if (se >= surfEdgeCount)
      return out;
    const int32_t edge = surfEdges[se];
    const size_t ei = size_t(edge < 0 ? -edge : edge);
    if (ei >= edgeCount || edges[ei][edge < 0 ? 1 : 0] >= vertexCount)
      return out;
    corners.push_back(vertexes[edges[ei][edge < 0 ? 1 : 0]]);
  }
  if (face.dispInfo < 0) {
    for (size_t k = 1; k + 1 < corners.size(); ++k)
      out.push_back({corners[0], corners[k], corners[k + 1]});
    return out;
  }
  if (!dispInfos || size_t(face.dispInfo) >= dispInfoCount || corners.size() != 4)
    return out;
  const DispInfo &di = dispInfos[face.dispInfo];
  int first = 0;
  float best = 1e30f;
  for (int c = 0; c < 4; ++c) {
    float d2 = 0;
    for (int k = 0; k < 3; ++k)
      d2 += (corners[c][k] - di.start[k]) * (corners[c][k] - di.start[k]);
    if (d2 < best) {
      best = d2;
      first = c;
    }
  }
  const int size = (1 << di.power) + 1;
  if (di.power < 2 || di.power > 4 || di.firstVert < 0 || size_t(di.firstVert + size * size) > dispVertCount)
    return out;
  std::vector<std::array<float, 3>> grid(size_t(size) * size);
  for (int v = 0; v < size; ++v)
    for (int u = 0; u < size; ++u) {
      const float t = float(v) / float(size - 1), s = float(u) / float(size - 1);
      const DispVert &dv = dispVerts[di.firstVert + v * size + u];
      for (int k = 0; k < 3; ++k) {
        const float left = corners[first][k] + (corners[(first + 1) % 4][k] - corners[first][k]) * t;
        const float right = corners[(first + 3) % 4][k] + (corners[(first + 2) % 4][k] - corners[(first + 3) % 4][k]) * t;
        grid[size_t(v) * size + u][k] = left + (right - left) * s + dv.vector[k] * dv.dist;
      }
    }
  for (int v = 0; v + 1 < size; ++v)
    for (int u = 0; u + 1 < size; ++u) {
      const auto &a = grid[size_t(v) * size + u], &b = grid[size_t(v) * size + u + 1],
                 &c = grid[size_t(v + 1) * size + u], &d = grid[size_t(v + 1) * size + u + 1];
      out.push_back({a, c, d});
      out.push_back({a, d, b});
    }
  return out;
}

// Overlay materials (usually translucent decals), by name.
int overlayMaterial(const std::string &name, bool &modulate) {
  const std::string key = lower(name);
  if (auto it = g_world.overlayMaterialIndex.find(key); it != g_world.overlayMaterialIndex.end()) {
    modulate = it->second < 0 ? false : g_world.materials[it->second].modulate;
    return it->second;
  }
  int &slot = g_world.overlayMaterialIndex[key];
  slot = -1;
  Material m;
  m.name = name;
  KeyValues kv;
  if (!loadMaterial(m.name, kv) || !kv.values.count("$basetexture"))
    return slot;
  m.modulate = kv.shader == "decalmodulate";
  m.noCull = std::atoi(kv.values["$nocull"].c_str()) != 0;
  m.pointSample = (std::atoi(kv.values["$pointsample"].c_str()) != 0 ||
                   std::atoi(kv.values["$point"].c_str()) != 0) ||
                  texturePointSample(kv.values["$basetexture"]);
  m.baseTexture = lower(kv.values["$basetexture"]);
  m.candidates = textureHashes(m.baseTexture);
  for (uint64_t h : m.candidates)
    m.categories |= categories::lookup(h);
  if (m.categories & (REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE | REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN))
    return slot;
  modulate = m.modulate;
  slot = int(g_world.materials.size());
  g_world.materials.push_back(std::move(m));
  return slot;
}

// info_overlay: the overlay's quad (in its plane's U/V basis) clipped against
// each listed face's triangles seen along the overlay normal, then placed on
// the triangle by its barycentric coordinates and lifted off it like the
// proxy's other decals.
template <typename InSky> void loadOverlays(const Bsp &bsp, const InSky &inSky) {
  const auto [overlays, count] = bsp.lump<Overlay>(kOverlays);
  const auto [texInfos, texInfoCount] = bsp.lump<TexInfo>(kTexInfo);
  const auto [texDatas, texDataCount] = bsp.lump<TexData>(kTexData);
  const auto [stringTable, stringTableCount] = bsp.lump<int32_t>(kTexDataStringTable);
  const auto [strings, stringsLength] = bsp.lump<char>(kTexDataStringData);
  if (!overlays || bsp.lumpVersion(kOverlays) != 0)
    return;
  constexpr float kOffset = 0.1f, kCell = 2048;
  struct Point {
    float p[2], uv[2];
  };
  for (size_t o = 0; o < count; ++o) {
    const Overlay &ov = overlays[o];
    if (ov.texInfo < 0 || size_t(ov.texInfo) >= texInfoCount)
      continue;
    const int32_t td = texInfos[ov.texInfo].texData;
    if (td < 0 || size_t(td) >= texDataCount)
      continue;
    const int32_t nameId = texDatas[td].nameId;
    if (nameId < 0 || size_t(nameId) >= stringTableCount || size_t(stringTable[nameId]) >= stringsLength)
      continue;
    const char *name = strings + stringTable[nameId];
    bool modulate = false;
    const int material = overlayMaterial(std::string(name, strnlen(name, stringsLength - stringTable[nameId])), modulate);
    if (material < 0)
      continue;
    // Basis: N, U (packed into the corners' z), V = N x U.
    float n[3], u[3], v[3];
    std::memcpy(n, ov.normal, 12);
    for (int k = 0; k < 3; ++k)
      u[k] = ov.uvPoints[k][2];
    v[0] = n[1] * u[2] - n[2] * u[1];
    v[1] = n[2] * u[0] - n[0] * u[2];
    v[2] = n[0] * u[1] - n[1] * u[0];
    const float quadUv[4][2] = {{ov.u[0], ov.v[0]}, {ov.u[0], ov.v[1]}, {ov.u[1], ov.v[1]}, {ov.u[1], ov.v[0]}};
    std::vector<Point> quad(4);
    for (int c = 0; c < 4; ++c)
      quad[c] = {{ov.uvPoints[c][0], ov.uvPoints[c][1]}, {quadUv[c][0], quadUv[c][1]}};
    // 3D skybox overlays stay with the engine, like its faces.
    if (inSky(ov.origin))
      continue;
    const bool sky = false, noCull = g_world.materials[material].noCull;
    std::array<int32_t, 5> key = {int32_t(std::floor(ov.origin[0] / kCell)), int32_t(std::floor(ov.origin[1] / kCell)),
                                  int32_t(std::floor(ov.origin[2] / kCell)), modulate, int32_t(sky) | (noCull ? 4 : 0)};
    auto &group = g_world.overlayGroups[key];
    group.modulate = modulate;
    group.sky = sky;
    group.noCull = noCull;
    group.id = XXH3_64bits_withSeed(key.data(), sizeof(key), 0x6f766c79ull) | 1;
    Geometry &g = group.surfaces[uint32_t(material)];
    const uint32_t faceCount = ov.faceCountAndRenderOrder & 0x3fff;
    for (uint32_t fi = 0; fi < faceCount && fi < 64; ++fi) {
      for (const auto &tri : faceTriangles(bsp, ov.faces[fi])) {
        float t2[3][2];
        for (int k = 0; k < 3; ++k) {
          const float rel[3] = {tri[k][0] - ov.origin[0], tri[k][1] - ov.origin[1], tri[k][2] - ov.origin[2]};
          t2[k][0] = dot(rel, u);
          t2[k][1] = dot(rel, v);
        }
        const float area = (t2[1][0] - t2[0][0]) * (t2[2][1] - t2[0][1]) - (t2[2][0] - t2[0][0]) * (t2[1][1] - t2[0][1]);
        if (std::fabs(area) < 1e-4f)
          continue; // edge-on to the overlay
        // Clip the quad against the triangle's edges (Sutherland-Hodgman).
        std::vector<Point> poly = quad;
        for (int e = 0; e < 3 && poly.size() >= 3; ++e) {
          const float *a = t2[e], *b = t2[(e + 1) % 3];
          auto side = [&](const Point &p) {
            return ((b[0] - a[0]) * (p.p[1] - a[1]) - (b[1] - a[1]) * (p.p[0] - a[0])) * (area > 0 ? 1.0f : -1.0f);
          };
          std::vector<Point> next;
          for (size_t i = 0; i < poly.size(); ++i) {
            const Point &p = poly[i], &q = poly[(i + 1) % poly.size()];
            const float sp = side(p), sq = side(q);
            if (sp >= 0)
              next.push_back(p);
            if ((sp >= 0) != (sq >= 0)) {
              const float t = sp / (sp - sq);
              next.push_back({{p.p[0] + (q.p[0] - p.p[0]) * t, p.p[1] + (q.p[1] - p.p[1]) * t},
                              {p.uv[0] + (q.uv[0] - p.uv[0]) * t, p.uv[1] + (q.uv[1] - p.uv[1]) * t}});
            }
          }
          poly = std::move(next);
        }
        if (poly.size() < 3)
          continue;
        const float e1[3] = {tri[1][0] - tri[0][0], tri[1][1] - tri[0][1], tri[1][2] - tri[0][2]};
        const float e2[3] = {tri[2][0] - tri[0][0], tri[2][1] - tri[0][1], tri[2][2] - tri[0][2]};
        float tn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float len = std::sqrt(dot(tn, tn));
        if (len < 1e-8f)
          continue;
        // The face's triangles wind clockwise seen from its front (faceTriangles
        // keeps the map's order): -tn is the face's front. The overlay lies on
        // the side its own normal names, as before; it is wound so that this
        // side is its front, like the face under it.
        const bool behind = dot(tn, n) > 0;
        g_world.cull.overlayBehind += behind;
        for (float &x : tn)
          x /= dot(tn, n) < 0 ? -len : len;
        float polyArea = 0;
        for (size_t i = 0; i < poly.size(); ++i) {
          const Point &p = poly[i], &q = poly[(i + 1) % poly.size()];
          polyArea += p.p[0] * q.p[1] - q.p[0] * p.p[1];
        }
        // Wound like the triangle it is mapped onto (clockwise seen from the
        // face's front) unless the overlay faces the other way.
        const bool sameAsFace = (polyArea > 0) == (area > 0);
        const bool rewind = sameAsFace == behind;
        ++(rewind ? g_world.cull.overlayRewound : g_world.cull.overlayKept);
        const uint32_t base = uint32_t(g.vertices.size());
        for (const Point &p : poly) {
          // Barycentric coordinates in the overlay plane.
          const float w1 = ((p.p[0] - t2[0][0]) * (t2[2][1] - t2[0][1]) - (t2[2][0] - t2[0][0]) * (p.p[1] - t2[0][1])) / area;
          const float w2 = ((t2[1][0] - t2[0][0]) * (p.p[1] - t2[0][1]) - (p.p[0] - t2[0][0]) * (t2[1][1] - t2[0][1])) / area;
          remixapi_HardcodedVertex vtx = {};
          for (int k = 0; k < 3; ++k)
            vtx.position[k] = tri[0][k] + e1[k] * w1 + e2[k] * w2 + tn[k] * kOffset;
          std::memcpy(vtx.normal, tn, 12);
          vtx.texcoord[0] = p.uv[0];
          vtx.texcoord[1] = p.uv[1];
          vtx.color = 0xffffffff;
          g.vertices.push_back(vtx);
        }
        for (uint32_t k = 1; k + 1 < poly.size(); ++k) {
          if (rewind)
            g.indices.insert(g.indices.end(), {base, base + k + 1, base + k});
          else
            g.indices.insert(g.indices.end(), {base, base + k, base + k + 1});
        }
        g_world.overlayTriangles += uint32_t(poly.size() - 2);
      }
    }
    ++g_world.overlays;
  }
  for (auto it = g_world.overlayGroups.begin(); it != g_world.overlayGroups.end();) {
    for (auto s = it->second.surfaces.begin(); s != it->second.surfaces.end();)
      s = s->second.indices.empty() ? it->second.surfaces.erase(s) : std::next(s);
    it = it->second.surfaces.empty() ? g_world.overlayGroups.erase(it) : std::next(it);
  }
}

// dworldlight_t, lump version 0 (88 bytes; HL2/GMod maps) and version 1
// (L4D2: shadow_cast_offset, 100 bytes).
struct WorldLightV0 {
  float origin[3], intensity[3], normal[3];
  int32_t cluster, type, style;
  float stopdot, stopdot2, exponent, radius, constantAttn, linearAttn, quadraticAttn;
  int32_t flags, texinfo, owner;
};
static_assert(sizeof(WorldLightV0) == 88);
struct WorldLight {
  float origin[3], intensity[3], normal[3], shadowCastOffset[3];
  int32_t cluster, type, style;
  float stopdot, stopdot2, exponent, radius, constantAttn, linearAttn, quadraticAttn;
  int32_t flags, texinfo, owner;
};
static_assert(sizeof(WorldLight) == 100);
enum { kEmitSurface = 0, kEmitPoint = 1, kEmitSpot = 2, kEmitSky = 3 };

// The map's static lights (HDR worldlights; LDR when the map has no HDR
// lighting). Lights with a light style (switchable, flickering) keep it: the
// renderer scales them by the engine's lightstyle value each frame (without
// engine_lights they stay with the engine's per-model light state, as lights
// in the 3D skybox do).
// OPENREMIX_PROXY_BSP_LIGHTS=0 leaves all lights to the engine.
template <typename InSky> void loadLights(const Bsp &bsp, const InSky &inSky) {
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_LIGHTS", v, sizeof(v)) && v[0] == '0');
  }();
  if (!enabled)
    return;
  int index = kWorldLightsHdr;
  if (!bsp.lump<uint8_t>(index).second)
    index = kWorldLights;
  std::vector<WorldLight> lights;
  if (bsp.lumpVersion(index) == 1) {
    const auto [v1, n] = bsp.lump<WorldLight>(index);
    if (v1)
      lights.assign(v1, v1 + n);
  } else if (bsp.lumpVersion(index) == 0) {
    const auto [v0, n] = bsp.lump<WorldLightV0>(index);
    for (size_t i = 0; v0 && i < n; ++i) {
      WorldLight w = {};
      std::memcpy(w.origin, v0[i].origin, 36);
      std::memcpy(&w.cluster, &v0[i].cluster, sizeof(WorldLightV0) - 36);
      lights.push_back(w);
    }
  }
  const size_t count = lights.size();
  for (size_t i = 0; i < count; ++i) {
    const WorldLight &w = lights[i];
    if (w.type != kEmitSurface && w.type != kEmitPoint && w.type != kEmitSpot && w.type != kEmitSky) {
      ++g_world.otherLights; // Quake and sky ambient lights
      continue;
    }
    g_world.surfaceLights += w.type == kEmitSurface;
    // The sun keeps its light (GMod's light_environment often has a style,
    // e.g. 33 on gm_construct, for day/night control).
    const bool styled = w.style != 0 && w.type != kEmitSky;
    if (styled && !g_world.lightStylesAvailable) {
      ++g_world.styledLights;
      continue;
    }
    if (w.type != kEmitSky && inSky(w.origin)) {
      ++g_world.skyboxLights;
      continue;
    }
    // As the engine passes it to models (WorldLightToMaterialLight).
    Light l;
    // Retain texture-light sidedness instead of the isotropic point-light
    // approximation that Source passes to model lighting.
    l.type = w.type == kEmitSurface ? lighting::kSurface : w.type == kEmitPoint ? 1 : w.type == kEmitSpot ? 3 : 2;
    std::memcpy(l.color, w.intensity, 12);
    std::memcpy(l.position, w.origin, 12);
    std::memcpy(l.direction, w.normal, 12);
    l.attenuation[0] = w.constantAttn;
    l.attenuation[1] = w.linearAttn;
    l.attenuation[2] = w.quadraticAttn;
    if (w.type == kEmitSurface)
      l.attenuation[0] = l.attenuation[1] = 0, l.attenuation[2] = 1;
    l.thetaDot = w.stopdot;
    l.phiDot = w.stopdot2;
    // By index and contents: the index alone named the same light on every
    // map, so after a map change the renderer found the previous map's light
    // cached under that name and went on drawing it in place of the new one.
    l.hash = XXH3_64bits_withSeed(&w, sizeof(w), 0x6c696768745f6273ull ^ (uint64_t(i) * 0x9e3779b97f4a7c15ull)) | 1;
    if (styled) {
      l.style = w.style;
      ++g_world.styleDrivenLights;
    }
    g_world.lights.push_back(l);
  }
}

// Leafs as lump version 1 records: version 0 (HL2-era v19 maps) adds a
// 24-byte ambient cube per leaf after the same first 30 bytes.
std::vector<Leaf> leafsOf(const Bsp &bsp) {
  std::vector<Leaf> out;
  const int32_t version = bsp.lumpVersion(kLeafs);
  const size_t stride = version == 0 ? 56 : version == 1 ? sizeof(Leaf) : 0;
  const auto [bytes, size] = bsp.lump<uint8_t>(kLeafs);
  if (!stride || !bytes || size % stride)
    return out;
  out.resize(size / stride);
  for (size_t i = 0; i < out.size(); ++i)
    std::memcpy(&out[i], bytes + i * stride, std::min<size_t>(30, sizeof(Leaf)));
  return out;
}

bool build(const Bsp &bsp) {
  const auto [planes, planeCount] = bsp.lump<Plane>(kPlanes);
  const auto [vertexes, vertexCount] = bsp.lump<std::array<float, 3>>(kVertexes);
  const auto [edges, edgeCount] = bsp.lump<std::array<uint16_t, 2>>(kEdges);
  const auto [surfEdges, surfEdgeCount] = bsp.lump<int32_t>(kSurfEdges);
  const auto [texInfos, texInfoCount] = bsp.lump<TexInfo>(kTexInfo);
  const auto [texDatas, texDataCount] = bsp.lump<TexData>(kTexData);
  const auto [stringTable, stringTableCount] = bsp.lump<int32_t>(kTexDataStringTable);
  const auto [strings, stringsLength] = bsp.lump<char>(kTexDataStringData);
  const auto [faces, faceCount] = bsp.lump<Face>(kFaces);
  const auto [models, modelCount] = bsp.lump<Model>(kModels);
  const std::vector<Leaf> leafStore = leafsOf(bsp);
  const Leaf *leafs = leafStore.empty() ? nullptr : leafStore.data();
  const size_t leafCount = leafStore.size();
  const auto [dispInfos, dispInfoCount] = bsp.lump<DispInfo>(kDispInfo);
  const auto [dispVerts, dispVertCount] = bsp.lump<DispVert>(kDispVerts);
  if (!planes || !vertexes || !edges || !surfEdges || !texInfos || !texDatas || !stringTable || !strings || !faces ||
      !models || !modelCount || bsp.lumpVersion(kFaces) > 1 || !leafs)
    return false;

  // The 3D skybox is a sealed room: its own BSP area (Leaf::areaFlags low 9
  // bits). Faces referenced by leaves of the sky camera's area, and points
  // inside them, belong to it. (The PVS of the camera's cluster is not
  // enough: skybox geometry spans clusters that cluster does not list.)
  parseMapCameras(bsp);
  int skyArea = -1;
  if (g_world.skyCamera && leafs) {
    const int leaf = leafAt(bsp, g_world.skyOrigin);
    if (leaf >= 0 && size_t(leaf) < leafCount)
      skyArea = leafs[leaf].areaFlags & 0x1ff;
  }
  g_world.skyArea = skyArea;
  g_world.skyTraced = skyArea >= 0 && skyTraceEnabled() && g_world.skyScale > 0;
  {
    // Bake bounds are needed on every map, including maps without a 3D skybox
    // and runs with sky tracing disabled. Keep the separate sky area excluded.
    bool any = false;
    for (size_t l = 1; l < leafCount; ++l) {
      const Leaf &leaf = leafs[l];
      if ((leaf.contents & 1) || leaf.cluster < 0 || (leaf.areaFlags & 0x1ff) == skyArea)
        continue;
      for (int k = 0; k < 3; ++k) {
        g_world.playableMins[k] = any ? std::min(g_world.playableMins[k], float(leaf.mins[k])) : float(leaf.mins[k]);
        g_world.playableMaxs[k] = any ? std::max(g_world.playableMaxs[k], float(leaf.maxs[k])) : float(leaf.maxs[k]);
      }
      any = true;
    }
    g_world.skyTraced &= any;
  }
  {
    const auto [nodes, nodeCount] = bsp.lump<Node>(kNodes);
    if (nodes)
      g_world.nodes.assign(nodes, nodes + nodeCount);
    g_world.planes.assign(planes, planes + planeCount);
    for (size_t l = 0; leafs && l < leafCount; ++l) {
      g_world.leafAreas.push_back(int16_t(leafs[l].areaFlags & 0x1ff));
      g_world.leafPlayable.push_back(l > 0 && !(leafs[l].contents & 1) && leafs[l].cluster >= 0 &&
                                     (leafs[l].areaFlags & 0x1ff) != skyArea);
    }
  }
  std::vector<uint8_t> skyFace(faceCount, 0);
  if (skyArea >= 0) {
    const auto [leafFaces, leafFaceCount] = bsp.lump<uint16_t>(kLeafFaces);
    for (size_t l = 0; leafFaces && l < leafCount; ++l) {
      if ((leafs[l].areaFlags & 0x1ff) != skyArea)
        continue;
      for (uint32_t k = 0; k < leafs[l].numLeafFaces; ++k) {
        const size_t at = size_t(leafs[l].firstLeafFace) + k;
        if (at < leafFaceCount && leafFaces[at] < faceCount)
          skyFace[leafFaces[at]] = 1;
      }
    }
  }
  auto inSky = [&](const float p[3]) {
    if (skyArea < 0)
      return false;
    const int leaf = leafAt(bsp, p);
    return leaf >= 0 && size_t(leaf) < leafCount && (leafs[leaf].areaFlags & 0x1ff) == skyArea;
  };

  // Materials by texdata.
  std::vector<int> materialOf(texDataCount, -2); // -2 unvisited
  auto material = [&](int texData) -> int {
    if (texData < 0 || size_t(texData) >= texDataCount)
      return -1;
    int &slot = materialOf[texData];
    if (slot != -2)
      return slot;
    slot = -1;
    const int32_t nameId = texDatas[texData].nameId;
    if (nameId < 0 || size_t(nameId) >= stringTableCount || size_t(stringTable[nameId]) >= stringsLength)
      return slot;
    const char *name = strings + stringTable[nameId];
    return slot = materialByName(std::string(name, strnlen(name, stringsLength - stringTable[nameId])), true);
  };

  // Leaves under water: the water material each one is in, for a camera
  // inside it (waterAt).
  {
    const auto [waterData, waterDataCount] = bsp.lump<LeafWater>(kLeafWaterData);
    const auto [infos, infoCount] = bsp.lump<TexInfo>(kTexInfo);
    g_world.leafWater.assign(leafCount, -1);
    for (size_t l = 0; leafs && waterData && infos && l < leafCount; ++l) {
      const int16_t w = leafs[l].waterData;
      if (!(leafs[l].contents & 0x20) || w < 0 || size_t(w) >= waterDataCount) // CONTENTS_WATER
        continue;
      const int16_t surface = waterData[w].surfaceTexInfo;
      if (surface < 0 || size_t(surface) >= infoCount)
        continue;
      const int m = material(infos[surface].texData);
      if (m >= 0 && g_world.materials[size_t(m)].waterBody)
        g_world.leafWater[l] = m;
    }
  }

  constexpr float kCell = 2048;
  // skybox: a face of the 3D skybox baked into world space. Such faces and
  // $nocull materials get groups of their own (the camera's culling is per
  // instance); the other groups keep the keys, and so the ids, they had.
  auto groupFor = [&](const float centroid[3], bool sky, int materialIndex, bool skybox) -> Geometry & {
    const auto &m = g_world.materials[materialIndex];
    std::array<int32_t, 6> key = {int32_t(std::floor(centroid[0] / kCell)), int32_t(std::floor(centroid[1] / kCell)),
                                  int32_t(std::floor(centroid[2] / kCell)), int32_t(sky) | (skybox ? 2 : 0) | (m.noCull ? 4 : 0),
                                  m.alphaTest ? m.alphaRef + 1 : 0, int32_t(m.categories)};
    auto &group = g_world.groups[key];
    if (!group.id) {
      group.sky = sky;
      group.skybox = skybox;
      group.noCull = m.noCull;
      group.alphaTest = m.alphaTest;
      group.alphaRef = m.alphaRef;
      group.categories = m.categories;
      group.id = XXH3_64bits_withSeed(key.data(), sizeof(key), 0x6273707764ull) | 1;
    }
    return group.surfaces[uint32_t(materialIndex)];
  };
  auto vertex = [&](const float p[3], const float n[3], const TexInfo &ti, const TexData &td) {
    remixapi_HardcodedVertex v = {};
    std::memcpy(v.position, p, 12);
    std::memcpy(v.normal, n, 12);
    const float w = td.width > 0 ? float(td.width) : 1, h = td.height > 0 ? float(td.height) : 1;
    v.texcoord[0] = (dot(ti.textureVecs[0], p) + ti.textureVecs[0][3]) / w;
    v.texcoord[1] = (dot(ti.textureVecs[1], p) + ti.textureVecs[1][3]) / h;
    v.color = 0xffffffff;
    return v;
  };

  const Model &world = models[0];
  // The map's own visible flat faces (planes and bounds): a traced skybox's
  // copies of the map boundary that lie on them would z-fight with them.
  struct WorldPlane {
    float normal[3], dist, mins[3], maxs[3];
  };
  std::vector<WorldPlane> worldPlanes;
  if (g_world.skyTraced)
    for (int32_t f = world.firstFace; f < world.firstFace + world.numFaces && size_t(f) < faceCount; ++f) {
      const Face &face = faces[f];
      if (face.texInfo < 0 || size_t(face.texInfo) >= texInfoCount || face.numEdges < 3 || face.dispInfo >= 0 ||
          size_t(face.plane) >= planeCount || skyFace[size_t(f)])
        continue;
      if (texInfos[face.texInfo].flags & (kSurfSky2D | kSurfSky | kSurfNoDraw | kSurfHint | kSurfSkip | kSurfTrans))
        continue;
      WorldPlane w;
      std::memcpy(w.normal, planes[face.plane].normal, sizeof(w.normal));
      w.dist = planes[face.plane].dist;
      bool valid = true;
      for (int e = 0; e < face.numEdges && valid; ++e) {
        const size_t se = size_t(face.firstEdge + e);
        const int32_t edge = se < surfEdgeCount ? surfEdges[se] : 0;
        const size_t ei = size_t(edge < 0 ? -edge : edge);
        valid = se < surfEdgeCount && ei < edgeCount && edges[ei][edge < 0 ? 1 : 0] < vertexCount;
        if (!valid)
          break;
        const auto &v = vertexes[edges[ei][edge < 0 ? 1 : 0]];
        for (int k = 0; k < 3; ++k) {
          w.mins[k] = e ? std::min(w.mins[k], v[k]) : v[k];
          w.maxs[k] = e ? std::max(w.maxs[k], v[k]) : v[k];
        }
      }
      if (valid && !inSky(w.mins))
        worldPlanes.push_back(w);
    }
  // Whether a skybox piece lies on (within a sky unit of) a world face's plane, over that face.
  auto onWorldFace = [&](const std::vector<remixapi_HardcodedVertex> &piece) {
    const float *a = piece[0].position, *b = piece[1].position, *c = piece[2].position;
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const float length = std::sqrt(dot(n, n));
    if (length < 1e-6f)
      return false;
    for (float &x : n)
      x /= length;
    float centroid[3] = {};
    for (const auto &v : piece)
      for (int k = 0; k < 3; ++k)
        centroid[k] += v.position[k] / float(piece.size());
    const float tolerance = g_world.skyScale;
    for (const auto &w : worldPlanes) {
      if (std::fabs(dot(w.normal, n)) < 0.996f || std::fabs(dot(w.normal, centroid) - w.dist) > tolerance)
        continue;
      bool over = true;
      for (int k = 0; k < 3 && over; ++k)
        over = centroid[k] >= w.mins[k] - tolerance && centroid[k] <= w.maxs[k] + tolerance;
      if (over)
        return true;
    }
    return false;
  };

  // The world's faces, then those of the func_lod brushes.
  std::vector<int32_t> faceList;
  for (int32_t f = world.firstFace; f < world.firstFace + world.numFaces && size_t(f) < faceCount; ++f)
    faceList.push_back(f);
  const size_t worldFaces = faceList.size();
  for (int32_t index : lodModels(bsp, modelCount)) {
    ++g_world.lodEntities;
    const Model &lod = models[index];
    for (int32_t f = lod.firstFace; f < lod.firstFace + lod.numFaces && size_t(f) < faceCount; ++f)
      faceList.push_back(f);
  }
  for (size_t listed = 0; listed < faceList.size(); ++listed) {
    const int32_t f = faceList[listed];
    const bool lodFace = listed >= worldFaces;
    const Face &face = faces[f];
    if (face.texInfo < 0 || size_t(face.texInfo) >= texInfoCount || face.numEdges < 3 ||
        size_t(face.plane) >= planeCount)
      continue;
    const TexInfo &ti = texInfos[face.texInfo];
    if (ti.flags & (kSurfSky2D | kSurfSky | kSurfNoDraw | kSurfHint | kSurfSkip))
      continue;
    if (ti.flags & kSurfTrans) {
      ++g_world.skippedTranslucent;
      continue;
    }
    const int m = material(ti.texData);
    if (m < 0)
      continue;
    const TexData &td = texDatas[ti.texData];
    lightmaps::Mapping lightmapMapping;
    std::memcpy(lightmapMapping.vectors, ti.lightmapVecs, sizeof(ti.lightmapVecs));
    std::copy_n(face.lightmapMins, 2, lightmapMapping.minimum);
    std::copy_n(face.lightmapSize, 2, lightmapMapping.size);
    // The face looks along its plane's normal: dface_t::planenum is the face's
    // own plane and dface_t::side only that plane's parity (vbsp numbers the
    // two planes of a pair 2n and 2n + 1; the node the face lies on holds the
    // even one). On gm_construct, gm_flatgrass and gm_7eleven_v6 the parity
    // equals side for every face, the empty leaf is on the normal's side of
    // every face with one solid side, the brush the face belongs to is behind
    // it, and the face's vertices run clockwise seen from that side.
    float normal[3];
    std::memcpy(normal, planes[face.plane].normal, sizeof(normal));
    std::vector<std::array<float, 3>> corners;
    for (int e = 0; e < face.numEdges; ++e) {
      const size_t se = size_t(face.firstEdge + e);
      if (se >= surfEdgeCount)
        break;
      const int32_t edge = surfEdges[se];
      const size_t ei = size_t(edge < 0 ? -edge : edge);
      if (ei >= edgeCount)
        break;
      const uint16_t vi = edges[ei][edge < 0 ? 1 : 0];
      if (vi >= vertexCount)
        break;
      corners.push_back(vertexes[vi]);
    }
    if (corners.size() != size_t(face.numEdges))
      continue;
    if (lodFace) {
      ++g_world.lodFaces;
      for (const auto &c : corners)
        g_world.lodCorners.insert(cornerKey(c.data()));
    }
    // Which way the polygon winds seen from its front (twice its area along
    // the normal, negative: clockwise). The runtime takes a triangle's front
    // from its winding (clockwise seen from the front): a polygon wound the
    // other way is emitted reversed.
    float winding = 0;
    for (size_t k = 1; k + 1 < corners.size(); ++k) {
      const float *a = corners[0].data(), *b = corners[k].data(), *c = corners[k + 1].data();
      const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
      winding += dot(n, normal);
    }
    const bool reversed = winding > 0;
    float centroid[3] = {};
    for (auto &c : corners)
      for (int k = 0; k < 3; ++k)
        centroid[k] += c[k] / float(corners.size());
    // Into the leaf in front of the face. A displacement's brush is not solid
    // and its base face may lie buried in another brush (a terrain block
    // sunk into the skybox's floor): the leaf behind it counts as well. (For
    // brush faces it must not: what is behind them is another room.)
    const float probe[3] = {centroid[0] + normal[0], centroid[1] + normal[1], centroid[2] + normal[2]};
    const float behind[3] = {centroid[0] - normal[0], centroid[1] - normal[1], centroid[2] - normal[2]};
    // The 3D skybox: path traced in world space, or left to the engine's
    // sky view (l4d2-rtx keeps its draws).
    const bool skyboxFace = skyFace[size_t(f)] != 0 || inSky(probe) || (face.dispInfo >= 0 && inSky(behind));
    if (skyboxFace) {
      ++g_world.skyFaces;
      if (!g_world.skyTraced)
        continue;
      float moved[3];
      skyToWorld(centroid, moved);
      std::memcpy(centroid, moved, sizeof(moved));
    }
    const bool sky = false;
    // Bakes a skybox face's vertices (from `base`, indices from `firstIndex`)
    // into world space and drops triangles inside the playable map.
    auto toWorld = [&](Geometry &g, size_t base, size_t firstIndex, bool count = true) {
      if (!skyboxFace)
        return;
      for (size_t v = base; v < g.vertices.size(); ++v) {
        float p[3];
        skyToWorld(g.vertices[v].position, p);
        std::memcpy(g.vertices[v].position, p, sizeof(p));
        noteSkyReach(p);
      }
      // Source draws the skybox behind the whole map. Here it shares world
      // space: what reaches into playable space (stand-ins of the map, or
      // skybox walls and ground misaligned with the map's own) is clipped
      // away along the map's BSP planes.
      const std::vector<uint32_t> triangles(g.indices.begin() + firstIndex, g.indices.end());
      g.indices.resize(firstIndex);
      std::vector<std::vector<remixapi_HardcodedVertex>> pieces;
      for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
        pieces.clear();
        SkyClipResult clipped;
        clipOutsidePlayable(bsp, 0,
                            {g.vertices[triangles[i]], g.vertices[triangles[i + 1]], g.vertices[triangles[i + 2]]},
                            pieces, clipped);
        pieces.erase(std::remove_if(pieces.begin(), pieces.end(),
                                    [&](const auto &piece) {
                                      const bool coplanar = onWorldFace(piece);
                                      g_world.skyCoplanarPieces += coplanar && count;
                                      clipped.split |= coplanar;
                                      return coplanar;
                                    }),
                     pieces.end());
        if (!clipped.split) {
          g.indices.insert(g.indices.end(), triangles.begin() + i, triangles.begin() + i + 3);
          g_world.skyTracedTriangles += count;
          continue;
        }
        if (pieces.empty())
          g_world.skyStandInTriangles += count;
        else
          g_world.skyClippedTriangles += count;
        for (const auto &piece : pieces) {
          const uint32_t first = uint32_t(g.vertices.size());
          g.vertices.insert(g.vertices.end(), piece.begin(), piece.end());
          for (uint32_t k = 1; k + 1 < piece.size(); ++k)
            g.indices.insert(g.indices.end(), {first, first + k, first + k + 1});
        }
      }
    };

    if (face.dispInfo >= 0 && dispInfos && size_t(face.dispInfo) < dispInfoCount && corners.size() == 4) {
      // Displacement: a (2^power+1)^2 grid over the face, offset per vertex;
      // the corners start at the one nearest the displacement's start.
      const DispInfo &di = dispInfos[face.dispInfo];
      int first = 0;
      float best = 1e30f;
      for (int c = 0; c < 4; ++c) {
        float d2 = 0;
        for (int k = 0; k < 3; ++k)
          d2 += (corners[c][k] - di.start[k]) * (corners[c][k] - di.start[k]);
        if (d2 < best) {
          best = d2;
          first = c;
        }
      }
      std::array<float, 3> p[4];
      for (int c = 0; c < 4; ++c)
        p[c] = corners[(first + c) % 4];
      const int size = (1 << di.power) + 1;
      if (di.power < 2 || di.power > 4 || di.firstVert < 0 || size_t(di.firstVert + size * size) > dispVertCount)
        continue;
      // The grid, then its triangles (indices into it).
      std::vector<remixapi_HardcodedVertex> grid;
      std::vector<float> alpha; // DISP_VERTS alpha, 0..255
      std::vector<std::array<float, 2>> lightmapCoordinates;
      grid.reserve(size_t(size) * size);
      alpha.reserve(size_t(size) * size);
      for (int v = 0; v < size; ++v) {
        const float t = float(v) / float(size - 1);
        float left[3], right[3];
        for (int k = 0; k < 3; ++k) {
          left[k] = p[0][k] + (p[1][k] - p[0][k]) * t;
          right[k] = p[3][k] + (p[2][k] - p[3][k]) * t;
        }
        for (int u = 0; u < size; ++u) {
          const float s = float(u) / float(size - 1);
          const DispVert &dv = dispVerts[di.firstVert + v * size + u];
          float flat[3], pos[3];
          for (int k = 0; k < 3; ++k) {
            flat[k] = left[k] + (right[k] - left[k]) * s;
            pos[k] = flat[k] + dv.vector[k] * dv.dist;
          }
          // Texture coordinates follow the undisplaced surface.
          auto out = vertex(flat, normal, ti, td);
          std::memcpy(out.position, pos, 12);
          std::memset(out.normal, 0, 12);
          grid.push_back(out);
          lightmapCoordinates.push_back(lightmapMapping.displacement(s, t));
          alpha.push_back(std::isfinite(dv.alpha) ? std::clamp(dv.alpha, 0.0f, 255.0f) : 0.0f);
        }
      }
      std::vector<uint32_t> triangles;
      triangles.reserve(size_t(size - 1) * (size - 1) * 6);
      for (int v = 0; v + 1 < size; ++v)
        for (int u = 0; u + 1 < size; ++u) {
          const uint32_t i = uint32_t(v * size + u), right = i + 1, down = i + uint32_t(size), diagonal = down + 1;
          if ((v * size + u) % 2)
            triangles.insert(triangles.end(), {i, down, right, right, down, diagonal});
          else
            triangles.insert(triangles.end(), {i, down, diagonal, i, diagonal, right});
        }
      // (The grid's triangles wind like the base face's corners.)
      if (reversed)
        for (size_t i = 0; i + 2 < triangles.size(); i += 3)
          std::swap(triangles[i + 1], triangles[i + 2]);
      g_world.cull.sideFaces += face.side != 0;
      // Smooth normals from the triangles, facing the base surface's side.
      for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
        auto &a = grid[triangles[i]], &b = grid[triangles[i + 1]], &c = grid[triangles[i + 2]];
        const float e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
        const float e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        // (Against the base's normal: clockwise seen from the base's front.
        // The rest are overhangs, seen from where the displaced surface faces.)
        ++(dot(n, normal) < 0 ? g_world.cull.dispClockwise : g_world.cull.dispCounterClockwise);
        if (dot(n, normal) < 0)
          for (float &x : n)
            x = -x;
        for (auto *vtx : {&a, &b, &c})
          for (int k = 0; k < 3; ++k)
            vtx->normal[k] += n[k];
      }
      for (auto &vtx : grid) {
        float *n = vtx.normal;
        const float len = std::sqrt(dot(n, n));
        for (int k = 0; k < 3; ++k)
          n[k] = len > 1e-12f ? n[k] / len : normal[k];
      }
      // Adds triangles of the grid to a geometry. sheet: as a layer lifted off
      // the surface whose vertex alpha is `blend`.
      constexpr float kLayerOffset = 0.05f; // below the overlays and the engine's decals (0.1)
      std::vector<float> blend;
      auto emit = [&](Geometry &g, const std::vector<uint32_t> &indices, bool sheet) {
        if (indices.empty())
          return;
        const uint32_t base = uint32_t(g.vertices.size());
        const size_t firstIndex = g.indices.size();
        g.vertices.insert(g.vertices.end(), grid.begin(), grid.end());
        if (sheet)
          for (size_t v = 0; v < grid.size(); ++v) {
            auto &vtx = g.vertices[base + v];
            for (int k = 0; k < 3; ++k)
              vtx.position[k] += vtx.normal[k] * kLayerOffset;
            vtx.color = 0x00ffffffu | uint32_t(std::lround(std::clamp(blend[v], 0.0f, 1.0f) * 255)) << 24;
          }
        for (uint32_t i : indices)
          g.indices.push_back(base + i);
        if (!skyboxFace)
          if (auto chart = lightmaps::capture(lightmapMapping, uint32_t(firstIndex), g.indices, base, lightmapCoordinates))
            g.lightmaps.push_back(std::move(*chart));
        toWorld(g, base, firstIndex, !sheet);
      };
      // A two-layer material ($basetexture2): the displacement's alpha blends
      // the second texture over the first, lerp(base, base2, alpha / 255) (the
      // engine bakes the alpha into the lightmap's alpha channel, which the
      // shaders read as the blend factor). Triangles whose blend is 1 at
      // every vertex are drawn with the second texture alone, those at 0 with
      // the first; across the others the second texture is a decal sheet over
      // the first with the blend as its vertex alpha, which the runtime
      // composes into the surface below like the overlays.
      bool anyAlpha = false;
      for (float a : alpha)
        anyAlpha |= a > 0;
      const bool twoLayers = secondLayer(m, false).present;
      // (Looked up for every such displacement: $maskedblending blends
      // without alpha.)
      const SecondLayer layer = twoLayers ? secondLayer(m, true) : SecondLayer{};
      if (twoLayers) {
        ++g_world.layers.displacements;
        for (float a : alpha) {
          g_world.layers.alphaMin = std::min(g_world.layers.alphaMin, a);
          g_world.layers.alphaMax = std::max(g_world.layers.alphaMax, a);
        }
      }
      if (layer.material < 0 || (!anyAlpha && !layer.masked)) {
        if (twoLayers)
          ++(layer.material < 0 ? g_world.layers.withoutTexture : g_world.layers.flat);
        emit(groupFor(centroid, sky, m, skyboxFace), triangles, false);
      } else {
        blend.resize(alpha.size());
        const float low = std::clamp(layer.centre - layer.halfWidth, 0.0f, 1.0f),
                    high = std::clamp(layer.centre + layer.halfWidth, 0.0f, 1.0f);
        for (size_t v = 0; v < alpha.size(); ++v) {
          const float a = alpha[v] / 255;
          if (layer.masked) {
            blend[v] = layer.centre;
          } else if (layer.modulate) {
            const float x = high > low ? std::clamp((a - low) / (high - low), 0.0f, 1.0f) : (a >= high ? 1.0f : 0.0f);
            blend[v] = x * x * (3 - 2 * x);
          } else {
            blend[v] = a;
          }
        }
        std::vector<uint32_t> first, second, mixed;
        for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
          const float b[3] = {blend[triangles[i]], blend[triangles[i + 1]], blend[triangles[i + 2]]};
          const bool none = std::max({b[0], b[1], b[2]}) < 0.002f, all = std::min({b[0], b[1], b[2]}) > 0.998f;
          auto &to = all ? second : first;
          to.insert(to.end(), triangles.begin() + i, triangles.begin() + i + 3);
          if (!all && !none)
            mixed.insert(mixed.end(), triangles.begin() + i, triangles.begin() + i + 3);
        }
        // (groupFor creates the group's surface for the material: only for
        // triangles. A surface without any makes the runtime reject the whole
        // mesh of the group, and with it the first layer under the sheets.)
        if (!first.empty())
          emit(groupFor(centroid, sky, m, skyboxFace), first, false);
        if (!second.empty())
          emit(groupFor(centroid, sky, layer.material, skyboxFace), second, false);
        if (!mixed.empty()) {
          const bool noCull = g_world.materials[m].noCull;
          std::array<int32_t, 5> key = {int32_t(std::floor(centroid[0] / kCell)), int32_t(std::floor(centroid[1] / kCell)),
                                        int32_t(std::floor(centroid[2] / kCell)), 2,
                                        int32_t(sky) | (skyboxFace ? 2 : 0) | (noCull ? 4 : 0)};
          auto &group = g_world.layerGroups[key];
          group.sky = sky;
          group.skybox = skyboxFace;
          group.noCull = noCull;
          group.id = XXH3_64bits_withSeed(key.data(), sizeof(key), 0x6c61796572ull) | 1;
          emit(group.surfaces[uint32_t(layer.material)], mixed, true);
        }
        ++g_world.layers.layered;
        g_world.layers.baseTriangles += uint32_t((first.size() - mixed.size()) / 3);
        g_world.layers.secondTriangles += uint32_t(second.size() / 3);
        g_world.layers.blendedTriangles += uint32_t(mixed.size() / 3);
      }
      g_world.triangles += uint32_t((size - 1) * (size - 1) * 2);
      ++g_world.displacements;
    } else if (face.dispInfo < 0 && g_world.materials[size_t(m)].waterBody && normal[2] < 0) {
      // Water is two-sided (see materialByName): its upward faces cover it,
      // and a downward face at the same place stopped rays going down at once
      // (no depth seen through the water there).
      ++g_world.waterDownFaces;
    } else if (face.dispInfo < 0) {
      g_world.layers.brushFaces += secondLayer(m, false).present;
      Geometry &g = groupFor(centroid, sky, m, skyboxFace);
      const uint32_t base = uint32_t(g.vertices.size());
      const size_t firstIndex = g.indices.size();
      for (auto &c : corners)
        g.vertices.push_back(vertex(c.data(), normal, ti, td));
      g_world.cull.sideFaces += face.side != 0;
      ++(winding < 0 ? g_world.cull.clockwise : winding > 0 ? g_world.cull.counterClockwise : g_world.cull.degenerate);
      for (uint32_t k = 1; k + 1 < corners.size(); ++k) {
        if (reversed)
          g.indices.insert(g.indices.end(), {base, base + k + 1, base + k});
        else
          g.indices.insert(g.indices.end(), {base, base + k, base + k + 1});
        // A fan triangle wound against its face: a sliver at collinear
        // vertices, covered by its neighbours (kept: the map's triangles
        // are the same with or without culling).
        const float *a = corners[0].data(), *b = corners[k].data(), *c = corners[k + 1].data();
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        g_world.cull.slivers += winding != 0 && dot(n, normal) * winding < 0;
      }
      if (!skyboxFace) {
        std::vector<std::array<float, 2>> coordinates;
        for (const auto &corner : corners)
          coordinates.push_back(lightmapMapping.project(corner.data()));
        for (auto &chart : lightmaps::captureFan(lightmapMapping, uint32_t(firstIndex), g.indices, base, coordinates))
          g.lightmaps.push_back(std::move(chart));
      }
      toWorld(g, base, firstIndex);
      g_world.triangles += uint32_t(corners.size() - 2);
      ++g_world.faces;
    }
  }
  loadProps(bsp, inSky);
  loadOverlays(bsp, inSky);
  loadLights(bsp, inSky);
  return !g_world.groups.empty();
}

// The map's potentially visible sets for the runtime's light selection
// (remixapi_SetVisibilityPartitionEXT): BSP nodes, leaf clusters and the
// decompressed cluster PVS rows. OPENREMIX_PROXY_PVS=0 disables it.
struct PartitionStats {
  uint32_t nodes = 0, leaves = 0, clusters = 0;
  int result = -1;
} g_partition;

void sendPartition(const Bsp *bsp) {
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_PVS", v, sizeof(v)) && v[0] == '0');
  }();
  static auto set = reinterpret_cast<PFN_remixapi_SetVisibilityPartitionEXT>(
      GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_SetVisibilityPartitionEXT"));
  if (!set)
    return;
  g_partition = {};
  if (!bsp || !enabled) {
    g_partition.result = set(nullptr);
    return;
  }
  const auto [nodes, nodeCount] = bsp->lump<Node>(kNodes);
  const auto [planes, planeCount] = bsp->lump<Plane>(kPlanes);
  const std::vector<Leaf> leafStore = leafsOf(*bsp);
  const Leaf *leafs = leafStore.empty() ? nullptr : leafStore.data();
  const size_t leafCount = leafStore.size();
  const auto [vis, visBytes] = bsp->lump<uint8_t>(kVisibility);
  if (!nodes || !planes || !leafs || !vis || visBytes < 4) {
    g_partition.result = set(nullptr);
    return;
  }
  int32_t clusters = 0;
  std::memcpy(&clusters, vis, 4);
  if (clusters <= 0 || size_t(4 + clusters * 8) > visBytes) {
    g_partition.result = set(nullptr);
    return;
  }
  std::vector<float> nodePlanes(nodeCount * 4);
  std::vector<int32_t> children(nodeCount * 2);
  for (size_t i = 0; i < nodeCount; ++i) {
    const Plane &plane = planes[size_t(nodes[i].plane) < planeCount ? nodes[i].plane : 0];
    std::memcpy(&nodePlanes[i * 4], plane.normal, 12);
    nodePlanes[i * 4 + 3] = plane.dist;
    children[i * 2] = nodes[i].children[0];
    children[i * 2 + 1] = nodes[i].children[1];
  }
  std::vector<int32_t> leafClusters(leafCount);
  for (size_t i = 0; i < leafCount; ++i)
    leafClusters[i] = leafs[i].cluster < clusters ? leafs[i].cluster : -1;
  const uint32_t rowBytes = uint32_t(clusters + 7) / 8;
  std::vector<uint8_t> rows(size_t(clusters) * rowBytes, 0);
  for (int32_t c = 0; c < clusters; ++c) {
    const auto row = pvs(*bsp, c);
    if (row.size() == rowBytes)
      std::memcpy(&rows[size_t(c) * rowBytes], row.data(), rowBytes);
    else
      std::memset(&rows[size_t(c) * rowBytes], 0xff, rowBytes); // unknown: everything visible
  }
  remixapi_VisibilityPartitionEXT info = {};
  info.structSize = sizeof(info);
  info.nodeCount = uint32_t(nodeCount);
  info.planes = nodePlanes.data();
  info.children = children.data();
  info.leafCount = uint32_t(leafCount);
  info.leafClusters = leafClusters.data();
  info.clusterCount = uint32_t(clusters);
  info.rowBytes = rowBytes;
  info.visibility = rows.data();
  g_partition = {uint32_t(nodeCount), uint32_t(leafCount), uint32_t(clusters), int(set(&info))};
}

std::wstring gameRoot() {
  wchar_t exe[32768] = {};
  const DWORD count = GetEnvironmentVariableW(L"OPENREMIX_PROXY_CONTENT_ROOT", exe, 32768);
  if (count && count < 32768)
    return exe;
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring root = exe;
  return root.substr(0, root.find_last_of(L"\\/"));
}

void ensureFileSystem() {
  static bool initialized = false;
  if (!initialized) {
    fs::init(gameRoot());
    initialized = true;
  }
}

} // namespace

std::vector<uint64_t> textureCandidates(const std::string &name, uint32_t frame) {
  ensureFileSystem();
  return textureHashes(name, frame);
}

uint64_t textureReplacementHash(const std::string &name, uint64_t sourceHash) {
  ensureFileSystem();
  const auto path = vtfPath(name);
  for (const bool engine : {false, true}) {
    const auto hashes = textureHashes(path, engine);
    const auto found = std::find(hashes.begin(), hashes.end(), sourceHash);
    if (found != hashes.end()) {
      const auto legacy = textureHashes(path, engine, true);
      const auto index = size_t(found - hashes.begin());
      return index < legacy.size() ? legacy[index] : 0;
    }
  }
  return 0;
}

bool textureImage(const std::string &name, uint32_t &width, uint32_t &height,
                  std::vector<uint8_t> &rgba, std::string &error, bool freezeAnimation) {
  ensureFileSystem();
  std::vector<std::vector<uint8_t>> frames;
  if (const char *failure = vtfFrames(name, width, height, frames)) {
    error = failure;
    return false;
  }
  if (frames.empty()) {
    error = "texture has no decoded frames";
    return false;
  }
  if (frames.size() != 1 && !freezeAnimation) {
    error = "animated texture requires engine playback";
    return false;
  }
  if (frames.size() > 1)
    std::fprintf(stderr, "Diagnostic texture snapshot: %s frozen at frame 0 of %zu\n", name.c_str(), frames.size());
  rgba = std::move(frames.front());
  error.clear();
  return true;
}

bool materialSource(uint64_t textureHash, std::string &baseTexture) {
  for (const auto &material : g_world.materials)
    if (std::find(material.candidates.begin(), material.candidates.end(), textureHash) != material.candidates.end()) {
      baseTexture = material.baseTexture;
      return !baseTexture.empty();
    }
  return false;
}

bool playerStart(float origin[3], float angles[3]) {
  if (!g_active || !g_world.hasPlayerStart)
    return false;
  std::copy_n(g_world.playerOrigin, 3, origin);
  std::copy_n(g_world.playerAngles, 3, angles);
  return true;
}

float textureLuminance(const std::string &name) {
  static std::mutex lock;
  static std::unordered_map<std::string, float> known;
  {
    std::lock_guard<std::mutex> guard(lock);
    if (auto it = known.find(name); it != known.end())
      return it->second;
  }
  ensureFileSystem();
  float rgba[4];
  const float value = textureMean(name, rgba) ? 0.2126f * rgba[0] + 0.7152f * rgba[1] + 0.0722f * rgba[2] : -1.0f;
  std::lock_guard<std::mutex> guard(lock);
  if (known.size() > 8192)
    known.clear();
  known[name] = value;
  return value;
}

static void loadSkyFace(KeyValues &values, SkyboxFace &image) {
  std::string error;
  const auto &compressed = values.values["$hdrcompressedtexture"];
  const auto &hdr = values.values["$hdrbasetexture"];
  if (!compressed.empty()) {
    // RGBS is linear RGB times alpha times eight, decoded before filtering.
    if (!textureImage(compressed, image.width, image.height, image.rgba, error))
      throw std::runtime_error(error);
    image.linearRgba = proxy::sky::decodeRgbs(image.rgba);
    image.rgba.clear();
  } else if (!values.values["$hdrcompressedtexture0"].empty()) {
    throw std::runtime_error("three-texture HDR sky encoding is unsupported");
  } else if (!hdr.empty()) {
    std::vector<uint8_t> file;
    const auto path = vtfPath(hdr);
    uint64_t bytes = 0;
    if (!fs::size(path, bytes) || bytes > (64u << 20) || !fs::read(path, file, 0, bytes) || file.size() != bytes)
      throw std::runtime_error("HDR sky texture is missing or exceeds 64 MiB");
    uint32_t format = 0;
    if (file.size() >= 56) std::memcpy(&format, file.data() + 52, 4);
    if (format == 24 || format == 25 || format == 28 || format == 29)
      image.linearRgba = proxy::sky::decodeHdrVtf(file, image.width, image.height);
    else if (!textureImage(hdr, image.width, image.height, image.rgba, error))
      throw std::runtime_error(error);
  } else if (!textureImage(values.values["$basetexture"], image.width, image.height, image.rgba, error)) {
    throw std::runtime_error(error);
  }
  float tint[3]{1, 1, 1};
  if (values.values.count("$color")) {
    if (!parseColor(values.values["$color"], tint) ||
        std::any_of(tint, tint + 3, [](float v) { return !std::isfinite(v) || v < 0; }))
      throw std::runtime_error("invalid sky colour multiplier");
    if (image.linearRgba.empty()) {
      image.linearRgba.resize(image.rgba.size(), 1);
      for (size_t pixel = 0; pixel < image.rgba.size(); pixel += 4)
        for (unsigned c = 0; c < 3; ++c) {
          const float value = float(image.rgba[pixel + c]) / 255;
          image.linearRgba[pixel + c] = value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
        }
      image.rgba.clear();
    }
    for (size_t pixel = 0; pixel < image.linearRgba.size(); pixel += 4)
      for (unsigned c = 0; c < 3; ++c) image.linearRgba[pixel + c] *= tint[c];
  }
}

const SkyboxImages &skyboxImages() { return g_world.skyboxImages; }

bool textureRgba(const std::string &name, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgba) {
  ensureFileSystem();
  std::vector<std::vector<uint8_t>> frames;
  if (vtfFrames(name, width, height, frames) || frames.empty() ||
      frames[0].size() != size_t(width) * height * 4)
    return false;
  rgba = std::move(frames[0]);
  return true;
}

bool textureMeanColor(const std::string &name, float rgba[4]) {
  static std::mutex lock;
  static std::unordered_map<std::string, std::array<float, 5>> known; // rgba, found
  {
    std::lock_guard<std::mutex> guard(lock);
    if (auto it = known.find(name); it != known.end()) {
      std::copy_n(it->second.begin(), 4, rgba);
      return it->second[4] != 0;
    }
  }
  ensureFileSystem();
  std::array<float, 5> value = {1, 1, 1, 1, 0};
  if (textureMean(name, value.data()))
    value[4] = 1;
  std::lock_guard<std::mutex> guard(lock);
  if (known.size() > 8192)
    known.clear();
  known[name] = value;
  std::copy_n(value.begin(), 4, rgba);
  return value[4] != 0;
}

bool load(const remixapi_Interface &api, const std::string &requestedMap, bool lightStylesAvailable,
          bool decodeSkybox) {
  std::string mapName = lower(requestedMap);
  if (mapName.rfind("maps/", 0) == 0)
    mapName.erase(0, 5);
  if (mapName.size() > 4 && mapName.compare(mapName.size() - 4, 4, ".bsp") == 0)
    mapName.resize(mapName.size() - 4);
  static const bool enabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_WORLD", v, sizeof(v)) && v[0] == '0');
  }();
  if (!mapName.empty() && lower(mapName) == g_world.map && g_active)
    return true;
  publishPropIndex(nullptr);
  destroyMeshes();
  g_world = World{};
  g_world.api = &api;
  g_world.lightStylesAvailable = lightStylesAvailable;
  g_active = false;
  // Old inputs must not survive a map unload, missing BSP, or disabled world.
  invalidateBakeScene();
  if (mapName.empty()) {
    sendPartition(nullptr);
    return false;
  }
  ensureFileSystem();
  Bsp bsp;
  // A loose map file, else through the file system (workshop addons, etc.).
  const std::string file = "maps/" + lower(mapName) + ".bsp";
  const std::wstring path = fs::locate(file);
  std::vector<uint8_t> bytes;
  if (path.empty() ? !(fs::read(file, bytes) && bsp.open(std::move(bytes))) : !bsp.open(path)) {
    sendPartition(nullptr);
    return false;
  }
  {
    wchar_t overridePath[32768]{};
    const auto length = GetEnvironmentVariableW(L"OPENREMIX_PROXY_CUBEMAP_BOUNDS", overridePath, 32768);
    const auto destination = length && length < 32768 ? std::filesystem::path(overridePath)
        : std::filesystem::path(gameRoot()) / L"garrysmod/data/openremix/cubemaps" / (mapName + ".cubemaps.bin");
    try {
      g_cubemapOverrides.configure(bsp.data, XXH3_64bits(mapName.data(), mapName.size()) | 1, destination);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Cubemap authoring unavailable: %s\n", error.what());
    }
  }
  sendPartition(&bsp);
  if (!enabled)
    return false;
  const auto [pak, pakBytes] = bsp.lump<uint8_t>(kPakfile);
  fs::setPakfile(pak ? std::vector<uint8_t>(pak, pak + pakBytes) : std::vector<uint8_t>());
  g_world.map = lower(mapName);
  invalidateBakeScene();
  g_active = build(bsp);
  char reflectionHints[8] = {};
  const bool useReflectionHints = !(GetEnvironmentVariableA("OPENREMIX_PROXY_REFLECTION_HINTS", reflectionHints,
                                                            sizeof(reflectionHints)) && reflectionHints[0] == '0');
  if (g_active && useReflectionHints && bsp.lumpVersion(kCubemaps) == 0) {
    // dcubemapsample_t is 16 bytes: three integer coordinates, size, padding.
    // Its authored positions seed fitting; they never supply room boundaries.
    const auto [samples, count] = bsp.lump<std::array<int32_t, 4>>(kCubemaps);
    for (size_t i = 0; samples && i < count && g_world.reflectionSeeds.size() < 4096; ++i) {
      const std::array<float, 3> position{float(samples[i][0]), float(samples[i][1]), float(samples[i][2])};
      bool inside = insidePlayable(bsp, position.data());
      for (uint32_t c = 0; c < 3; ++c)
        inside &= position[c] >= g_world.playableMins[c] && position[c] <= g_world.playableMaxs[c];
      if (inside)
        g_world.reflectionSeeds.push_back(position);
    }
  }
  if (g_active && !g_world.props.empty()) {
    auto index = std::make_shared<PropIndex>();
    index->cells = g_world.propCells;
    for (const auto &prop : g_world.props) {
      std::array<float, 12> t;
      std::memcpy(t.data(), prop.transform, sizeof(prop.transform));
      if (prop.model->engineParts)
        t[3] = NAN; // never matches: the engine still draws part of it
      index->transforms.push_back(t);
    }
    publishPropIndex(std::move(index));
  }
  if (g_active && decodeSkybox && !g_world.skyboxImages.name.empty()) {
    // Decode while the map pakfile is mounted, just like ordinary BSP assets.
    // Procedural painted skies need an explicit frozen live-sky reference.
    const char *suffixes[] = {"rt", "lf", "bk", "ft", "up", "dn"};
    auto &sky = g_world.skyboxImages;
    if (lower(sky.name) == "painted") sky.error = "procedural painted sky requires a live-sky reference";
    for (unsigned face = 0; face < 6 && sky.error.empty(); ++face) {
      const auto material = "skybox/" + sky.name + suffixes[face];
      KeyValues values;
      if (!loadMaterial(material, values)) {
        sky.error = material + ": missing sky material";
        break;
      }
      auto &image = sky.faces[face];
      try {
        loadSkyFace(values, image);
      } catch (const std::exception &failure) {
        sky.error = material + ": " + failure.what();
      }
    }
  }
  fs::setPakfile({});
  if (!g_active)
    g_world = World{};
  return g_active;
}

bool active() { return g_active; }

bool overlaysActive() { return g_active && !g_world.overlayGroups.empty(); }

void publishBakeScene(const std::vector<remixapi_LightHandle> &lights,
                      const std::vector<remixapi_BakeStyledLightEXT> &styled,
                      const std::array<float, 6> *probeBounds) {
  static const auto publishCubemaps = sceneFunction<PFN_remixapi_SetReflectionVolumesEXT>("remixapi_SetReflectionVolumesEXT");
  if (g_active) g_cubemapOverrides.poll(publishCubemaps);
  static const auto publish = sceneFunction<PFN_remixapi_SetBakeSceneEXT>("remixapi_SetBakeSceneEXT");
  if (!publish) {
    g_world.bakeReason = "native bake-scene API unavailable";
    return;
  }
  if (!g_active || !g_world.sceneValid)
    return;
  // Never label missing brush groups or failed/partially decoded props a
  // complete map. Keep direct Forward+ lighting while static inputs load.
  if (g_world.propModelsFailed || std::any_of(g_world.propModels.begin(), g_world.propModels.end(),
                                             [](const auto &entry) { return entry.second.missingBakeOccluders; })) {
    g_world.bakeReason = "static prop geometry is incomplete";
    return;
  }
  for (const auto &[key, group] : g_world.groups)
    if (!group.mesh && std::any_of(group.surfaces.begin(), group.surfaces.end(),
                                   [](const auto &entry) { return !entry.second.indices.empty(); })) {
      g_world.bakeReason = "brush geometry is still loading";
      return;
    }
  if (g_world.bakeMaterialVersion != g_world.submitVersion ||
      g_world.bakeMaterialCount != g_world.materials.size()) {
    g_world.bakeMaterials = bakeMaterialDependencies(g_world);
    g_world.bakeMaterialVersion = g_world.submitVersion;
    g_world.bakeMaterialCount = g_world.materials.size();
  }
  if (const auto pending = firstUnreadyBakeMaterial(g_world.bakeMaterials, g_world.materials)) {
    if (*pending >= g_world.materials.size())
      g_world.bakeReason = "retained geometry references an invalid static material";
    else if (g_world.materials[*pending].candidates.empty())
      g_world.bakeReason = "required static material has no supported texture source";
    else
      g_world.bakeReason = "static material textures are still loading";
    return;
  }
  uint64_t signature = XXH3_64bits(lights.data(), lights.size() * sizeof(lights[0]));
  if (probeBounds)
    signature = XXH3_64bits_withSeed(probeBounds->data(), sizeof(*probeBounds), signature);
  for (const auto &light : styled) {
    const uint64_t fields[] = {uint64_t(reinterpret_cast<uintptr_t>(light.authoredLight)),
                               uint64_t(reinterpret_cast<uintptr_t>(light.runtimeLight)), light.style};
    signature = XXH3_64bits_withSeed(fields, sizeof(fields), signature);
  }
  // Successful requests compare dependency revisions: the native runtime
  // captures again only when a material/mod/light changed. Retry
  // refused inputs twice a second while late assets finish resolving.
  if (g_world.bakeStatus != int(REMIXAPI_ERROR_CODE_SUCCESS) &&
      g_world.bakeVersion == g_world.submitVersion && g_world.bakeLights == signature && g_world.frames % 30)
    return;
  remixapi_BakeSceneStyledLightsInfoEXT styledInfo = {};
  auto &reflections = styledInfo.reflections;
  auto &surfaces = reflections.surfaces;
  auto &bounds = surfaces.bounds;
  auto &info = bounds.scene;
  info.structSize = styled.empty() ? sizeof(reflections) : sizeof(styledInfo);
  info.version = styled.empty() ? REMIXAPI_BAKE_SCENE_REFLECTIONS_VERSION_EXT
                               : REMIXAPI_BAKE_SCENE_STYLED_LIGHTS_VERSION_EXT;
  styledInfo.styledLightCount = uint32_t(styled.size());
  styledInfo.styledLights = styled.data();
  reflections.reflectionRevision = 1;
  reflections.positionCount = uint32_t(g_world.reflectionSeeds.size());
  reflections.positions = g_world.reflectionSeeds.empty() ? nullptr : g_world.reflectionSeeds.front().data();
  std::vector<std::array<float, 3>> localReflectionSeeds;
  if (probeBounds) {
    for (const auto &seed : g_world.reflectionSeeds) {
      bool inside = true;
      for (size_t axis = 0; axis < 3; ++axis)
        inside &= seed[axis] >= (*probeBounds)[axis] && seed[axis] <= (*probeBounds)[axis + 3];
      if (inside) localReflectionSeeds.push_back(seed);
    }
    reflections.positionCount = uint32_t(localReflectionSeeds.size());
    reflections.positions = localReflectionSeeds.empty() ? nullptr : localReflectionSeeds.front().data();
  }
  surfaces.chartRevision = g_world.submitVersion + 1;
  surfaces.chartCount = uint32_t(g_world.bakeCharts.size());
  surfaces.charts = g_world.bakeCharts.data();
  std::copy_n(g_world.playableMins, 3, bounds.minimum);
  std::copy_n(g_world.playableMaxs, 3, bounds.maximum);
  // Diagnostic hosts can concentrate probe density in a reference domain.
  // This never clips the captured surfaces, occluders or light list.
  if (probeBounds) {
    std::copy_n(probeBounds->begin(), 3, bounds.minimum);
    std::copy_n(probeBounds->begin() + 3, 3, bounds.maximum);
  }
  info.sceneId = XXH3_64bits(g_world.map.data(), g_world.map.size()) | 1;
  info.revision = g_world.submitVersion + 1;
  info.sceneScale = .0254f;
  info.state = REMIXAPI_BAKE_SCENE_READY_EXT;
  info.instances = g_world.bakeInstances.data();
  info.instanceCount = uint32_t(g_world.bakeInstances.size());
  info.lights = lights.data();
  info.lightCount = uint32_t(lights.size());
  g_world.bakeStatus = int(publish(&info));
  g_world.bakeReason = g_world.bakeStatus == int(REMIXAPI_ERROR_CODE_SUCCESS)
                           ? "static inputs captured for GPU baking"
                           : "native capture refused unresolved or time-dependent inputs";
  if (g_world.bakeStatus != int(REMIXAPI_ERROR_CODE_SUCCESS)) {
    info.state = REMIXAPI_BAKE_SCENE_LOADING_EXT;
    info.instanceCount = info.lightCount = 0;
    info.instances = nullptr;
    info.lights = nullptr;
    surfaces.chartCount = 0;
    surfaces.charts = nullptr;
    reflections.positionCount = 0;
    reflections.positions = nullptr;
    styledInfo.styledLightCount = 0;
    styledInfo.styledLights = nullptr;
    publish(&info);
  }
  g_world.bakeVersion = g_world.submitVersion;
  g_world.bakeLights = signature;
}

float skyTracedReach() { return g_active && g_world.skyTraced ? g_world.skyReach : 0; }

bool skyCamera(float origin[3], float &scale) {
  if (!g_active || !g_world.skyCamera || !g_world.skyFaces)
    return false;
  std::memcpy(origin, g_world.skyOrigin, 12);
  scale = g_world.skyScale;
  return true;
}

static bool updateCubemapReceivers() {
  static const bool bindReceivers = [] {
    char option[8] = {};
    return GetEnvironmentVariableA("OPENREMIX_PROXY_CUBEMAP_RECEIVER_BINDINGS", option, sizeof(option)) && option[0] == '1';
  }();
  bool bindingsChanged = false;
  if (bindReceivers) {
    static const bool supported = [] {
      const auto query = sceneFunction<PFN_remixapi_GetReflectionBindingVersionEXT>("remixapi_GetReflectionBindingVersionEXT");
      const bool capable = cubemaps::supportsReceiverBindings(query);
      if (!capable)
        std::fprintf(stderr, "BSP receiver bindings disabled: runtime lacks frame-safe receiver binding support\n");
      return capable;
    }();
    if (!supported) return false;
    static const auto publish = sceneFunction<PFN_remixapi_SetReflectionVolumesEXT>("remixapi_SetReflectionVolumesEXT");
    g_cubemapOverrides.poll(publish);
    const auto revision = g_cubemapOverrides.acceptedRevision();
    if (g_world.receiverDocumentRevision == revision && g_world.receiverMaterialCount == g_world.materials.size())
      return false;
    g_world.receiverDocumentRevision = revision;
    g_world.receiverMaterialCount = g_world.materials.size();
    const auto document = g_cubemapOverrides.accepted();
    const auto sceneId = XXH3_64bits(g_world.map.data(), g_world.map.size()) | 1;
    for (auto &material : g_world.materials) {
      if (material.owned || material.requiresOwned || material.blended || material.modulate) continue;
      if (!material.captureReference) continue;
      const auto volumeId = document ? cubemaps::uniqueVolume(*material.captureReference, *document) : 0;
      const auto scope = volumeId ? sceneId : 0;
      auto &shading = material.shading;
      const auto receiver = XXH3_64bits_withSeed(material.name.data(), material.name.size(), sceneId) | 1;
      if (shading.reflectionReceiverKey == receiver && shading.reflectionSceneId == scope &&
          shading.reflectionVolumeId == volumeId) continue;
      shading.reflectionReceiverKey = receiver;
      shading.reflectionSceneId = scope;
      shading.reflectionVolumeId = volumeId;
      material.resolved = false; // retain its previous pin until the replacement resolves
      bindingsChanged = true;
    }
    if (bindingsChanged) {
      ++g_world.receiverRevision;
      const auto count = std::count_if(g_world.materials.begin(), g_world.materials.end(),
          [](const auto &material) { return material.shading.reflectionVolumeId != 0; });
      std::fprintf(stderr, "BSP authored receiver bindings: %zu materials, generation %llu\n", size_t(count),
                   static_cast<unsigned long long>(g_world.receiverRevision));
    }
  }
  return bindingsChanged;
}

void submit(MaterialLookup lookup) {
  if (!g_active || !g_world.api)
    return;
  const auto &api = *g_world.api;
  const uint64_t frame = g_world.frames++;
  animateSurfaceNormals();
  const bool bindingsChanged = updateCubemapReceivers();
  // Materials resolve once the engine has uploaded their textures (at map
  // load, as a rule): every frame at first, then twice a second.
  if (bindingsChanged || frame < 600 || frame % 30 == 0)
    for (auto &m : g_world.materials) {
      if (m.resolved || m.requiresOwned)
        continue;
      // A mask the engine never uploads (a format the proxy does not take)
      // must not keep the surface untextured: lit after ten seconds.
      // (A PBR conversion waits for its maps as well.)
      auto shading = m.shading;
      if (m.maskDropped) {
        shading.mode = engine_materials::Shading::Lit;
        shading.pbr = {};
        shading.mask.clear();
      }
      const bool emissive = shading.mode != engine_materials::Shading::Lit || shading.pbr.active;
      const bool variant = emissive || shading.reflectionReceiverKey;
      for (uint64_t h : m.candidates) {
        auto ref = lookup(h, variant ? &shading : nullptr, &m.maskCandidates, m.pointSample);
        if (!ref.handle && emissive && frame >= 600) {
          auto fallback = shading;
          fallback.mode = engine_materials::Shading::Lit;
          fallback.pbr = {};
          fallback.mask.clear();
          ref = lookup(h, fallback.reflectionReceiverKey ? &fallback : nullptr, nullptr, m.pointSample);
          if (ref.handle) m.maskDropped = true;
        }
        if (ref.handle) {
          if (ref.handle != m.ref.handle) ++g_world.submitVersion;
          m.ref = std::move(ref);
          m.resolved = true;
          break;
        }
      }
    }
  // Meshes are built when their materials are known (or after a grace
  // period, with the default material), and rebuilt as late ones resolve.
  auto build = [&](auto &group, bool resolvedOnly = false) {
    uint32_t resolved = 0;
    for (auto &[index, geometry] : group.surfaces)
      resolved += g_world.materials[index].resolved;
    if (group.mesh && resolved == group.builtResolved)
      return;
    // resolvedOnly (second layers): a sheet without its texture would paint
    // the default material over the first layer; it waits for the texture.
    if (resolvedOnly ? resolved == 0 : !group.mesh && resolved < group.surfaces.size() && frame < 30)
      return;
    std::vector<remixapi_MeshInfoSurfaceTriangles> surfaces;
    for (auto &[index, geometry] : group.surfaces) {
      if (resolvedOnly && !g_world.materials[index].resolved)
        continue;
      // CreateMesh rejects a mesh with an empty surface.
      if (geometry.indices.empty() || geometry.vertices.empty())
        continue;
      remixapi_MeshInfoSurfaceTriangles s = {};
      s.vertices_values = geometry.vertices.data();
      s.vertices_count = geometry.vertices.size();
      s.indices_values = geometry.indices.data();
      s.indices_count = geometry.indices.size();
      s.material = g_world.materials[index].ref.handle;
      surfaces.push_back(s);
    }
    if (surfaces.empty()) {
      group.builtResolved = resolved;
      return;
    }
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = group.id ^ resolved;
    info.surfaces_values = surfaces.data();
    info.surfaces_count = uint32_t(surfaces.size());
    remixapi_MeshHandle mesh = nullptr;
    if (api.CreateMesh(&info, &mesh) != REMIXAPI_ERROR_CODE_SUCCESS)
      return;
    if (group.mesh)
      api.DestroyMesh(group.mesh);
    group.mesh = mesh;
    group.builtResolved = resolved;
    ++g_world.submitVersion;
  };
  for (auto &[key, group] : g_world.groups)
    build(group);
  for (auto &[key, group] : g_world.overlayGroups)
    build(group);
  for (auto &[key, group] : g_world.layerGroups)
    build(group, true);
  // The instances below change rarely: once the submission has been stable
  // for a while it is recorded as a bridge batch (openremix_bridge_*) and
  // replayed with one command per frame instead of thousands of DrawInstance
  // encodings. OPENREMIX_PROXY_BSP_BATCH=0 always submits directly.
  using BeginFn = remixapi_ErrorCode(REMIXAPI_CALL *)(void);
  using BatchFn = remixapi_ErrorCode(REMIXAPI_CALL *)(uint64_t);
  static const HMODULE bridge = GetModuleHandleW(L"remix_bridge.dll");
  static const bool batchEnabled = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_BATCH", v, sizeof(v)) && v[0] == '0');
  }();
  static const auto beginRecording =
      bridge && batchEnabled ? reinterpret_cast<BeginFn>(GetProcAddress(bridge, "openremix_bridge_BeginRecording")) : nullptr;
  static const auto endRecording =
      bridge ? reinterpret_cast<BatchFn>(GetProcAddress(bridge, "openremix_bridge_EndRecording")) : nullptr;
  static const auto replayBatch =
      bridge ? reinterpret_cast<BatchFn>(GetProcAddress(bridge, "openremix_bridge_ReplayBatch")) : nullptr;
  if (g_world.submitVersion != g_world.stableVersion) {
    g_world.stableVersion = g_world.submitVersion;
    g_world.stableSince = frame;
  }
  if (g_world.batchValid && g_world.batchVersion == g_world.submitVersion && replayBatch &&
      replayBatch(kBatchId) == REMIXAPI_ERROR_CODE_SUCCESS) {
    ++g_world.replayedFrames;
    return;
  }
  g_world.batchValid = false;
  // In process there is no bridge: the stable submission becomes the
  // runtime's scene instances, added once and drawn by one call a frame. The
  // map is thousands of instances, and describing each to the runtime every
  // frame was a quarter of the frame (gm_voidplaces: 8200 instances, 4.8 ms).
  static const auto addScene =
      batchEnabled ? sceneFunction<PFN_remixapi_AddSceneInstancesEXT>("remixapi_AddSceneInstancesEXT") : nullptr;
  static const auto drawScene = sceneFunction<PFN_remixapi_DrawSceneInstancesEXT>("remixapi_DrawSceneInstancesEXT");
  if (g_world.sceneValid && g_world.sceneVersion == g_world.submitVersion && drawScene &&
      drawScene(nullptr) == REMIXAPI_ERROR_CODE_SUCCESS) {
    ++g_world.replayedFrames;
    return;
  }
  dropSceneInstances();
  const bool keep = addScene && drawScene && frame - g_world.stableSince >= 30 &&
                    g_world.sceneVersion != g_world.submitVersion;
  bool kept = keep;
  // An instance of the map: the runtime keeps it, or it is drawn this frame.
  const auto draw = [&](const remixapi_InstanceInfo &info) {
    uint64_t instance = 0;
    if (!keep)
      api.DrawInstance(&info);
    else if (addScene(&info, 1, &instance) == REMIXAPI_ERROR_CODE_SUCCESS) {
      g_world.sceneInstances.push_back(instance);
      if (!(info.categoryFlags & REMIXAPI_INSTANCE_CATEGORY_BIT_SKY))
        g_world.bakeInstances.push_back(instance);
    } else
      kept = false;
    return instance;
  };
  const auto captureCharts = [&](uint64_t retained, const auto &group, bool resolvedOnly = false) {
    if (!retained || group.sky)
      return;
    uint32_t surfaceIndex = 0;
    for (const auto &[material, geometry] : group.surfaces) {
      if (geometry.indices.empty() || geometry.vertices.empty() ||
          (resolvedOnly && !g_world.materials[material].resolved))
        continue;
      for (const auto &chart : geometry.lightmaps)
        g_world.bakeCharts.push_back({retained, surfaceIndex, 0, chart.firstIndex,
                                     uint32_t(chart.texcoords.size() / 2), chart.width, chart.height,
                                     chart.texcoords.data()});
      ++surfaceIndex;
    }
  };
  // What the camera does not see of the map, as in Source: back faces (except
  // $nocull materials) and what lies in clusters outside the camera cluster's
  // potentially visible set (the runtime locates the camera and each surface
  // point in the partition sendPartition() gave it; the 3D skybox, drawn by
  // Source whatever the PVS says, is exempt). Camera rays only: shadows,
  // bounces and reflections still meet every instance from both sides, so
  // lighting is what it was. OPENREMIX_PROXY_BSP_CULL_BACKFACES=0 and
  // OPENREMIX_PROXY_BSP_CULL_PVS=0 switch each off.
  static const uint32_t cullEnabled = [] {
    char v[8] = {};
    uint32_t flags = REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT | REMIXAPI_INSTANCE_CAMERA_CULL_VISIBILITY_EXT;
    if (GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_CULL_BACKFACES", v, sizeof(v)) && v[0] == '0')
      flags &= ~uint32_t(REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT);
    if (GetEnvironmentVariableA("OPENREMIX_PROXY_BSP_CULL_PVS", v, sizeof(v)) && v[0] == '0')
      flags &= ~uint32_t(REMIXAPI_INSTANCE_CAMERA_CULL_VISIBILITY_EXT);
    return flags;
  }();
  auto &cullStats = g_world.cull;
  cullStats.backFaces = cullStats.visibility = cullStats.twoSided = cullStats.skybox = cullStats.instances = 0;
  // Chains the camera culling of an instance in front of info.pNext.
  auto cameraCulling = [&](remixapi_InstanceInfo &info, remixapi_InstanceInfoCameraCullingEXT &culling, bool noCull,
                           bool skybox) {
    culling = {};
    culling.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_CAMERA_CULLING_EXT;
    culling.flags = cullEnabled & ~((noCull ? uint32_t(REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT) : 0u) |
                                    (skybox ? uint32_t(REMIXAPI_INSTANCE_CAMERA_CULL_VISIBILITY_EXT) : 0u));
    ++cullStats.instances;
    cullStats.twoSided += noCull;
    cullStats.skybox += skybox;
    cullStats.backFaces += (culling.flags & REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT) != 0;
    cullStats.visibility += (culling.flags & REMIXAPI_INSTANCE_CAMERA_CULL_VISIBILITY_EXT) != 0;
    if (!culling.flags)
      return;
    culling.pNext = info.pNext;
    info.pNext = &culling;
  };
  const bool record = beginRecording && endRecording && replayBatch && frame - g_world.stableSince >= 30 &&
                      g_world.batchVersion != g_world.submitVersion &&
                      beginRecording() == REMIXAPI_ERROR_CODE_SUCCESS;
  // Second layers of displacements: decal sheets over the first layer, their
  // coverage the vertex alpha alone (the texture's own alpha is not opacity).
  for (auto &[key, group] : g_world.layerGroups) {
    if (!group.mesh)
      continue;
    remixapi_InstanceInfoBlendEXT blend = {};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaTestCompareOp = 7;
    blend.alphaBlendEnabled = 1;
    blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6; // SRC_ALPHA
    blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7; // ONE_MINUS_SRC_ALPHA
    blend.writeMask = 15;
    blend.textureAlphaOperation = 1; // select argument 1: the vertex alpha
    blend.textureAlphaArg1Source = 2;
    remixapi_InstanceInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    remixapi_InstanceIdentityEXT identity = {};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.pNext = &blend;
    identity.instanceId = group.id;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    info.pNext = &identity;
    info.mesh = group.mesh;
    info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC |
                         REMIXAPI_INSTANCE_CATEGORY_BIT_TERRAIN |
                         (group.sky ? REMIXAPI_INSTANCE_CATEGORY_BIT_SKY : 0);
    info.transform.matrix[0][0] = info.transform.matrix[1][1] = info.transform.matrix[2][2] = 1;
    info.doubleSided = 1;
    // Paint is seen from its face's front only, and where its face is.
    remixapi_InstanceInfoCameraCullingEXT culling;
    cameraCulling(info, culling, group.noCull, group.skybox);
    captureCharts(draw(info), group, true);
  }
  // Overlays: static decals, blended over the surface below.
  for (auto &[key, group] : g_world.overlayGroups) {
    if (!group.mesh)
      continue;
    remixapi_InstanceInfoBlendEXT blend = {};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaTestCompareOp = 7;
    blend.alphaBlendEnabled = 1;
    // VkBlendFactor: SRC_ALPHA 6, ONE_MINUS_SRC_ALPHA 7, DST_COLOR 4, SRC_COLOR 2.
    blend.srcColorBlendFactor = group.modulate ? 4 : 6;
    blend.dstColorBlendFactor = group.modulate ? 2 : 7;
    blend.srcAlphaBlendFactor = blend.srcColorBlendFactor;
    blend.dstAlphaBlendFactor = blend.dstColorBlendFactor;
    blend.writeMask = 15;
    remixapi_InstanceInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    remixapi_InstanceIdentityEXT identity = {};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.pNext = &blend;
    identity.instanceId = group.id;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    info.pNext = &identity;
    info.mesh = group.mesh;
    info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC | (group.sky ? REMIXAPI_INSTANCE_CATEGORY_BIT_SKY : 0);
    info.transform.matrix[0][0] = info.transform.matrix[1][1] = info.transform.matrix[2][2] = 1;
    info.doubleSided = 1;
    // Paint is seen from its face's front only, and where its face is.
    remixapi_InstanceInfoCameraCullingEXT culling;
    cameraCulling(info, culling, group.noCull, group.skybox);
    draw(info);
  }
  for (auto &[key, group] : g_world.groups) {
    if (!group.mesh)
      continue;
    // The material's alpha state comes from the instance (useDrawCallAlphaState).
    remixapi_InstanceInfoBlendEXT blend = {};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.alphaTestEnabled = group.alphaTest;
    blend.alphaTestReferenceValue = group.alphaRef;
    blend.alphaTestCompareOp = group.alphaTest ? 6 : 7; // VK_COMPARE_OP_GREATER_OR_EQUAL : ALWAYS
    remixapi_InstanceIdentityEXT identity = {};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.pNext = &blend;
    identity.instanceId = group.id;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    remixapi_InstanceInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    // 3D skybox geometry is drawn by the sky camera, like the engine's.
    info.pNext = group.sky ? static_cast<void *>(&blend) : static_cast<void *>(&identity);
    info.mesh = group.mesh;
    info.categoryFlags = group.categories | (group.sky ? REMIXAPI_INSTANCE_CATEGORY_BIT_SKY : 0);
    info.transform.matrix[0][0] = info.transform.matrix[1][1] = info.transform.matrix[2][2] = 1;
    info.doubleSided = 1;
    remixapi_InstanceInfoCameraCullingEXT culling;
    cameraCulling(info, culling, group.noCull, group.skybox);
    const auto retained = draw(info);
    captureCharts(retained, group);
  }
  for (const auto &prop : g_world.props) {
    if (prop.skybox)
      continue;
    for (const auto &part : prop.model->parts) {
      const auto &m = g_world.materials[part.material];
      remixapi_InstanceInfoMaterialEXT material = {};
      material.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT;
      material.material = m.ref.handle;
      remixapi_InstanceInfoBlendEXT blend = {};
      blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      blend.pNext = m.ref.handle ? &material : nullptr;
      blend.alphaTestEnabled = m.alphaTest;
      blend.alphaTestReferenceValue = m.alphaRef;
      blend.alphaTestCompareOp = m.alphaTest ? 6 : 7;
      if (m.blended) {
        blend.alphaBlendEnabled = 1;
        blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6;
        blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7;
        blend.writeMask = 15;
        blend.tFactor = 0x00ffffffu | (uint32_t(m.opacity * 255 + .5f) << 24);
        blend.textureAlphaOperation = 3; // modulate texture alpha by constant opacity
        blend.textureAlphaArg1Source = 1;
        blend.textureAlphaArg2Source = 3;
      }
      remixapi_InstanceInfoReplacementHashEXT replacement = {};
      replacement.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_REPLACEMENT_HASH_EXT;
      replacement.pNext = &blend;
      replacement.meshHash = part.replacementHash;
      remixapi_InstanceIdentityEXT identity = {};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.pNext = &replacement;
      identity.instanceId = prop.id ^ part.replacementHash;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      identity.rasterVisible = 1;
      remixapi_InstanceInfo info = {};
      info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      info.pNext = &identity;
      info.mesh = part.mesh;
      info.categoryFlags = m.categories;
      std::memcpy(info.transform.matrix, prop.transform, sizeof(info.transform.matrix));
      info.doubleSided = m.blended ? m.noCull : 1;
      // (A prop is hidden per point like the map, not per leaf list as Source
      // draws it: see the runtime's VISIBILITY flag.)
      remixapi_InstanceInfoCameraCullingEXT culling;
      cameraCulling(info, culling, m.noCull, prop.skyTraced);
      draw(info);
    }
  }
  if (keep) {
    // Attempted once per version: a refused instance leaves the map to
    // DrawInstance (from the next frame; this one draws what was kept).
    g_world.sceneVersion = g_world.submitVersion;
    g_world.sceneValid = kept;
    ++g_world.recordings;
    drawScene(nullptr);
    if (!kept)
      dropSceneInstances();
  }
  if (record) {
    // Attempted once per version: a failed recording is not retried.
    g_world.batchVersion = g_world.submitVersion;
    g_world.batchValid = endRecording(kBatchId) == REMIXAPI_ERROR_CODE_SUCCESS;
    ++g_world.recordings;
  }
}

bool waterAt(const float eye[3], float color[3], float &distance) {
  if (!g_active || g_world.leafWater.empty())
    return false;
  int index = 0;
  for (int guard = 0; index >= 0 && guard < 4096; ++guard) {
    if (size_t(index) >= g_world.nodes.size() || size_t(g_world.nodes[index].plane) >= g_world.planes.size())
      return false;
    const auto &plane = g_world.planes[g_world.nodes[index].plane];
    index = g_world.nodes[index].children[dot(plane.normal, eye) - plane.dist >= 0 ? 0 : 1];
  }
  const int leaf = index < 0 ? -1 - index : -1;
  if (leaf < 0 || size_t(leaf) >= g_world.leafWater.size() || g_world.leafWater[size_t(leaf)] < 0)
    return false;
  const Material &m = g_world.materials[size_t(g_world.leafWater[size_t(leaf)])];
  std::memcpy(color, m.waterColor, sizeof(m.waterColor));
  distance = m.waterDistance;
  return distance > 0;
}

int areaAt(const float p[3]) {
  int index = 0;
  for (int guard = 0; index >= 0 && guard < 4096; ++guard) {
    if (size_t(index) >= g_world.nodes.size() || size_t(g_world.nodes[index].plane) >= g_world.planes.size())
      return -1;
    const auto &plane = g_world.planes[g_world.nodes[index].plane];
    index = g_world.nodes[index].children[dot(plane.normal, p) - plane.dist >= 0 ? 0 : 1];
  }
  const int leaf = index < 0 ? -1 - index : -1;
  return leaf >= 0 && size_t(leaf) < g_world.leafAreas.size() ? g_world.leafAreas[leaf] : -1;
}

bool isBakedBrushEntity(const float *a, const float *b, const float *c) {
  if (!g_active || g_world.lodCorners.empty())
    return false;
  for (const float *p : {a, b, c})
    if (!g_world.lodCorners.count(cornerKey(p)))
      return false;
  return true;
}

bool isStaticProp(const float *modelRows, const float eye[3]) {
  if (!g_active || g_world.props.empty())
    return false;
  // A view from inside the 3D skybox (its camera's area): the engine draws
  // the skybox's props; a placement it draws there is a skybox prop.
  // The eye's area changes only when the camera moves: one tree walk per view.
  static float lastEye[3] = {NAN, NAN, NAN};
  static bool lastSkyView = false;
  if (std::memcmp(lastEye, eye, sizeof(lastEye)) != 0) {
    std::memcpy(lastEye, eye, sizeof(lastEye));
    lastSkyView = g_world.skyArea >= 0 && areaAt(eye) == g_world.skyArea;
  }
  const bool skyView = lastSkyView;
  // A model is drawn as several consecutive draws with the same placement:
  // the last answer stands while placement, view and the props' skybox flags
  // are unchanged.
  static float lastRows[12] = {NAN};
  static bool lastSkyViewForRows = false, lastResult = false;
  static uint64_t lastVersion = ~0ull;
  if (std::memcmp(lastRows, modelRows, sizeof(lastRows)) == 0 && lastSkyViewForRows == skyView &&
      lastVersion == g_world.submitVersion)
    return lastResult;
  auto remember = [&](bool result) {
    std::memcpy(lastRows, modelRows, sizeof(lastRows));
    lastSkyViewForRows = skyView;
    lastVersion = g_world.submitVersion;
    return lastResult = result;
  };
  const float origin[3] = {modelRows[3], modelRows[7], modelRows[11]};
  // The origin's cell, and its neighbors only within the match tolerance of
  // a cell boundary.
  bool nearEdge = false;
  for (float v : origin) {
    const float f = v / 64 - std::floor(v / 64);
    nearEdge |= f * 64 < .5f || f * 64 > 63.5f;
  }
  uint64_t checked[8];
  int n = 0;
  for (int corner = 0; corner < (nearEdge ? 8 : 1); ++corner) {
    const float probe[3] = {origin[0] + (corner & 1 ? .5f : -.5f), origin[1] + (corner & 2 ? .5f : -.5f),
                            origin[2] + (corner & 4 ? .5f : -.5f)};
    const uint64_t cell = nearEdge ? propCell(probe) : propCell(origin);
    if (std::find(checked, checked + n, cell) != checked + n)
      continue;
    checked[n++] = cell;
    auto it = g_world.propCells.find(cell);
    if (it == g_world.propCells.end())
      continue;
    for (uint32_t index : it->second) {
      const auto &t = g_world.props[index].transform;
      bool same = true;
      for (int r = 0; r < 3 && same; ++r)
        for (int c = 0; c < 4 && same; ++c)
          same = std::fabs(t[r][c] - modelRows[r * 4 + c]) < (c == 3 ? .1f : 2e-3f);
      if (same) {
        if (skyView) {
          if (!g_world.props[index].skybox) {
            g_world.props[index].skybox = true;
            ++g_world.submitVersion;
          }
          return remember(false);
        }
        return remember(!g_world.props[index].skybox);
      }
    }
  }
  return remember(false);
}

const Occluders &occluders() {
  static const Occluders none;
  if (!g_active)
    return none;
  auto &o = g_world.occluders;
  if (g_world.occludersBuilt)
    return o;
  g_world.occludersBuilt = true;
  static uint64_t version = 0;
  o = Occluders{};
  o.version = ++version;
  auto add = [&](const float *positions, size_t vertexCount, size_t stride, const std::vector<uint32_t> &indices,
                 const float (*transform)[4]) {
    const uint32_t base = uint32_t(o.positions.size() / 3);
    for (size_t v = 0; v < vertexCount; ++v) {
      const float *p = positions + v * stride;
      if (transform)
        for (int r = 0; r < 3; ++r)
          o.positions.push_back(transform[r][0] * p[0] + transform[r][1] * p[1] + transform[r][2] * p[2] +
                                transform[r][3]);
      else
        o.positions.insert(o.positions.end(), {p[0], p[1], p[2]});
    }
    for (uint32_t i : indices)
      o.indices.push_back(base + i);
  };
  // Opaque surfaces only: alpha-tested ones (fences, foliage) and water let
  // light through.
  auto opaque = [](int material) {
    const auto &m = g_world.materials[size_t(material)];
    return !m.alphaTest && !m.owned;
  };
  static_assert(sizeof(remixapi_HardcodedVertex) % sizeof(float) == 0);
  for (const auto &[key, group] : g_world.groups) {
    if (group.sky)
      continue;
    for (const auto &[material, geometry] : group.surfaces)
      if (opaque(int(material)))
        add(geometry.vertices.front().position, geometry.vertices.size(),
            sizeof(remixapi_HardcodedVertex) / sizeof(float), geometry.indices, nullptr);
  }
  for (const auto &prop : g_world.props) {
    const float origin[3] = {prop.transform[0][3], prop.transform[1][3], prop.transform[2][3]};
    if (prop.skybox || (g_world.skyArea >= 0 && areaAt(origin) == g_world.skyArea))
      continue;
    for (const auto &part : prop.model->parts)
      if (part.material >= 0 && opaque(part.material) && !part.positions.empty())
        add(part.positions.data(), part.positions.size() / 3, 3, part.indices, prop.transform);
  }
  return o;
}

const std::vector<Light> &lights() {
  static const std::vector<Light> none;
  return g_active ? g_world.lights : none;
}

bool providesLight(int type, const float position[3], const float direction[3]) {
  if (!g_active)
    return false;
  for (const auto &l : g_world.lights) {
    // A position is one light whatever the engine calls it: it hands surface
    // (texture) lights to models as points or as spots, and a styled one's
    // colour changes every frame (kept as its own light, that copy was a new
    // light each frame).
    if ((l.type == 2) != (type == 2))
      continue;
    if (type == 2 ? dot(l.direction, direction) > 0.9999f
                  : std::fabs(l.position[0] - position[0]) < .01f && std::fabs(l.position[1] - position[1]) < .01f &&
                        std::fabs(l.position[2] - position[2]) < .01f)
      return true;
  }
  return false;
}

bool providesProp(const float *rows) {
  std::shared_ptr<const PropIndex> index;
  {
    std::lock_guard<std::mutex> lock(g_propIndexLock);
    index = g_propIndex;
  }
  if (!index || !rows)
    return false;
  const float origin[3] = {rows[3], rows[7], rows[11]};
  uint64_t checked[8];
  int n = 0;
  for (int corner = 0; corner < 8; ++corner) {
    const float probe[3] = {origin[0] + (corner & 1 ? .5f : -.5f), origin[1] + (corner & 2 ? .5f : -.5f),
                            origin[2] + (corner & 4 ? .5f : -.5f)};
    const uint64_t cell = propCell(probe);
    if (std::find(checked, checked + n, cell) != checked + n)
      continue;
    checked[n++] = cell;
    auto it = index->cells.find(cell);
    if (it == index->cells.end())
      continue;
    for (uint32_t i : it->second) {
      const auto &t = index->transforms[i];
      bool same = true;
      for (int k = 0; k < 12 && same; ++k)
        same = std::fabs(t[k] - rows[k]) < (k % 4 == 3 ? .1f : 2e-3f);
      if (same)
        return true;
    }
  }
  return false;
}

std::string materialName(uint64_t textureHash) {
  for (const auto &m : g_world.materials)
    for (uint64_t h : m.candidates)
      if (h == textureHash)
        return m.name;
  return {};
}

void dumpStatistics(FILE *out) {
  if (g_partition.result >= 0 || g_partition.clusters)
    fprintf(out, "   visibility partition: %u nodes, %u leaves, %u clusters -> %d\n", g_partition.nodes,
            g_partition.leaves, g_partition.clusters, g_partition.result);
  if (!g_active) {
    fprintf(out, "   BSP world: inactive\n");
    return;
  }
  uint32_t resolved = 0, meshes = 0;
  for (auto &m : g_world.materials)
    resolved += m.resolved;
  for (auto &[key, group] : g_world.groups)
    meshes += group.mesh != nullptr;
  fprintf(out,
          "   BSP world %s: faces %u displacements %u (sky %u) triangles %u | materials %u/%zu resolved | groups %zu "
          "(meshes %u) | left to the engine: translucent %u, ignored %u | rtx.conf-ignored brush materials kept %u | sky camera %d\n"
          "   BSP static props: %zu placements (%u in the 3D skybox), %zu models (%u failed), %u parts | overlays %u "
          "(%u triangles, %zu groups) | func_lod brushes %u (%u faces; left to the engine %u) | water materials %u | surface normal textures %u (animated %u; "
          "water optional via OPENREMIX_PROXY_WATER_NORMALS), water downward faces left out %u%s%s\n",

          g_world.map.c_str(), g_world.faces, g_world.displacements, g_world.skyFaces, g_world.triangles, resolved,
          g_world.materials.size(), g_world.groups.size(), meshes, g_world.skippedTranslucent, g_world.skippedIgnored, g_world.keptIgnored,
          int(g_world.skyCamera), g_world.props.size(), g_world.skyProps, g_world.propModels.size(),
          g_world.propModelsFailed, g_world.propParts, g_world.overlays, g_world.overlayTriangles,
          g_world.overlayGroups.size(), g_world.lodEntities, g_world.lodFaces, g_world.lodLeft, g_world.waterMaterials, g_world.surfaceNormalCount, g_world.surfaceNormalAnimated,

          g_world.waterDownFaces,
          g_world.surfaceNormalFailure.empty() ? "" : " | not loaded: ", g_world.surfaceNormalFailure.c_str());
  {
    const auto &l = g_world.layers;
    uint32_t meshes = 0, waiting = 0;
    for (auto &[key, group] : g_world.layerGroups)
      meshes += group.mesh != nullptr;
    for (auto &[base, layer] : g_world.secondLayers)
      waiting += layer.material >= 0 && !g_world.materials[layer.material].resolved;
    fprintf(out,
            "   BSP second layers ($basetexture2): %u materials on %u displacements (alpha %.0f..%.0f) | layered %u: "
            "triangles first texture %u, second texture %u, blended (decal sheet, vertex alpha) %u in %zu groups (%u "
            "meshes) | not layered: alpha 0 everywhere %u, second texture without a hash %u (%u materials) | second "
            "textures not uploaded yet %u | $blendmodulatetexture: %u by its mean per vertex, %u not read | 4-way "
            "blend materials (layers 3-4 not drawn) %u | on %u faces without a displacement (first texture only)\n%s",
            l.materials, l.displacements, l.displacements ? l.alphaMin : 0.0f, l.alphaMax, l.layered, l.baseTriangles,
            l.secondTriangles, l.blendedTriangles, g_world.layerGroups.size(), meshes, l.flat, l.withoutTexture,
            l.noHash, waiting, l.modulated, l.modulateUnread, l.fourWay, l.brushFaces, l.names.c_str());
  }
  {
    const auto &c = g_world.cull;
    fprintf(out,
            "   BSP camera culling (camera rays only; OPENREMIX_PROXY_BSP_CULL_BACKFACES / _PVS): instances %u: back "
            "faces hidden %u, unseen clusters hidden %u | two-sided ($nocull) %u, 3D skybox (always seen) %u\n"
            "   BSP orientation: faces wound clockwise seen from their plane's front %u, counter-clockwise (emitted "
            "reversed) %u, without area %u | faces with dface_t::side set (normal no longer flipped) %u | fan slivers "
            "wound against their face (kept) %u | displacement triangles clockwise from the base's front %u, overhangs "
            "%u | overlay polygons wound like their face %u, re-wound %u, facing behind their face %u | static prop "
            "triangles clockwise seen from their normals %u, counter-clockwise %u (strips mostly counter-clockwise %u)\n",
            c.instances, c.backFaces, c.visibility, c.twoSided, c.skybox, c.clockwise, c.counterClockwise, c.degenerate,
            c.sideFaces, c.slivers, c.dispClockwise, c.dispCounterClockwise, c.overlayKept, c.overlayRewound,
            c.overlayBehind, c.propAgainst, c.propAlong, c.propPartsAlong);
    // The runtime's view of it (last traced frame).
    static auto get = reinterpret_cast<PFN_remixapi_GetCameraCullingStatisticsEXT>(
        GetProcAddress(GetModuleHandleW(L"remix.dll"), "remixapi_GetCameraCullingStatisticsEXT"));
    remixapi_CameraCullingStatisticsEXT r = {};
    r.structSize = sizeof(r);
    if (get && get(&r) == REMIXAPI_ERROR_CODE_SUCCESS)
      fprintf(out,
              "   runtime camera culling: requested %u, active %u (1 back faces, 2 visibility) | partition %u | camera "
              "cluster %d (previous frame %d)%s | visible clusters %u of %u, leaves %u of %u | visible-set changes %llu\n",
              r.requested, r.active, r.partition, r.cameraCluster, r.previousCluster,
              r.outside ? ", OUTSIDE the world: everything is seen" : "", r.visibleClusters, r.clusterCount,
              r.visibleLeaves, r.leafCount, (unsigned long long)r.updates);
    else
      fprintf(out, "   runtime camera culling: not available (remixapi_GetCameraCullingStatisticsEXT missing)\n");
  }
  if (g_world.skyTraced)
    fprintf(out, "   3D skybox path traced: %u triangles, %u clipped at the map, %u props (in playable space: %u "
            "triangles, %u props; pieces on world faces %u), %zu clipped prop models, reach %.0f\n",
            g_world.skyTracedTriangles, g_world.skyClippedTriangles, g_world.skyTracedProps,
            g_world.skyStandInTriangles, g_world.skyStandInProps, g_world.skyCoplanarPieces,
            g_world.clippedSkyModels.size(), g_world.skyReach);
  {
    // Unresolved materials: no predictable texture hash (VTF missing or its
    // format not modelled) vs. a hash the engine never uploaded.
    uint32_t noCandidates = 0, notUploaded = 0;
    std::string examples;
    for (const auto &m : g_world.materials) {
      if (m.resolved)
        continue;
      ++(m.candidates.empty() ? noCandidates : notUploaded);
      if (examples.size() < 600) {
        KeyValues kv;
        std::string base = loadMaterial(m.name, kv) && kv.values.count("$basetexture") ? kv.values["$basetexture"] : "-";
        std::vector<uint8_t> header;
        int32_t format = -1;
        if (base != "-" && fs::read(vtfPath(base), header, 0, 64) && header.size() >= 56)

          std::memcpy(&format, header.data() + 52, 4);
        examples += "      " + m.name + " (" + kv.shader + ", $basetexture " + base + ", vtf format " +
                    std::to_string(format) + (m.candidates.empty() ? ", no hash" : ", not uploaded") + ")\n";
      }
    }
    if (noCandidates + notUploaded)
      fprintf(out, "   BSP materials unresolved: %u without a texture hash, %u not uploaded\n%s", noCandidates,
              notUploaded, examples.c_str());
  }
  {
    using engine_materials::Shading;
    uint32_t modes[5] = {}, luminous = 0, waiting = 0, dropped = 0;
    std::string examples;
    for (const auto &m : g_world.materials) {
      if (m.shading.mode == Shading::Lit)
        continue;
      ++modes[m.shading.mode];
      luminous += m.shading.luminous;
      waiting += !m.resolved;
      dropped += m.maskDropped;
      if (m.shading.mode != Shading::Unlit && examples.size() < 700) {
        char line[320];
        std::snprintf(line, sizeof(line), "      %s: %s%s%s, tint %.2f %.2f %.2f x %.2f%s\n", m.name.c_str(),
                      m.shading.mode == Shading::SelfIllumBaseAlpha ? "base alpha"
                      : m.shading.mode == Shading::SelfIllumMask    ? "mask "
                                                                    : "envmap mask alpha ",
                      m.shading.mask.c_str(), m.maskCandidates.empty() && !m.shading.mask.empty() ? " (no texture hash)" : "",
                      m.shading.tint[0], m.shading.tint[1], m.shading.tint[2], m.shading.maskScale,
                      m.maskDropped ? " -> lit (mask not uploaded)" : m.resolved ? "" : " (texture not uploaded yet)");
        examples += line;
      }
    }
    fprintf(out,
            "   BSP emissive materials: unlit %u (%u marked $selfillum: lighting the scene), self-illuminated: base "
            "alpha %u, $selfillummask %u, envmap mask alpha %u | waiting for textures %u, mask never uploaded (lit) "
            "%u\n%s",
            modes[Shading::Unlit], luminous, modes[Shading::SelfIllumBaseAlpha], modes[Shading::SelfIllumMask],
            modes[Shading::SelfIllumMaskAlpha], waiting, dropped, examples.c_str());
  }
  fprintf(out, "   BSP world batch: %u recordings, valid %d (scene instances %zu), replayed frames %llu of %llu\n",
          g_world.recordings, int(g_world.batchValid || g_world.sceneValid), g_world.sceneInstances.size(), (unsigned long long)g_world.replayedFrames, (unsigned long long)g_world.frames);
  fprintf(out, "   BSP bake inputs: %s; status %d, %zu authored instances, %zu surface charts\n",
          g_world.bakeReason, g_world.bakeStatus, g_world.bakeInstances.size(), g_world.bakeCharts.size());
  fprintf(out, "   BSP lights: %zu static, %u of them styled, %u surface (texture) lights (left to the engine: styled %u, 3D skybox %u, other types %u)\n",
          g_world.lights.size(), g_world.styleDrivenLights, g_world.surfaceLights, g_world.styledLights,
          g_world.skyboxLights, g_world.otherLights);
  fprintf(out, "   BSP authored reflection seeds: %zu (validated playable env_cubemap positions)\n",
          g_world.reflectionSeeds.size());
  size_t captureReferences = 0, authoredMatches = 0;
  for (const auto &material : g_world.materials) {
    if (!material.captureReference) continue;
    ++captureReferences;
    if (const auto document = g_cubemapOverrides.accepted();
        document && cubemaps::uniqueVolume(*material.captureReference, *document)) ++authoredMatches;
  }
  fprintf(out, "   BSP explicit material capture references: %zu; unique accepted-sidecar matches: %zu (binding opt-in)\n",
          captureReferences, authoredMatches);
  for (const auto &[key, model] : g_world.propModels)
    if (model.engineParts)
      fprintf(out, "     deferred prop transport: %s; %u additive camera-only parts; missing occluders %u\n",
              key.first.c_str(), model.additiveEngineParts, unsigned(model.missingBakeOccluders));
  for (const auto &[key, model] : g_world.propModels)
    for (const auto &material : model.engineMaterials)
      fprintf(out, "     prop material left to the engine: %s (%s)\n", material.c_str(), key.first.c_str());
  for (auto &path : g_world.failedModels)
    fprintf(out, "     prop model left to the engine: %s\n", path.c_str());
  for (auto &m : g_world.materials)
    if (!m.resolved)
      fprintf(out, "     unresolved material %s (%zu texture hashes): %s\n", m.name.c_str(), m.candidates.size(),
              m.importFailure.c_str());
}

} // namespace proxy::bsp
