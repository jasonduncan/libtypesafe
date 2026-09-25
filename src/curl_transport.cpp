#ifdef LIBTYPESAFE_WITH_CURL

#include <curl/curl.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "libtypesafe/transport.hpp"
#include "util.hpp"

namespace libtypesafe {

namespace {

std::once_flag global_init_once;

struct Transfer {
  TransferId id = 0;
  CURL* easy = nullptr;
  curl_slist* header_list = nullptr;
  std::string method;
  std::string body;  // must outlive the transfer: CURLOPT_POSTFIELDS does not copy
  HttpResponse response;
  std::function<void(TransportOutcome)> done;
  char error_buffer[CURL_ERROR_SIZE] = {};

  ~Transfer() {
    if (header_list != nullptr) curl_slist_free_all(header_list);
    if (easy != nullptr) curl_easy_cleanup(easy);
  }
};

std::size_t on_body(char* data, std::size_t size, std::size_t count, void* user) {
  static_cast<Transfer*>(user)->response.body.append(data, size * count);
  return size * count;
}

std::size_t on_header(char* data, std::size_t size, std::size_t count, void* user) {
  auto* transfer = static_cast<Transfer*>(user);
  std::string_view line(data, size * count);
  line = detail::trim(line);
  if (line.rfind("HTTP/", 0) == 0) {
    transfer->response.headers = Headers();  // a new response (e.g. after 100 Continue)
  } else if (const auto colon = line.find(':'); colon != std::string_view::npos && colon > 0) {
    transfer->response.headers.set(std::string(detail::trim(line.substr(0, colon))),
                                   std::string(detail::trim(line.substr(colon + 1))));
  }
  return size * count;
}

class CurlTransport final : public Transport {
 public:
  explicit CurlTransport(CurlOptions options) : options_(std::move(options)) {
    if (options_.global_init) std::call_once(global_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    multi_ = curl_multi_init();
  }

  ~CurlTransport() override {
    auto transfers = std::move(transfers_);
    for (auto& [id, transfer] : transfers) {
      curl_multi_remove_handle(multi_, transfer->easy);
      finish(std::move(transfer), TransportError{TransportError::Kind::cancelled, "transport destroyed", true});
    }
    if (multi_ != nullptr) curl_multi_cleanup(multi_);
  }

  TransferId start(HttpRequest request, std::function<void(TransportOutcome)> done) override {
    auto transfer = std::make_unique<Transfer>();
    const TransferId id = transfer->id = ++next_id_;
    transfer->done = std::move(done);
    transfer->method = std::move(request.method);
    transfer->body = std::move(request.body);
    transfer->easy = curl_easy_init();
    if (multi_ == nullptr || transfer->easy == nullptr) {
      finish(std::move(transfer), TransportError{TransportError::Kind::connection, "libcurl initialization failed", false});
      return id;
    }

    CURL* easy = transfer->easy;
    curl_easy_setopt(easy, CURLOPT_PRIVATE, transfer.get());
    curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(std::min(request.timeout, options_.connect_timeout).count()));
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, transfer->error_buffer);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, transfer.get());
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, transfer.get());
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");  // whatever libcurl supports
    if (options_.http2) curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
    if (options_.proxy) curl_easy_setopt(easy, CURLOPT_PROXY, options_.proxy->c_str());
    if (options_.ca_bundle) curl_easy_setopt(easy, CURLOPT_CAINFO, options_.ca_bundle->c_str());

