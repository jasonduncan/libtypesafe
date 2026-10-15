// Images (multipart assets), Box questions and answers, and the images header. Offline.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "fakes.hpp"
#include "imagekit.hpp"
#include "wire.hpp"

using namespace fakes;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

/// Parts of a multipart body by name: {content type, content}.
std::map<std::string, std::pair<std::string, std::string>> parts(const ts::HttpRequest& request) {
  const std::string type(*request.headers.get("Content-Type"));
  const std::string boundary = "--" + type.substr(type.find("boundary=") + 9);
  std::map<std::string, std::pair<std::string, std::string>> out;
  std::size_t at = request.body.find(boundary);
  while (true) {
    const std::size_t start = at + boundary.size();
    if (request.body.compare(start, 2, "--") == 0) break;
    const std::size_t next = request.body.find(boundary, start);
    const std::string part = request.body.substr(start + 2, next - start - 4);  // skip \r\n, drop \r\n
    const std::size_t head_end = part.find("\r\n\r\n");
    const std::string head = part.substr(0, head_end);
    const std::size_t n = head.find("name=\"") + 6;
    const std::string name = head.substr(n, head.find('"', n) - n);
    const std::size_t t = head.find("Content-Type: ") + 14;
    out[name] = {head.substr(t), part.substr(head_end + 4)};
    at = next;
  }
  return out;
}

std::string str(const imagekit::Bytes& b) { return std::string(b.begin(), b.end()); }

}  // namespace

TEST_CASE("inspect_image enforces TypeSafe's limits") {
  auto png = ts::inspect_image(imagekit::png(300, 400));
  REQUIRE(png);
  CHECK(png->content_type == "image/png");
  CHECK((png->width == 300 && png->height == 400));
  auto jpeg = ts::inspect_image(imagekit::jpeg(1568, 1568, 0xC2, 6));
  REQUIRE(jpeg);
  CHECK(jpeg->content_type == "image/jpeg");
  CHECK(jpeg->progressive);
  CHECK(jpeg->exif_orientation == 6);

  const auto rejects = [](const imagekit::Bytes& bytes, const char* message) {
    auto info = ts::inspect_image(bytes);
    REQUIRE_FALSE(info);
    CHECK(info.error().kind == ts::ErrorKind::invalid_argument);
    CHECK_THAT(info.error().message, ContainsSubstring(message));
  };
  rejects({'G', 'I', 'F', '8', '9', 'a', 0, 0}, "PNG or JPEG");
  rejects(imagekit::png(255, 400), "width is 255 px");
  rejects(imagekit::png(256, 256), "area is 65536");
  rejects(imagekit::png(280, 280, true), "animated");
  rejects(imagekit::jpeg(300, 1600), "height is 1600 px");
  auto big = imagekit::png();
  big.resize(ts::image_limits::max_file_bytes + 1);
  rejects(big, "16 MiB");
}

TEST_CASE("placeholders are v4 UUIDs") {
  const std::string p = ts::new_placeholder();
  CHECK(p.size() == 36);
  CHECK(p[14] == '4');
  CHECK(p != ts::new_placeholder());
}

TEST_CASE("images are sent as multipart file parts, as the Python SDK sends them") {
  Harness h;
  const auto photo = imagekit::png();
  const auto logo = imagekit::png(300, 300);
  ts::CallOptions call;
  call.model = "jev-1.14.0";
  call.assets.add_image("6e1d0b7c-kitchen", photo);
  call.assets.add_image("logo-ph", logo);
  ts::Questions q;
  auto mug = q.add("mug", ts::box("the coffee mug in `photo`"));
  auto brand = q.add("brand", ts::choice("Which brand?", {{"acme", "this logo: logo-ph"}, {"other", nullptr}}));
  ts::Json state = {{"photo", "6e1d0b7c-kitchen"}};
  auto pending = h.client.system_one_async(state, q, call);

  REQUIRE(h.transport->attempts.size() == 1);
  const auto& request = h.transport->attempts[0].request;
  CHECK(request.headers.get("TypeSafe-Danger-Zone-Images") == "true");
  CHECK_THAT(std::string(*request.headers.get("Content-Type")), StartsWith("multipart/form-data; boundary="));
  auto p = parts(request);
  REQUIRE(p.size() == 3);
  CHECK(p["request"].first == "application/json");
  CHECK(p["request"].second ==
        R"({"state":{"photo":"6e1d0b7c-kitchen"},"model":"jev-1.14.0","questions":{"mug":{"type":"box","instructions":"the coffee mug in `photo`"},"brand":{"type":"choice","instructions":"Which brand?","criteria":{"acme":"this logo: logo-ph","other":null}}},"assets":{"6e1d0b7c-kitchen":{"type":"image","part":"asset-0"},"logo-ph":{"type":"image","part":"asset-1"}}})");
  CHECK(p["asset-0"] == std::pair<std::string, std::string>{"image/png", str(photo)});
  CHECK(p["asset-1"].second == str(logo));
  CHECK_THAT(request.body, ContainsSubstring(R"(name="asset-0"; filename="asset-0")"));
  CHECK(h.logged("(+2 image parts"));  // debug log shows the JSON, not the bytes

  h.transport->complete(0, json_response(200, {{"model", "jev-1.14.0"},
                                               {"answers",
                                                {{"mug", {{"type", "box"}, {"box", {{"top", 0.25}, {"bottom", 0.82}, {"left", 0.44}, {"right", 0.84}}}, {"confidence", 0.92}}},
                                                 {"brand", {{"type", "choice"}, {"choice", "acme"}, {"confidence", 0.8}, {"probabilities", {{"acme", 0.8}, {"other", 0.2}}}}}}},
                                               {"usage", {{"input_tokens", 500}, {"output_tokens", 30}}}}));
  h.client.pump();
  auto result = pending.take();
  REQUIRE(result);
  CHECK((*result)[mug].box.top == 0.25);
  CHECK((*result)[mug].box.right == 0.84);
  CHECK((*result)[mug].confidence == 0.92);
  CHECK((*result)[brand].choice == "acme");
  CHECK_FALSE(h.logged("unrecognized type"));
}

