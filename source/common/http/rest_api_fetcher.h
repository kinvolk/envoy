#pragma once

#include <chrono>
#include <string>

#include "envoy/common/random_generator.h"
#include "envoy/event/dispatcher.h"
#include "envoy/runtime/runtime.h"
#include "envoy/upstream/cluster_manager.h"

#include "common/http/simple_rest_api_fetcher.h"

namespace Envoy {
namespace Http {

/**
 * A helper base class used to fetch a REST API at a jittered periodic interval. Once initialize()
 * is called, the API will be fetched and events raised.
 */
class RestApiFetcher : public SimpleRestApiFetcher {
protected:
  RestApiFetcher(Upstream::ClusterManager& cm, const std::string& remote_cluster_name,
                 Event::Dispatcher& dispatcher, Random::RandomGenerator& random,
                 std::chrono::milliseconds refresh_interval,
                 std::chrono::milliseconds request_timeout);
  ~RestApiFetcher() override;

  /**
   * Start the fetch sequence. This should be called once.
   */
  void initialize();

private:
  void requestComplete() override;

  Random::RandomGenerator& random_;
  const std::chrono::milliseconds refresh_interval_;
  Event::TimerPtr refresh_timer_;
};

} // namespace Http
} // namespace Envoy