    if (transfer->method == "GET") {
      curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    } else {
      if (transfer->method != "POST") curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, transfer->method.c_str());
      curl_easy_setopt(easy, CURLOPT_POST, 1L);
      curl_easy_setopt(easy, CURLOPT_POSTFIELDS, transfer->body.data());
      curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(transfer->body.size()));
    }
    for (const auto& [name, value] : request.headers) {
      const std::string line = name + ": " + value;
      transfer->header_list = curl_slist_append(transfer->header_list, line.c_str());
    }
    transfer->header_list = curl_slist_append(transfer->header_list, "Expect:");  // no 100-continue round trip
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer->header_list);

    if (curl_multi_add_handle(multi_, easy) != CURLM_OK) {
      finish(std::move(transfer), TransportError{TransportError::Kind::connection, "curl_multi_add_handle failed", false});
      return id;
    }
    transfers_.emplace(id, std::move(transfer));
    return id;
  }

  void cancel(TransferId id) override {
    const auto it = transfers_.find(id);
    if (it == transfers_.end()) return;
    auto transfer = std::move(it->second);
    transfers_.erase(it);
    curl_multi_remove_handle(multi_, transfer->easy);
    const bool sent = request_sent(*transfer);
    finish(std::move(transfer), TransportError{TransportError::Kind::cancelled, "cancelled", sent});
  }

  void poll() override {
    if (transfers_.empty()) return;
    int running = 0;
    curl_multi_perform(multi_, &running);
    int queued = 0;
    while (CURLMsg* message = curl_multi_info_read(multi_, &queued)) {
      if (message->msg != CURLMSG_DONE) continue;
      CURL* easy = message->easy_handle;
      const CURLcode code = message->data.result;  // copy before removing the handle
      Transfer* raw = nullptr;
      curl_easy_getinfo(easy, CURLINFO_PRIVATE, reinterpret_cast<char**>(&raw));
      const auto it = transfers_.find(raw->id);
      if (it == transfers_.end()) continue;
      auto transfer = std::move(it->second);
      transfers_.erase(it);
      curl_multi_remove_handle(multi_, easy);
      finish_transfer(std::move(transfer), code);
    }
  }

  void wait(std::chrono::milliseconds max_wait) override {
    const auto ms = static_cast<int>(std::clamp<std::int64_t>(max_wait.count(), 0, 1000));
    if (multi_ != nullptr) curl_multi_poll(multi_, nullptr, 0, ms, nullptr);
  }

 private:
  /// Whether the server may have received the whole request. CURLINFO_REQUEST_SIZE is not usable:
  /// libcurl 8.7 still reports 0 after a timeout that follows a fully sent request.
  static bool request_sent(const Transfer& transfer) {
    curl_off_t pretransfer = 0;  // microseconds until the request was about to be sent; 0 = never
    curl_off_t uploaded = 0;
    curl_easy_getinfo(transfer.easy, CURLINFO_PRETRANSFER_TIME_T, &pretransfer);
    curl_easy_getinfo(transfer.easy, CURLINFO_SIZE_UPLOAD_T, &uploaded);
    if (pretransfer <= 0) return false;  // DNS, connect, or TLS never finished
    return uploaded >= static_cast<curl_off_t>(transfer.body.size());
  }

  void finish_transfer(std::unique_ptr<Transfer> transfer, CURLcode code) {
    if (code == CURLE_OK) {
      long status = 0;
      curl_easy_getinfo(transfer->easy, CURLINFO_RESPONSE_CODE, &status);
      transfer->response.status = static_cast<int>(status);
      HttpResponse response = std::move(transfer->response);
      finish(std::move(transfer), std::move(response));
      return;
    }
    TransportError error;
    error.kind = code == CURLE_OPERATION_TIMEDOUT ? TransportError::Kind::timeout : TransportError::Kind::connection;
    error.message = transfer->error_buffer[0] != '\0' ? std::string(transfer->error_buffer)
                                                      : std::string(curl_easy_strerror(code));
    error.request_sent = request_sent(*transfer);
    finish(std::move(transfer), std::move(error));
  }

  static void finish(std::unique_ptr<Transfer> transfer, TransportOutcome outcome) {
    auto done = std::move(transfer->done);
    transfer.reset();
    if (done) done(std::move(outcome));
  }

  CurlOptions options_;
  CURLM* multi_ = nullptr;
  TransferId next_id_ = 0;
  std::unordered_map<TransferId, std::unique_ptr<Transfer>> transfers_;
};

}  // namespace

std::shared_ptr<Transport> make_curl_transport(CurlOptions options) {
  return std::make_shared<CurlTransport>(std::move(options));
}

}  // namespace libtypesafe

#endif  // LIBTYPESAFE_WITH_CURL
