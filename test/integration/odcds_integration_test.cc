#include <string>

#include "envoy/common/platform.h"
#include "envoy/config/cluster/v3/cluster.pb.h"

#include "source/common/common/fmt.h"
#include "source/common/common/macros.h"

#include "test/common/grpc/grpc_client_integration.h"
#include "test/integration/ads_integration.h"
#include "test/integration/fake_upstream.h"
#include "test/integration/http_integration.h"
#include "test/test_common/resources.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace {

const std::string& config() {
  CONSTRUCT_ON_FIRST_USE(std::string, fmt::format(R"EOF(
admin:
  access_log:
  - name: envoy.access_loggers.file
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.access_loggers.file.v3.FileAccessLog
      path: "{}"
  address:
    socket_address:
      address: 127.0.0.1
      port_value: 0
static_resources:
  clusters:
  - name: xds_cluster
    type: STATIC
    typed_extension_protocol_options:
      envoy.extensions.upstreams.http.v3.HttpProtocolOptions:
        "@type": type.googleapis.com/envoy.extensions.upstreams.http.v3.HttpProtocolOptions
        explicit_http_config:
          http2_protocol_options: {{}}
    load_assignment:
      cluster_name: xds_cluster
      endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: 127.0.0.1
                port_value: 0
  listeners:
  - name: http
    address:
      socket_address:
        address: 127.0.0.1
        port_value: 0
    filter_chains:
    - filters:
      - name: http
        typed_config:
          "@type": type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager
          stat_prefix: config_test
          http_filters:
          - name: envoy.filters.http.on_demand
          - name: envoy.filters.http.router
          codec_type: HTTP2
          route_config:
            name: local_route
            virtual_hosts:
            - name: local_service
              domains: ["*"]
              typed_per_filter_config:
                envoy.filters.http.on_demand:
                  "@type": type.googleapis.com/envoy.extensions.filters.http.on_demand.v3.PerRouteConfig
                  odcds_config:
                    resource_api_version: V3
                    api_config_source:
                      api_type: DELTA_GRPC
                      transport_api_version: V3
                      grpc_services:
                        envoy_grpc:
                          cluster_name: xds_cluster
              routes:
              - match: {{ prefix: "/" }}
                route:
                  cluster_header: "Pick-This-Cluster"
)EOF",
                                                  Platform::null_device_path));
}

class OdCdsIntegrationTestBase : public HttpIntegrationTest,
                                 public Grpc::GrpcClientIntegrationParamTest {
public:
  OdCdsIntegrationTestBase(const std::string& config)
      : HttpIntegrationTest(Http::CodecClient::Type::HTTP2, ipVersion(), config) {
    use_lds_ = false;
  }

  void TearDown() override {
    if (doCleanUpXdsConnection_) {
      cleanUpXdsConnection();
    }
  }

  void initialize() override {
    // Controls how many addFakeUpstream() will happen in
    // BaseIntegrationTest::createUpstreams() (which is part of initialize()).
    // Make sure this number matches the size of the 'clusters' repeated field in the bootstrap
    // config that you use!
    setUpstreamCount(1);                                  // The xDS cluster
    setUpstreamProtocol(FakeHttpConnection::Type::HTTP2); // CDS uses gRPC uses HTTP2.

    // BaseIntegrationTest::initialize() does many things:
    // 1) It appends to fake_upstreams_ as many as you asked for via setUpstreamCount().
    // 2) It updates your bootstrap config with the ports your fake upstreams are actually listening
    //    on (since you're supposed to leave them as 0).
    // 3) It creates and starts an IntegrationTestServer - the thing that wraps the almost-actual
    //    Envoy used in the tests.
    // 4) Bringing up the server usually entails waiting to ensure that any listeners specified in
    //    the bootstrap config have come up, and registering them in a port map (see lookupPort()).
    HttpIntegrationTest::initialize();

    addFakeUpstream(FakeHttpConnection::Type::HTTP2);
    new_cluster_ = ConfigHelper::buildStaticCluster(
        "new_cluster", fake_upstreams_[1]->localAddress()->ip()->port(),
        Network::Test::getLoopbackAddressString(ipVersion()));

    test_server_->waitUntilListenersReady();
    registerTestServerPorts({"http"});
  }

  FakeStreamPtr odcds_stream_;
  envoy::config::cluster::v3::Cluster new_cluster_;
  bool doCleanUpXdsConnection_ = true;
};

