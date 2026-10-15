#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libtypesafe/result.hpp"

namespace libtypesafe {

// Images ("Dreamer-ready" on TypeSafe's side: formats, limits and accuracy may change).
//
// List each image once in `CallOptions::assets`, keyed by a placeholder string (a UUID is
// recommended; `new_placeholder()` makes one), and write the placeholder wherever the image
// belongs: as a state value, inside instructions, or in Choice/Score/Noul criteria. Several
// frames go in the state as an array, oldest first. The request is sent as multipart/form-data
// (the JSON as part `request`, each image as its own file part, as the Python SDK sends it), and
// every request with an asset or a Box question carries `TypeSafe-Danger-Zone-Images: true`.

namespace image_limits {
inline constexpr int min_side = 256;
inline constexpr int max_side = 1568;
inline constexpr std::int64_t min_area = 78'400;
inline constexpr std::int64_t max_area = 2'458'624;
inline constexpr std::size_t max_file_bytes = 16u * 1024 * 1024;
inline constexpr std::size_t max_request_bytes = 32u * 1024 * 1024;
}  // namespace image_limits

/// The header image and Box requests need, with the value "true".
inline constexpr std::string_view images_header = "TypeSafe-Danger-Zone-Images";

/// What `inspect_image` read from an image file.
struct ImageInfo {
  /// "image/png" or "image/jpeg".
  std::string content_type;
  int width = 0;
  int height = 0;
  /// A JPEG's EXIF orientation when it isn't 1 (upright). TypeSafe ignores it: rotate the pixels.
  std::optional<int> exif_orientation;
  /// A progressive (non-baseline) JPEG. TypeSafe recommends baseline.
  bool progressive = false;
};

/// Check an image against TypeSafe's limits: PNG or JPEG still (no animated PNG), at most 16 MiB,
/// each side 256-1568 px, area 78,400-2,458,624 px. Images are rejected, not resized, so this
/// fails with an invalid_argument Error saying what to fix.
[[nodiscard]] Result<ImageInfo> inspect_image(const std::uint8_t* data, std::size_t size);
[[nodiscard]] inline Result<ImageInfo> inspect_image(const std::vector<std::uint8_t>& bytes) {
  return inspect_image(bytes.data(), bytes.size());
}

/// A fresh random placeholder: a version 4 UUID, e.g. "6e1d0b7c-...".
[[nodiscard]] std::string new_placeholder();

/// The images for one request, keyed by placeholder, in the order added. Problems (a bad image,
/// a repeated placeholder, over 32 MiB in total) are kept and reported by the call, before
/// anything is sent, like `Questions`.
class Assets {
 public:
  struct Image {
    std::string placeholder;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    ImageInfo info;
  };

  /// Add an image file's bytes (copied once and shared by copies of this Assets).
  void add_image(std::string placeholder, std::vector<std::uint8_t> bytes);
  /// Add bytes already shared, without copying.
  void add_image(std::string placeholder, std::shared_ptr<const std::vector<std::uint8_t>> bytes);

  [[nodiscard]] bool empty() const noexcept { return images_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return images_.size(); }
  [[nodiscard]] const std::vector<Image>& images() const noexcept { return images_; }
  [[nodiscard]] std::size_t total_bytes() const noexcept { return total_; }
  [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }

 private:
  void fail(std::string message);
  std::vector<Image> images_;
  std::size_t total_ = 0;
  std::optional<Error> error_;
};

}  // namespace libtypesafe
