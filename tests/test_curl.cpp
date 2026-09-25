// The libcurl transport against a local HTTP server on 127.0.0.1. No external network.
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <httplib.h>

#include <libtypesafe/libtypesafe.hpp>

#include "fakes.hpp"

namespace ts = libtypesafe;
using namespace std::chrono_literals;

namespace {

struct TestServer {
  httplib::Server server;
  int port = 0;
  std::thread thread;

  void start() {
    port = server.bind_to_any_port("127.0.0.1");
    thread = std::thread([this] { server.listen_after_bind(); });
    server.wait_until_ready();
  }
  ~TestServer() {
    server.stop();
    if (thread.joinable()) thread.join();
  }
  std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

/// Drive a transport until `done` fires or `limit` passes.
std::optional<ts::TransportOutcome> run(ts::Transport& transport, ts::HttpRequest request,
                                        std::chrono::milliseconds limit = 5s) {
  std::optional<ts::TransportOutcome> outcome;
  transport.start(std::move(request), [&](ts::TransportOutcome o) { outcome = std::move(o); });
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!outcome && std::chrono::steady_clock::now() < deadline) {
    transport.poll();
    if (!outcome) transport.wait(20ms);
  }
  return outcome;
}

ts::HttpRequest post(std::string url, std::chrono::milliseconds timeout = 5s) {
  ts::HttpRequest request;
  request.method = "POST";
  request.url = std::move(url);
  request.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer k"}};
  request.body = R"({"hello":"world"})";
  request.timeout = timeout;
  return request;
}

}  // namespace

TEST_CASE("curl: POST round trip with headers and body") {
  TestServer s;
  std::string seen_body, seen_auth, seen_expect = "unset";
  s.server.Post("/echo", [&](const httplib::Request& req, httplib::Response& res) {
    seen_body = req.body;
    seen_auth = req.get_header_value("Authorization");
    if (req.has_header("Expect")) seen_expect = req.get_header_value("Expect");
    res.set_header("x-typesafe-request-id", "local-1");
    res.set_content(R"({"ok":true})", "application/json");
  });
  s.start();

  auto transport = ts::make_curl_transport();
  auto outcome = run(*transport, post(s.url("/echo")));
  REQUIRE(outcome);
  auto* response = std::get_if<ts::HttpResponse>(&*outcome);
  REQUIRE(response != nullptr);
  CHECK(response->status == 200);
  CHECK(response->body == R"({"ok":true})");
  CHECK(response->headers.get("X-TypeSafe-Request-Id") == "local-1");
  CHECK(seen_body == R"({"hello":"world"})");
  CHECK(seen_auth == "Bearer k");
  CHECK(seen_expect == "unset");
}

TEST_CASE("curl: non-2xx responses are responses, not errors") {
  TestServer s;
  s.server.Get("/missing", [](const httplib::Request&, httplib::Response& res) {
    res.status = 404;
    res.set_content(R"({"detail":"Not Found"})", "application/json");
  });
  s.start();
  auto transport = ts::make_curl_transport();
  ts::HttpRequest request;
  request.method = "GET";
  request.url = s.url("/missing");
  auto outcome = run(*transport, request);
  REQUIRE(outcome);
  CHECK(std::get<ts::HttpResponse>(*outcome).status == 404);
}

TEST_CASE("curl: a slow response times out after the request was sent") {
  TestServer s;
  s.server.Post("/slow", [](const httplib::Request&, httplib::Response& res) {
    std::this_thread::sleep_for(800ms);
    res.set_content("{}", "application/json");
  });
  s.start();
  auto transport = ts::make_curl_transport();
  auto outcome = run(*transport, post(s.url("/slow"), 200ms));
  REQUIRE(outcome);
  auto* error = std::get_if<ts::TransportError>(&*outcome);
  REQUIRE(error != nullptr);
  CHECK(error->kind == ts::TransportError::Kind::timeout);
  CHECK(error->request_sent);
}

#ifndef _WIN32
TEST_CASE("curl: connection refused never sent the request") {
  // Bind a port without listening, then close it: connections to it are refused.
  int port = 0;
  {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
    socklen_t length = sizeof address;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    port = ntohs(address.sin_port);
    ::close(fd);
  }
  auto transport = ts::make_curl_transport();
  auto outcome = run(*transport, post("http://127.0.0.1:" + std::to_string(port) + "/x"));
  REQUIRE(outcome);
  auto* error = std::get_if<ts::TransportError>(&*outcome);
  REQUIRE(error != nullptr);
  CHECK(error->kind == ts::TransportError::Kind::connection);
  CHECK_FALSE(error->request_sent);
}
#endif

TEST_CASE("curl: cancel mid-transfer reports cancelled") {
  TestServer s;
  s.server.Post("/slow", [](const httplib::Request&, httplib::Response& res) {
    std::this_thread::sleep_for(500ms);
    res.set_content("{}", "application/json");
  });
  s.start();
  auto transport = ts::make_curl_transport();
  std::optional<ts::TransportOutcome> outcome;
  const auto id = transport->start(post(s.url("/slow")), [&](ts::TransportOutcome o) { outcome = std::move(o); });
  for (int i = 0; i < 5; ++i) {
    transport->poll();
    transport->wait(20ms);
  }
  REQUIRE_FALSE(outcome);
  transport->cancel(id);
  REQUIRE(outcome);
  CHECK(std::get<ts::TransportError>(*outcome).kind == ts::TransportError::Kind::cancelled);
}

TEST_CASE("client over curl: retries a 503 with retry-after-ms, then succeeds") {
  TestServer s;
  std::mutex mutex;
  std::vector<std::string> retry_counts;
  s.server.Post("/v1/systemone", [&](const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lock(mutex);
    retry_counts.push_back(req.get_header_value("X-TypeSafe-Retry-Count"));
    if (retry_counts.size() == 1) {
      res.status = 503;
      res.set_header("retry-after-ms", "10");
      res.set_content(R"({"error":"busy"})", "application/json");
      return;
    }
    res.set_header("x-typesafe-request-id", "local-2");
    res.set_content(fakes::ok_body({{"billing", fakes::noul_answer(0.9)}}).dump(), "application/json");
  });
  s.server.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(R"({"models":[{"name":"jev-latest","description":"d","release_date":"2026-09-15"}]})",
                    "application/json");
  });
  s.start();

  ts::ClientOptions options;
  options.api_key = "local-test-key";
  options.base_url = s.url("");
  options.log_level = ts::LogLevel::off;
  auto client = ts::Client::create(options);
  REQUIRE(client);

  ts::Questions q;
  auto billing = q.add("billing", ts::noul("Is this about billing?"));
  auto result = client->system_one("I was charged twice.", q);
  REQUIRE(result);
  CHECK((*result)[billing].noul == 0.9);
  CHECK(result->meta().attempts == 2);
  CHECK(result->meta().request_id == "local-2");
  REQUIRE(retry_counts.size() == 2);
  CHECK(retry_counts[0].empty());
  CHECK(retry_counts[1] == "1");

  auto models = client->list_models();
  REQUIRE(models);
  CHECK(models->at(0).name == "jev-latest");
}