class OdCdsIntegrationTest : public OdCdsIntegrationTestBase {
public:
  OdCdsIntegrationTest() : OdCdsIntegrationTestBase(config()) {}
};

INSTANTIATE_TEST_SUITE_P(IpVersionsClientType, OdCdsIntegrationTest,
                         GRPC_CLIENT_INTEGRATION_PARAMS);

// tests a scenario when:
//  - making a request to an unknown cluster
//  - odcds initiates a connection with a request for the cluster
//  - a response contains the cluster
//  - request is resumed
TEST_P(OdCdsIntegrationTest, OnDemandClusterDiscoveryWorksWithClusterHeader) {
  initialize();
  codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":path", "/"},
                                                 {":scheme", "http"},
                                                 {":authority", "vhost.first"},
                                                 {"Pick-This-Cluster", "new_cluster"}};
  IntegrationStreamDecoderPtr response = codec_client_->makeHeaderOnlyRequest(request_headers);

  auto result = // xds_connection_ is filled with the new FakeHttpConnection.
      fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, xds_connection_);
  RELEASE_ASSERT(result, result.message());
  result = xds_connection_->waitForNewStream(*dispatcher_, odcds_stream_);
  RELEASE_ASSERT(result, result.message());
  odcds_stream_->startGrpcStream();

  EXPECT_TRUE(compareDeltaDiscoveryRequest(Config::TypeUrl::get().Cluster, {"new_cluster"}, {},
                                           odcds_stream_));
  sendDeltaDiscoveryResponse<envoy::config::cluster::v3::Cluster>(
      Config::TypeUrl::get().Cluster, {new_cluster_}, {}, "1", odcds_stream_);
  EXPECT_TRUE(compareDeltaDiscoveryRequest(Config::TypeUrl::get().Cluster, {}, {}, odcds_stream_));

  waitForNextUpstreamRequest(1);
  // Send response headers, and end_stream if there is no response body.
  upstream_request_->encodeHeaders(default_response_headers_, true);

  response->waitForHeaders();
  EXPECT_EQ("200", response->headers().getStatusValue());

  cleanupUpstreamAndDownstream();
}

const std::string& configODCDSVhostEnabledRouteDisabled() {
  CONSTRUCT_ON_FIRST_USE(std::string, fmt::format(R"EOF(
admin:
  access_log:
  - name: envoy.access_loggers.file
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.access_loggers.file.v3.FileAccessLog
      path: "{}"
  address:
    socket_address:
      address: 127.0.0.1
      port_value: 0
static_resources:
  clusters:
  - name: xds_cluster
    type: STATIC
    typed_extension_protocol_options:
      envoy.extensions.upstreams.http.v3.HttpProtocolOptions:
        "@type": type.googleapis.com/envoy.extensions.upstreams.http.v3.HttpProtocolOptions
        explicit_http_config:
          http2_protocol_options: {{}}
    load_assignment:
      cluster_name: xds_cluster
      endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: 127.0.0.1
                port_value: 0
  listeners:
  - name: http
    address:
      socket_address:
        address: 127.0.0.1
        port_value: 0
    filter_chains:
    - filters:
      - name: http
        typed_config:
          "@type": type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager
          stat_prefix: config_test
          http_filters:
          - name: envoy.filters.http.on_demand
          - name: envoy.filters.http.router
          codec_type: HTTP2
          route_config:
            name: local_route
            virtual_hosts:
            - name: local_service
              domains: ["*"]
              typed_per_filter_config:
                envoy.filters.http.on_demand:
                  "@type": type.googleapis.com/envoy.extensions.filters.http.on_demand.v3.PerRouteConfig
                  odcds_config:
                    resource_api_version: V3
                    api_config_source:
                      api_type: DELTA_GRPC
                      transport_api_version: V3
                      grpc_services:
                        envoy_grpc:
                          cluster_name: xds_cluster
              routes:
              - match: {{ prefix: "/" }}
                route:
                  cluster_header: "Pick-This-Cluster"
                typed_per_filter_config:
                  envoy.filters.http.on_demand:
                    "@type": type.googleapis.com/envoy.extensions.filters.http.on_demand.v3.PerRouteConfig
)EOF",
                                                  Platform::null_device_path));
}

