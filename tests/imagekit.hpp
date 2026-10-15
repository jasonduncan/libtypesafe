// Tiny PNG/JPEG builders for tests. The PNG is real (stored deflate blocks); the JPEG has just
// enough structure to read its size.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace imagekit {

using Bytes = std::vector<std::uint8_t>;

inline void be32(Bytes& out, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(std::uint8_t(v >> s));
}
inline void be16(Bytes& out, std::uint32_t v) {
  out.push_back(std::uint8_t(v >> 8));
  out.push_back(std::uint8_t(v));
}

inline std::uint32_t crc32(const Bytes& data, std::size_t from) {
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::size_t i = from; i < data.size(); ++i) {
    c ^= data[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

inline void chunk(Bytes& out, const char* kind, const Bytes& data) {
  be32(out, std::uint32_t(data.size()));
  const std::size_t start = out.size();
  out.insert(out.end(), kind, kind + 4);
  out.insert(out.end(), data.begin(), data.end());
  be32(out, crc32(out, start));
}

/// A decodable 8-bit grayscale PNG. `dark` paints a black rectangle (x0, y0, x1, y1) on white.
inline Bytes png(int width = 280, int height = 280, bool animated = false,
                 std::optional<std::vector<int>> dark = std::nullopt) {
  Bytes raw;
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    for (int x = 0; x < width; ++x) {
      const bool in = dark && x >= (*dark)[0] && y >= (*dark)[1] && x < (*dark)[2] && y < (*dark)[3];
      raw.push_back(in ? 0 : (dark ? 255 : 128));
    }
  }
  // zlib with stored blocks.
  Bytes z = {0x78, 0x01};
  for (std::size_t at = 0; at < raw.size(); at += 65535) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size() ? 1 : 0);
    z.push_back(std::uint8_t(n));
    z.push_back(std::uint8_t(n >> 8));
    z.push_back(std::uint8_t(~n));
    z.push_back(std::uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
  }
  std::uint32_t a = 1, b = 0;
  for (auto byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  be32(z, (b << 16) | a);

  Bytes out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  Bytes ihdr;
  be32(ihdr, std::uint32_t(width));
  be32(ihdr, std::uint32_t(height));
  ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0});
  chunk(out, "IHDR", ihdr);
  if (animated) {
    Bytes actl;
    be32(actl, 2);
    be32(actl, 0);
    chunk(out, "acTL", actl);
  }
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  return out;
}

inline Bytes jpeg(int width = 300, int height = 300, std::uint8_t sof = 0xC0, std::optional<int> orientation = std::nullopt) {
  Bytes out = {0xFF, 0xD8};
  if (orientation) {
    Bytes seg = {'E', 'x', 'i', 'f', 0, 0, 'M', 'M', 0, 0x2A, 0, 0, 0, 8};
    be16(seg, 1);
    be16(seg, 0x0112);
    be16(seg, 3);
    be32(seg, 1);
    be16(seg, std::uint32_t(*orientation));
    be16(seg, 0);
    be32(seg, 0);
    out.insert(out.end(), {0xFF, 0xE1});
    be16(out, std::uint32_t(seg.size() + 2));
    out.insert(out.end(), seg.begin(), seg.end());
  }
  out.insert(out.end(), {0xFF, sof});
  be16(out, 11);
  out.push_back(8);
  be16(out, std::uint32_t(height));
  be16(out, std::uint32_t(width));
  out.insert(out.end(), {1, 1, 0x11, 0});
  out.insert(out.end(), {0xFF, 0xD9});
  return out;
}

}  // namespace imagekit
