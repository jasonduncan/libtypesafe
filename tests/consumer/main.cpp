// Built with the caller's default flags (exceptions and RTTI on) against the packaged library.
#include <iostream>

#include <libtypesafe/libtypesafe.hpp>

namespace ts = libtypesafe;

// Subclassing Transport from outside the library needs its typeinfo to link.
class CannedTransport : public ts::Transport {
 public:
  ts::TransferId start(ts::HttpRequest request, std::function<void(ts::TransportOutcome)> done) override {
    seen_url = request.url;
    ts::Json body = {{"model", "jev-test"},
                     {"answers", {{"billing", {{"type", "noul"}, {"noul", 0.93}}}}},
                     {"usage", {{"input_tokens", 5}, {"output_tokens", 1}}}};
    done(ts::HttpResponse{200, {{"Content-Type", "application/json"}}, body.dump()});
    return 1;
  }
  void cancel(ts::TransferId) override {}
  std::string seen_url;
};

int main() {
  auto transport = std::make_shared<CannedTransport>();
  ts::ClientOptions options;
  options.api_key = "consumer-test-key";
  options.transport = transport;
  auto client = ts::Client::create(options);
  if (!client) {
    std::cerr << client.error().message << '\n';
    return 1;
  }
  ts::Questions q;
  auto billing = q.add("billing", ts::noul("Is this about billing?"));
  auto result = client->system_one("I was charged twice.", q);
  if (!result || (*result)[billing].noul != 0.93 || transport->seen_url != "https://api.typesafe.ai/v1/systemone") {
    std::cerr << "unexpected result\n";
    return 1;
  }
  // The default transport links libcurl through the package's dependencies.
  const bool curl = ts::make_curl_transport() != nullptr;
  std::cout << "libtypesafe " << ts::version << " OK (curl transport: " << (curl ? "yes" : "no") << ")\n";
  return 0;
}