class OdCdsIntegrationTestDisabled : public OdCdsIntegrationTestBase {
public:
  OdCdsIntegrationTestDisabled()
      : OdCdsIntegrationTestBase(configODCDSVhostEnabledRouteDisabled()) {
    doCleanUpXdsConnection_ = false;
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersionsClientType, OdCdsIntegrationTestDisabled,
                         GRPC_CLIENT_INTEGRATION_PARAMS);

// tests a scenario when:
//  - ODCDS is enabled at a virtual host level
//  - ODCDS is disabled at a route level
//  - making a request to an unknown cluster
//  - request fails
TEST_P(OdCdsIntegrationTestDisabled, DisablingODCDSAtRouteLevelWorks) {
  initialize();
  codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":path", "/"},
                                                 {":scheme", "http"},
                                                 {":authority", "vhost.first"},
                                                 {"Pick-This-Cluster", "new_cluster"}};
  IntegrationStreamDecoderPtr response = codec_client_->makeHeaderOnlyRequest(request_headers);

  EXPECT_FALSE(fake_upstreams_[0]->waitForHttpConnection(*dispatcher_, xds_connection_,
                                                         std::chrono::milliseconds(1000)));

  response->waitForHeaders();
  EXPECT_EQ("503", response->headers().getStatusValue());

  cleanupUpstreamAndDownstream();
}

envoy::config::listener::v3::Listener buildOdCdsListener(const std::string& address) {
  envoy::config::listener::v3::Listener listener;
  TestUtility::loadFromYaml(fmt::format(R"EOF(
      name: http
      address:
        socket_address:
          address: {}
          port_value: 0
      filter_chains:
      - filters:
        - name: http
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager
            stat_prefix: config_test
            http_filters:
            - name: envoy.filters.http.on_demand
            - name: envoy.filters.http.router
            codec_type: HTTP2
            route_config:
              name: local_route
              virtual_hosts:
              - name: local_service
                domains: ["*"]
                typed_per_filter_config:
                  envoy.filters.http.on_demand:
                    "@type": type.googleapis.com/envoy.extensions.filters.http.on_demand.v3.PerRouteConfig
                    odcds_config:
                      resource_api_version: V3
                      ads: {{}}
                routes:
                - match: {{ prefix: "/" }}
                  route:
                    cluster_header: "Pick-This-Cluster"
)EOF",
                                        address),
                            listener);
  return listener;
}

class OdCdsAdsIntegrationTest : public AdsIntegrationTest {
public:
  void initialize() override {
    AdsIntegrationTest::initialize();

    test_server_->waitUntilListenersReady();
    fake_upstream_idx_ = fake_upstreams_.size();
    auto& upstream = addFakeUpstream(FakeHttpConnection::Type::HTTP2);
    new_cluster_ =
        ConfigHelper::buildStaticCluster("new_cluster", upstream.localAddress()->ip()->port(),
                                         Network::Test::getLoopbackAddressString(ipVersion()));
  }

  std::size_t fake_upstream_idx_;
  envoy::config::cluster::v3::Cluster new_cluster_;
};

INSTANTIATE_TEST_SUITE_P(
    IpVersionsClientTypeDeltaWildcard, OdCdsAdsIntegrationTest,
    testing::Combine(testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                     testing::ValuesIn(TestEnvironment::getsGrpcVersionsForTest()),
                     // Only delta xDS is supported for on-demand CDS.
                     testing::Values(Grpc::SotwOrDelta::Delta, Grpc::SotwOrDelta::UnifiedDelta),
                     // Only new DSS is supported for on-demand CDS.
                     testing::Values(OldDssOrNewDss::New)));

