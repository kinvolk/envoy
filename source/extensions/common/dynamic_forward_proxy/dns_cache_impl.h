#pragma once

#include "envoy/network/dns.h"
#include "envoy/thread_local/thread_local.h"

#include "common/common/cleanup.h"

#include "extensions/common/dynamic_forward_proxy/dns_cache.h"

#include "absl/container/flat_hash_map.h"

namespace Envoy {
namespace Extensions {
namespace Common {
namespace DynamicForwardProxy {

class DnsCacheImpl : public DnsCache, Logger::Loggable<Logger::Id::dfproxy> {
public:
  DnsCacheImpl(Event::Dispatcher& main_thread_dispatcher, ThreadLocal::SlotAllocator& tls,
               const envoy::config::common::dynamic_forward_proxy::v2alpha::DnsCacheConfig& config);
  ~DnsCacheImpl();

  // DnsCache
  LoadDnsCacheHandlePtr loadDnsCache(absl::string_view host, uint16_t default_port,
                                     LoadDnsCacheCallbacks& callbacks) override;
  AddUpdateCallbacksHandlePtr addUpdateCallbacks(UpdateCallbacks& callbacks) override;

private:
  using TlsHostMap = absl::flat_hash_map<std::string, DnsHostInfoSharedPtr>;
  using TlsHostMapSharedPtr = std::shared_ptr<TlsHostMap>;

  // fixfix
  struct LoadDnsCacheHandleImpl : public LoadDnsCacheHandle,
                                  RaiiListElement<LoadDnsCacheHandleImpl*> {
    LoadDnsCacheHandleImpl(std::list<LoadDnsCacheHandleImpl*>& parent, absl::string_view host,
                           LoadDnsCacheCallbacks& callbacks)
        : RaiiListElement<LoadDnsCacheHandleImpl*>(parent, this), host_(host),
          callbacks_(callbacks) {}

    const std::string host_;
    LoadDnsCacheCallbacks& callbacks_;
  };

  // fixfix
  struct ThreadLocalHostInfo : public ThreadLocal::ThreadLocalObject {
    TlsHostMapSharedPtr host_map_{std::make_shared<TlsHostMap>()};
    std::list<LoadDnsCacheHandleImpl*> pending_resolutions_;
  };

  // fixfix
  struct DnsHostInfoImpl : public DnsHostInfo {
    // DnsHostInfo
    Network::Address::InstanceConstSharedPtr address() override { return address_; }

    Network::Address::InstanceConstSharedPtr address_;
    std::atomic<std::chrono::steady_clock::duration> last_used_time_{
        std::chrono::steady_clock::now().time_since_epoch()}; // fixfix time source
  };

  using DnsHostInfoImplSharedPtr = std::shared_ptr<DnsHostInfoImpl>;

  // fixfix
  struct PrimaryHostInfo {
    PrimaryHostInfo(uint16_t port) : port_(port) {}

    const uint16_t port_;
    DnsHostInfoImplSharedPtr host_info_;
    Network::ActiveDnsQuery* active_query_{};
  };

  // fixfix
  struct AddUpdateCallbacksHandleImpl : public AddUpdateCallbacksHandle,
                                        RaiiListElement<UpdateCallbacks*> {
    AddUpdateCallbacksHandleImpl(std::list<UpdateCallbacks*>& parent, UpdateCallbacks& callbacks)
        : RaiiListElement<UpdateCallbacks*>(parent, &callbacks) {}
  };

  void startResolve(const std::string& host, uint16_t default_port);
  void finishResolve(const std::string& host,
                     const std::list<Network::Address::InstanceConstSharedPtr>& address_list);
  void runAddUpdateCallbacks(const std::string& host, const DnsHostInfoSharedPtr& host_info);
  void updateTlsHostsMap();

  Event::Dispatcher& main_thread_dispatcher_;
  const Network::DnsLookupFamily dns_lookup_family_;
  const Network::DnsResolverSharedPtr resolver_;
  const ThreadLocal::SlotPtr tls_slot_;
  std::list<UpdateCallbacks*> update_callbacks_;
  absl::flat_hash_map<std::string, PrimaryHostInfo> primary_hosts_;
};

} // namespace DynamicForwardProxy
} // namespace Common
} // namespace Extensions
} // namespace Envoy
