#include "libtypesafe/assets.hpp"

#include <algorithm>
#include <random>

namespace libtypesafe {

namespace {

using Bytes = std::vector<std::uint8_t>;

Error bad(std::string message) {
  Error error;
  error.kind = ErrorKind::invalid_argument;
  error.message = std::move(message);
  return error;
}

std::uint32_t be32(const std::uint8_t* b) {
  return (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | b[3];
}
std::uint32_t be16(const std::uint8_t* b) { return (std::uint32_t(b[0]) << 8) | b[1]; }

std::optional<Error> read_png(const std::uint8_t* b, std::size_t n, ImageInfo& info) {
  if (n < 33 || !std::equal(b + 12, b + 16, "IHDR")) return bad("malformed PNG: no IHDR header");
  info.width = int(std::min<std::uint32_t>(be32(b + 16), 1u << 30));
  info.height = int(std::min<std::uint32_t>(be32(b + 20), 1u << 30));
  std::size_t at = 8;
  while (at + 8 <= n) {
    const std::uint32_t length = be32(b + at);
    if (std::equal(b + at + 4, b + at + 8, "acTL")) return bad("animated PNGs are not accepted; send a still frame");
    if (std::equal(b + at + 4, b + at + 8, "IDAT") || std::equal(b + at + 4, b + at + 8, "IEND")) break;
    at += 12 + std::size_t(length);
  }
  return std::nullopt;
}

std::optional<int> exif_orientation(const std::uint8_t* b, std::size_t start, std::size_t end) {
  if (end < start + 8) return std::nullopt;
  const bool little = b[start] == 'I' && b[start + 1] == 'I';
  if (!little && !(b[start] == 'M' && b[start + 1] == 'M')) return std::nullopt;
  const auto u16 = [&](std::size_t at) -> std::uint32_t {
    return little ? std::uint32_t(b[at] | (b[at + 1] << 8)) : std::uint32_t((b[at] << 8) | b[at + 1]);
  };
  const auto u32 = [&](std::size_t at) -> std::uint32_t {
    return little ? (u16(at) | (u16(at + 2) << 16)) : ((u16(at) << 16) | u16(at + 2));
  };
  const std::size_t ifd = start + u32(start + 4);
  if (ifd < start || ifd + 2 > end) return std::nullopt;
  const std::uint32_t count = u16(ifd);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t entry = ifd + 2 + 12 * std::size_t(i);
    if (entry + 12 > end) return std::nullopt;
    if (u16(entry) == 0x0112) return int(u16(entry + 8));
  }
  return std::nullopt;
}

std::optional<Error> read_jpeg(const std::uint8_t* b, std::size_t n, ImageInfo& info) {
  std::size_t at = 2;
  while (at + 4 <= n) {
    if (b[at] != 0xFF) return bad("malformed JPEG: bad marker");
    const std::uint8_t marker = b[at + 1];
    if (marker == 0xFF) {
      ++at;
      continue;
    }
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      at += 2;
      continue;
    }
    const std::size_t length = be16(b + at + 2);
    const std::size_t body = at + 4;
    const std::size_t end = std::min(n, at + 2 + length);
    if (marker == 0xE1 && end >= body + 6 && std::equal(b + body, b + body + 6, "Exif\0\0")) {
      const auto o = exif_orientation(b, body + 6, end);
      if (o && *o != 1) info.exif_orientation = o;
    }
    if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
      if (end < body + 5) break;
      info.height = int(be16(b + body + 1));
      info.width = int(be16(b + body + 3));
      info.progressive = marker != 0xC0 && marker != 0xC1;
      return std::nullopt;
    }
    if (marker == 0xDA) break;
    at += 2 + length;
  }
  return bad("malformed JPEG: no frame header with its size");
}

std::string mib(std::size_t bytes) {
  const auto tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MiB";
}

}  // namespace

Result<ImageInfo> inspect_image(const std::uint8_t* b, std::size_t n) {
  namespace lim = image_limits;
  if (n > lim::max_file_bytes) return bad("image is " + mib(n) + "; the limit is 16 MiB per file");
  ImageInfo info;
  static const std::uint8_t png_sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (n >= 8 && std::equal(std::begin(png_sig), std::end(png_sig), b)) {
    info.content_type = "image/png";
    if (auto e = read_png(b, n, info)) return *e;
  } else if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
    info.content_type = "image/jpeg";
    if (auto e = read_jpeg(b, n, info)) return *e;
  } else {
    return bad("image must be PNG or JPEG (unrecognized file signature)");
  }
  for (const auto& [side, px] : {std::pair<const char*, int>{"width", info.width}, {"height", info.height}}) {
    if (px < lim::min_side || px > lim::max_side) {
      return bad(std::string("image ") + side + " is " + std::to_string(px) +
                 " px; each side must be 256-1568 px (images are rejected, not resized: resize it first)");
    }
  }
  const std::int64_t area = std::int64_t(info.width) * info.height;
  if (area < lim::min_area || area > lim::max_area) {
    return bad("image area is " + std::to_string(area) + " px; it must be 78400-2458624 px");
  }
  return info;
}

std::string new_placeholder() {
  static thread_local std::mt19937_64 rng{(std::uint64_t(std::random_device{}()) << 32) ^ std::random_device{}()};
  std::uint8_t u[16];
  for (int i = 0; i < 16; i += 8) {
    const std::uint64_t r = rng();
    for (int j = 0; j < 8; ++j) u[i + j] = std::uint8_t(r >> (8 * j));
  }
  u[6] = std::uint8_t((u[6] & 0x0F) | 0x40);
  u[8] = std::uint8_t((u[8] & 0x3F) | 0x80);
  static const char* hex = "0123456789abcdef";
  std::string text;
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
    text += hex[u[i] >> 4];
    text += hex[u[i] & 15];
  }
  return text;
}

void Assets::fail(std::string message) {
  if (!error_) error_ = bad(std::move(message));
}

void Assets::add_image(std::string placeholder, Bytes bytes) {
  add_image(std::move(placeholder), std::make_shared<const Bytes>(std::move(bytes)));
}

void Assets::add_image(std::string placeholder, std::shared_ptr<const Bytes> bytes) {
  if (placeholder.empty() || placeholder.find_first_not_of(" \t\r\n") == std::string::npos) {
    return fail("Asset placeholders must be non-empty strings.");
  }
  for (const auto& image : images_) {
    if (image.placeholder == placeholder) return fail("Duplicate asset placeholder \"" + placeholder + "\".");
  }
  if (!bytes) return fail("Asset \"" + placeholder + "\" has no data.");
  auto info = inspect_image(*bytes);
  if (!info) return fail("Asset \"" + placeholder + "\": " + info.error().message);
  total_ += bytes->size();
  if (total_ > image_limits::max_request_bytes) {
    return fail("Images in one request must total at most 32 MiB (got " + mib(total_) + ").");
  }
  images_.push_back({std::move(placeholder), std::move(bytes), std::move(*info)});
}

}  // namespace libtypesafe