// tests a scenario when:
//  - making a request to an unknown cluster
//  - odcds initiates a connection with a request for the cluster
//  - a response contains the cluster
//  - request is resumed
TEST_P(OdCdsAdsIntegrationTest, OnDemandClusterDiscoveryWorksWithClusterHeader) {
  initialize();

  auto compareRequest = [this](const std::string& type_url,
                               const std::vector<std::string>& expected_resource_subscriptions,
                               const std::vector<std::string>& expected_resource_unsubscriptions,
                               bool expect_node = false) {
    return compareDeltaDiscoveryRequest(type_url, expected_resource_subscriptions,
                                        expected_resource_unsubscriptions,
                                        Grpc::Status::WellKnownGrpcStatus::Ok, "", expect_node);
  };

  // initial cluster query
  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Cluster, {}, {}, true));
  sendDeltaDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                                  {}, {}, "1");

  // initial listener query
  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Listener, {}, {}));
  auto odcds_listener = buildOdCdsListener(Network::Test::getLoopbackAddressString(ipVersion()));
  sendDeltaDiscoveryResponse<envoy::config::listener::v3::Listener>(Config::TypeUrl::get().Listener,
                                                                    {odcds_listener}, {}, "2");

  // acks
  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Cluster, {}, {}));
  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Listener, {}, {}));

  // listener got acked, so register the http port now.
  test_server_->waitUntilListenersReady();
  registerTestServerPorts({"http"});
  codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":path", "/"},
                                                 {":scheme", "http"},
                                                 {":authority", "vhost.first"},
                                                 {"Pick-This-Cluster", "new_cluster"}};
  IntegrationStreamDecoderPtr response = codec_client_->makeHeaderOnlyRequest(request_headers);

  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Cluster, {"new_cluster"}, {}));
  sendDeltaDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                                  {new_cluster_}, {}, "3");
  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Cluster, {}, {}));

  waitForNextUpstreamRequest(fake_upstream_idx_);
  // Send response headers, and end_stream if there is no response body.
  upstream_request_->encodeHeaders(default_response_headers_, true);

  response->waitForHeaders();
  EXPECT_EQ("200", response->headers().getStatusValue());

  cleanupUpstreamAndDownstream();
}

envoy::config::listener::v3::Listener buildOdCdsXdstpListener(const std::string& address) {
  envoy::config::listener::v3::Listener listener;
  TestUtility::loadFromYaml(fmt::format(R"EOF(
      name: http
      address:
        socket_address:
          address: {}
          port_value: 0
      filter_chains:
      - filters:
        - name: http
          typed_config:
            "@type": type.googleapis.com/envoy.extensions.filters.network.http_connection_manager.v3.HttpConnectionManager
            stat_prefix: config_test
            http_filters:
            - name: envoy.filters.http.on_demand
            - name: envoy.filters.http.router
            codec_type: HTTP2
            route_config:
              name: local_route
              virtual_hosts:
              - name: local_service
                domains: ["*"]
                typed_per_filter_config:
                  envoy.filters.http.on_demand:
                    "@type": type.googleapis.com/envoy.extensions.filters.http.on_demand.v3.PerRouteConfig
                    odcds_config:
                      resource_api_version: V3
                      ads: {{}}
                    resources_locator: "xdstp://test/envoy.config.cluster.v3.Cluster/cluster_collection/*"
                routes:
                - match: {{ prefix: "/" }}
                  route:
                    cluster_header: "Pick-This-Cluster"
)EOF",
                                        address),
                            listener);
  return listener;
}