TEST_CASE("multi-frame state is an array of frames, oldest first") {
  Harness h;
  ts::CallOptions call;
  call.assets.add_image("f0", imagekit::png());
  call.assets.add_image("f1", imagekit::png());
  ts::Questions q;
  q.add("opened", ts::noul("Did the door open during `frames`?"));
  ts::Json state = {{"frames", {{{"seconds_ago", 2}, {"frame", "f0"}}, {{"seconds_ago", 0}, {"frame", "f1"}}}}};
  auto pending = h.client.system_one_async(state, q, call);
  auto p = parts(h.transport->attempts.at(0).request);
  CHECK_THAT(p["request"].second, StartsWith(R"({"state":{"frames":[{"seconds_ago":2,"frame":"f0"},{"seconds_ago":0,"frame":"f1"}]})"));
}

TEST_CASE("a Box question alone turns the images header on; text-only calls don't") {
  Harness h;
  ts::Questions boxed;
  boxed.add("mug", ts::box("the mug"));
  auto a = h.client.system_one_async("s", boxed);
  CHECK(h.transport->attempts.at(0).request.headers.get("TypeSafe-Danger-Zone-Images") == "true");
  CHECK(h.transport->attempts.at(0).request.headers.get("Content-Type") == "application/json");
  ts::Questions plain;
  plain.add("q", ts::noul("Is it?"));
  auto b = h.client.system_one_async("s", plain);
  CHECK_FALSE(h.transport->attempts.at(1).request.headers.contains("TypeSafe-Danger-Zone-Images"));
}

TEST_CASE("base64 assets through extra_body are sent as JSON with the header") {
  Harness h;
  ts::CallOptions call;
  call.extra_body = {{"assets", {{"ph", {{"type", "image"}, {"format", "png"}, {"data_base64", "AAAA"}}}}}};
  call.assets.add_image("ignored", imagekit::png());  // extra_body wins, as in the Python SDK
  ts::Questions q;
  q.add("q", ts::noul("Is ph red?"));
  auto pending = h.client.system_one_async("ph", q, call);
  const auto& request = h.transport->attempts.at(0).request;
  CHECK(request.headers.get("Content-Type") == "application/json");
  CHECK(request.headers.get("TypeSafe-Danger-Zone-Images") == "true");
  CHECK(ts::Json::parse(request.body)["assets"]["ph"]["format"] == "png");
}

TEST_CASE("bad assets fail before sending") {
  Harness h;
  ts::CallOptions call;
  call.assets.add_image("ph", imagekit::png(100, 100));
  ts::Questions q;
  q.add("q", ts::noul("Is it?"));
  auto pending = h.client.system_one_async("ph", q, call);
  h.client.pump();
  auto result = pending.take();
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == ts::ErrorKind::invalid_argument);
  CHECK_THAT(result.error().message, ContainsSubstring("Asset \"ph\": image width is 100 px"));
  CHECK(h.transport->attempts.empty());

  ts::Assets twice;
  twice.add_image("x", imagekit::png());
  twice.add_image("x", imagekit::png());
  REQUIRE(twice.error());
  CHECK_THAT(twice.error()->message, ContainsSubstring("Duplicate"));
}

TEST_CASE("box answers are checked") {
  ts::Questions q;
  q.add("mug", ts::box("the mug"));
  const auto decode = [&](const ts::Json& answer) {
    return ts::detail::Wire::decode_system_one(json_response(200, ok_body({{"mug", answer}})), q, {});
  };
  CHECK(decode({{"type", "box"}, {"box", {{"top", 0.1}, {"bottom", 0.2}, {"left", 0}, {"right", 1}}}, {"confidence", nullptr}}));
  CHECK_FALSE(decode({{"type", "box"}, {"box", {{"top", 0.9}, {"bottom", 0.2}, {"left", 0}, {"right", 1}}}}));
  CHECK_FALSE(decode({{"type", "box"}, {"box", {{"top", 0.1}, {"bottom", 1.2}, {"left", 0}, {"right", 1}}}}));
  CHECK_FALSE(decode(noul_answer(0.5)));  // wrong type for a Box question
  ts::Questions empty_box;
  empty_box.add("b", ts::box(nullptr));
  CHECK(empty_box.error());
}
