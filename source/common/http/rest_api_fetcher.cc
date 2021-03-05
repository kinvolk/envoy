#include "common/http/rest_api_fetcher.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace Envoy {
namespace Http {

RestApiFetcher::RestApiFetcher(Upstream::ClusterManager& cm, const std::string& remote_cluster_name,
                               Event::Dispatcher& dispatcher, Random::RandomGenerator& random,
                               std::chrono::milliseconds refresh_interval,
                               std::chrono::milliseconds request_timeout)
    : SimpleRestApiFetcher(cm, remote_cluster_name, request_timeout), random_(random),
      refresh_interval_(refresh_interval),
      refresh_timer_(dispatcher.createTimer([this]() -> void { send(); })) {}

RestApiFetcher::~RestApiFetcher() {}

void RestApiFetcher::initialize() { send(); }

void RestApiFetcher::requestComplete() {
  // Add refresh jitter based on the configured interval.
  std::chrono::milliseconds final_delay =
      refresh_interval_ + std::chrono::milliseconds(random_.random() % refresh_interval_.count());

  refresh_timer_->enableTimer(final_delay);
}

} // namespace Http
} // namespace Envoy