class OdCdsAdsXdstpIntegrationTest : public AdsIntegrationTest {
public:
  void initialize() override {
    AdsIntegrationTest::initialize();

    test_server_->waitUntilListenersReady();
    fake_upstream_idx_ = fake_upstreams_.size();
    auto& upstream = addFakeUpstream(FakeHttpConnection::Type::HTTP2);
    collection_cluster_ = ConfigHelper::buildStaticCluster(
        "xdstp://test/envoy.config.cluster.v3.Cluster/cluster_collection/bar",
        upstream.localAddress()->ip()->port(),
        Network::Test::getLoopbackAddressString(ipVersion()));
    new_cluster_ =
        ConfigHelper::buildStaticCluster("new_cluster", upstream.localAddress()->ip()->port(),
                                         Network::Test::getLoopbackAddressString(ipVersion()));
  }

  std::size_t fake_upstream_idx_;
  envoy::config::cluster::v3::Cluster collection_cluster_;
  envoy::config::cluster::v3::Cluster new_cluster_;
};

INSTANTIATE_TEST_SUITE_P(
    IpVersionsClientTypeDeltaWildcard, OdCdsAdsXdstpIntegrationTest,
    testing::Combine(testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                     testing::ValuesIn(TestEnvironment::getsGrpcVersionsForTest()),
                     // Only delta xDS is supported for on-demand CDS.
                     testing::Values(Grpc::SotwOrDelta::Delta, Grpc::SotwOrDelta::UnifiedDelta),
                     // Only new DSS is supported for on-demand CDS.
                     testing::Values(OldDssOrNewDss::New)));

// tests a scenario when:
//  - making a request to an unknown cluster
//  - odcds initiates a connection with a request for the xdstp resource
//  - odcds request cluster resource referenced by the request.
//  - a response contains the cluster
//  - request is resumed
TEST_P(OdCdsAdsXdstpIntegrationTest, OnDemandClusterDiscoveryWorksWithClusterHeader) {
  initialize();

  auto compareRequest = [this](const std::string& type_url,
                               const std::vector<std::string>& expected_resource_subscriptions,
                               const std::vector<std::string>& expected_resource_unsubscriptions,
                               bool expect_node = false) {
    return compareDeltaDiscoveryRequest(type_url, expected_resource_subscriptions,
                                        expected_resource_unsubscriptions,
                                        Grpc::Status::WellKnownGrpcStatus::Ok, "", expect_node);
  };

  // initial cluster query
  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Cluster, {}, {}, true));
  sendDeltaDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                                  {}, {}, "1");

  // initial listener query
  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Listener, {}, {}));
  auto odcds_listener =
      buildOdCdsXdstpListener(Network::Test::getLoopbackAddressString(ipVersion()));
  sendDeltaDiscoveryResponse<envoy::config::listener::v3::Listener>(Config::TypeUrl::get().Listener,
                                                                    {odcds_listener}, {}, "2");

  // acks
  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Cluster, {}, {}));
  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Listener, {}, {}));

  // listener got acked, so register the http port now.
  test_server_->waitUntilListenersReady();
  registerTestServerPorts({"http"});
  codec_client_ = makeHttpConnection(makeClientConnection(lookupPort("http")));
  Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                 {":path", "/"},
                                                 {":scheme", "http"},
                                                 {":authority", "vhost.first"},
                                                 {"Pick-This-Cluster", "new_cluster"}};
  IntegrationStreamDecoderPtr response = codec_client_->makeHeaderOnlyRequest(request_headers);

  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Cluster,
                             {"xdstp://test/envoy.config.cluster.v3.Cluster/cluster_collection/*"},
                             {}));
  sendDeltaDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                                  {collection_cluster_}, {}, "3");
  EXPECT_TRUE(compareRequest(Config::TypeUrl::get().Cluster, {"new_cluster"}, {}));
  sendDeltaDiscoveryResponse<envoy::config::cluster::v3::Cluster>(Config::TypeUrl::get().Cluster,
                                                                  {new_cluster_}, {}, "4");

  waitForNextUpstreamRequest(fake_upstream_idx_);
  // Send response headers, and end_stream if there is no response body.
  upstream_request_->encodeHeaders(default_response_headers_, true);

  response->waitForHeaders();
  EXPECT_EQ("200", response->headers().getStatusValue());

  cleanupUpstreamAndDownstream();
}

} // namespace
} // namespace Envoy
