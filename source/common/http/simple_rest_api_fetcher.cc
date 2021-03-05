#include "common/http/simple_rest_api_fetcher.h"

#include <chrono>
#include <cstdint>
#include <string>

#include "common/common/enum_to_int.h"
#include "common/http/message_impl.h"
#include "common/http/utility.h"

namespace Envoy {
namespace Http {

SimpleRestApiFetcher::SimpleRestApiFetcher(Upstream::ClusterManager& cm, const std::string& remote_cluster_name,
                                           std::chrono::milliseconds request_timeout)
    : remote_cluster_name_(remote_cluster_name), cm_(cm),
      request_timeout_(request_timeout) {}

SimpleRestApiFetcher::~SimpleRestApiFetcher() {
  if (active_request_) {
    active_request_->cancel();
  }
}

void SimpleRestApiFetcher::send() {
  if (active_request_ != nullptr) {
    // TODO: throw
    return;
  }

  RequestMessagePtr message(new RequestMessageImpl());
  createRequest(*message);
  message->headers().setHost(remote_cluster_name_);
  const auto thread_local_cluster = cm_.getThreadLocalCluster(remote_cluster_name_);
  if (thread_local_cluster != nullptr) {
    active_request_ = thread_local_cluster->httpAsyncClient().send(
        std::move(message), *this, AsyncClient::RequestOptions().setTimeout(request_timeout_));
  } else {
    onFetchFailure(Config::ConfigUpdateFailureReason::ConnectionFailure, nullptr);
    done();
  }
}

void SimpleRestApiFetcher::onSuccess(const Http::AsyncClient::Request& request,
                                     Http::ResponseMessagePtr&& response) {
  uint64_t response_code = Http::Utility::getResponseStatus(response->headers());
  if (response_code == enumToInt(Http::Code::NotModified)) {
    done();
    return;
  } else if (response_code != enumToInt(Http::Code::OK)) {
    onFailure(request, Http::AsyncClient::FailureReason::Reset);
    return;
  }

  try {
    parseResponse(*response);
  } catch (EnvoyException& e) {
    onFetchFailure(Config::ConfigUpdateFailureReason::UpdateRejected, &e);
  }

  done();
}

void SimpleRestApiFetcher::onFailure(const Http::AsyncClient::Request&,
                                     Http::AsyncClient::FailureReason reason) {
  // Currently Http::AsyncClient::FailureReason only has one value: "Reset".
  ASSERT(reason == Http::AsyncClient::FailureReason::Reset);
  onFetchFailure(Config::ConfigUpdateFailureReason::ConnectionFailure, nullptr);
  done();
}

void SimpleRestApiFetcher::done() {
  onFetchComplete();
  active_request_ = nullptr;
  requestComplete();
}

} // namespace Http
} // namespace Envoy
