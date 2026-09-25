#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <libtypesafe/libtypesafe.hpp>

#include "client_impl.hpp"

namespace fakes {

namespace ts = libtypesafe;

/// Records every attempt; tests complete them by hand or script automatic replies.
struct FakeTransport : ts::Transport {
  struct Attempt {
    ts::TransferId id = 0;
    ts::HttpRequest request;
    std::function<void(ts::TransportOutcome)> done;
    bool finished = false;
    bool cancelled = false;
  };

  std::vector<Attempt> attempts;
  /// Replies sent, in order, to attempts started while they last. Delivered from poll().
  std::deque<ts::TransportOutcome> script;
  int polls = 0;

  ts::TransferId start(ts::HttpRequest request, std::function<void(ts::TransportOutcome)> done) override {
    attempts.push_back({static_cast<ts::TransferId>(attempts.size() + 1), std::move(request), std::move(done)});
    return attempts.back().id;
  }

  void cancel(ts::TransferId id) override {
    for (auto& attempt : attempts) {
      if (attempt.id == id && !attempt.finished) {
        attempt.cancelled = true;
        complete(attempt, ts::TransportError{ts::TransportError::Kind::cancelled, "cancelled", true});
      }
    }
  }

  void poll() override {
    ++polls;
    for (auto& attempt : attempts) {
      if (attempt.finished || script.empty()) continue;
      auto outcome = std::move(script.front());
      script.pop_front();
      complete(attempt, std::move(outcome));
    }
  }

  void complete(Attempt& attempt, ts::TransportOutcome outcome) {
    attempt.finished = true;
    attempt.done(std::move(outcome));
  }

  void complete(std::size_t index, ts::TransportOutcome outcome) { complete(attempts.at(index), std::move(outcome)); }
};

inline ts::HttpResponse json_response(int status, const ts::Json& body, ts::Headers headers = {}) {
  headers.set("Content-Type", "application/json");
  return {status, std::move(headers), body.dump()};
}

inline ts::Json noul_answer(double p) { return {{"type", "noul"}, {"noul", p}}; }

inline ts::Json ok_body(ts::Json answers) {
  return {{"model", "jev-1.13.0"}, {"answers", std::move(answers)}, {"usage", {{"input_tokens", 12}, {"output_tokens", 1}}}};
}

/// A controllable clock and RNG installed into a client.
struct Harness {
  std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
  ts::detail::Clock::time_point now{};
  std::vector<std::pair<ts::LogLevel, std::string>> logs;
  ts::Client client;

  explicit Harness(ts::ClientOptions options = {}) : client(make(options)) {
    auto& impl = ts::detail::TestAccess::impl(client);
    impl.now = [this] { return now; };
    impl.random = [] { return 0.0; };  // no jitter
  }

  void advance(std::chrono::milliseconds ms) { now += ms; }

  ts::Client make(ts::ClientOptions& options) {
    if (!options.api_key) options.api_key = "ts-test-key-123456";
    options.transport = transport;
    if (!options.log_level) options.log_level = ts::LogLevel::debug;
    options.logger = [this](ts::LogLevel level, std::string_view message) { logs.emplace_back(level, message); };
    auto created = ts::Client::create(std::move(options));
    if (!created) throw std::runtime_error(created.error().message);
    return std::move(created).value();
  }

  bool logged(std::string_view needle) const {
    for (const auto& [level, line] : logs) {
      if (line.find(needle) != std::string::npos) return true;
    }
    return false;
  }
};

}  // namespace fakes
