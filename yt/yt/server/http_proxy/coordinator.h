#pragma once

#include "public.h"

#include "config.h"
#include "helpers.h"
#include "private.h"
#include "component_discovery.h"

#include <yt/yt/ytlib/api/public.h>
#include <yt/yt/ytlib/api/native/public.h>

#include <yt/yt/core/actions/signal.h>

#include <yt/yt/core/http/http.h>

#include <yt/yt/library/tracing/jaeger/sampler.h>

#include <yt/yt/core/ytree/yson_struct.h>

#include <library/cpp/yt/memory/atomic_intrusive_ptr.h>

namespace NYT::NHttpProxy {

////////////////////////////////////////////////////////////////////////////////

// @gearonixx

// UpdatedAt — момент снятия снимка; по этой метке другие прокси решают «жив/мёртв».
// LoadAverage — системный load average, средняя длина очереди готовых процессов.
// NetworkCoef — коэффициент сетевой нагрузки.
// UserCpu — доля CPU в юзерспейсе.
// SystemCpu — доля CPU в ядре.
// CpuWait — доля CPU в ожидании io.
// ConcurrentRequests — сколько HTTP-запросов сейчас в обработке.
struct TLiveness
    : public NYTree::TYsonStruct
{
    TInstant UpdatedAt;
    double LoadAverage;
    double NetworkCoef;
    double UserCpu, SystemCpu, CpuWait;
    int ConcurrentRequests;

    REGISTER_YSON_STRUCT(TLiveness);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TLiveness)

////////////////////////////////////////////////////////////////////////////////
///
///
// ● Запись об одной прокси в реестре //sys/proxies/<host>:
// её endpoint, роль, флаг бана и Liveness (CPU, load, число запросов).
// YSON-структура — сериализуется в Cypress и читается оттуда другими прокси, чтобы
//   знать состояние соседей.

//   ● Да, отдельный процесс на своём хосте — ytserver-http-proxy. Их пускают пачкой (десятки штук), они стоят перед кластером YT и принимают HTTP от клиентов, потом форвардят как RPC в мастер/ноды. Координатор
// как раз и нужен, чтобы эти прокси знали друг про друга.

//  ❯ компьютеры или процессы?
//
// ● Процессы. Один хост может крутить несколько прокси (на разных портах), но обычно 1 процесс = 1 машина.

struct TProxyEntry
    : public NYTree::TYsonStruct
{

  //   Endpoint — адрес прокси (host:port), по которому к ней ходят клиенты. Role — логическая группа (data, control, default...), клиент в /hosts?role=data получит только прокси с этой ролью. Liveness — снимок
  // текущей нагрузки (CPU, load average, число активных запросов), по нему координатор балансирует и решает, кто «жив».
    std::string Endpoint;
    // етка-группа прокси: админ ставит роль в //sys/proxies/<host>/@role, и клиент запросом /hosts?role=data получает только прокси с этой ролью. Так разделяют трафик — например, тяжёлые батчи на одни прокси,
    // интерактив на другие.
    std::string Role;

    TLivenessPtr Liveness;

    bool IsBanned;
    std::optional<TString> BanMessage;

    std::string GetHost() const;

    REGISTER_YSON_STRUCT(TProxyEntry);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TProxyEntry)

////////////////////////////////////////////////////////////////////////////////

struct TCoordinatorProxy
    : public TRefCounted
{
    const TProxyEntryPtr Entry;
    std::atomic<i64> Dampening = 0;

    explicit TCoordinatorProxy(const TProxyEntryPtr& proxyEntry);
};

DEFINE_REFCOUNTED_TYPE(TCoordinatorProxy)

////////////////////////////////////////////////////////////////////////////////
///
/// отому что на координатор держат TIntrusivePtr сразу несколько владельцев (TBootstrap, хендлеры /hosts и /ping, TAccessChecker, фоновые периодики),
/// и время жизни не привязано к одному из них — объект должен
// жить, пока есть хоть одна ссылка. TRefCounted даёт встроенный счётчик ссылок, на котором работает TIntrusivePtr (дешевле shared_ptr, счётчик в самом
class TCoordinator
    : public TRefCounted
{
public:
    TCoordinator(
        TProxyBootstrapConfigPtr config,
        TBootstrap* bootstrap);

    void Start();

    bool IsBanned() const;
    bool CanHandleHeavyRequests() const;

    std::vector<TProxyEntryPtr> ListProxyEntries(
        const std::optional<std::string>& role,
        bool includeDeadAndBanned = false);
    TProxyEntryPtr AllocateProxyEntry(const std::string& role);
    TProxyEntryPtr GetSelfEntry() const;

    const TCoordinatorConfigPtr& GetConfig() const;
    NTracing::TSamplerPtr GetTraceSampler();

    bool IsDead(const TProxyEntryPtr& proxy, TInstant at) const;
    bool IsUnavailable(TInstant at) const;

    TDuration GetDeathAge() const;

    //! Raised when proxy role changes.
    DEFINE_SIGNAL(void(const std::string&), OnSelfRoleChanged);

private:
    const TCoordinatorConfigPtr Config_;
    const NTracing::TSamplerPtr Sampler_;
    TBootstrap* const Bootstrap_;
    const NApi::IClientPtr Client_;
    const NConcurrency::TPeriodicExecutorPtr UpdateStateExecutor_;

    ICypressRegistrarPtr CypressRegistrar_;

    const NProfiling::TGauge BannedGauge_ = HttpProxyProfiler().Gauge("/banned");

    const TPromise<void> FirstUpdateIterationFinished_ = NewPromise<void>();

    // @gearonixx @@custom
    // the "self" proxy
    TAtomicIntrusivePtr<TCoordinatorProxy> Self_;

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, ProxiesLock_);
    std::vector<TCoordinatorProxyPtr> Proxies_;

    TInstant StatisticsUpdatedAt_;
    std::optional<TNetworkStatistics> LastStatistics_;
    std::atomic<TInstant> AvailableAt_;

    // TODO(aleksandra-zh): replace with the time read-only mode was entered.
    std::atomic<bool> MastersInReadOnly_ = false;

    void UpdateReadOnly();

    void UpdateState();
    std::vector<TCoordinatorProxyPtr> ListCypressProxies();
    std::vector<TCoordinatorProxyPtr> ListProxies(
        const std::optional<std::string>& role,
        bool includeDeadAndBanned = false);

    TLivenessPtr GetSelfLiveness();

    TCoordinatorProxyPtr GetSelf() const;
    void SetSelf(TCoordinatorProxyPtr self);
};

DEFINE_REFCOUNTED_TYPE(TCoordinator)

////////////////////////////////////////////////////////////////////////////////

class THostsHandler
    : public NHttp::IHttpHandler
{
public:
    explicit THostsHandler(TCoordinatorPtr coordinator);

    void HandleRequest(
        const NHttp::IRequestPtr& req,
        const NHttp::IResponseWriterPtr& rsp) override;

private:
    const TCoordinatorPtr Coordinator_;
};

DEFINE_REFCOUNTED_TYPE(THostsHandler)

////////////////////////////////////////////////////////////////////////////////

class TClusterConnectionHandler
    : public NHttp::IHttpHandler
{
public:
    explicit TClusterConnectionHandler(NApi::IClientPtr client);

    void HandleRequest(
        const NHttp::IRequestPtr& req,
        const NHttp::IResponseWriterPtr& rsp) override;

private:
    const NApi::IClientPtr Client_;
};

DEFINE_REFCOUNTED_TYPE(TClusterConnectionHandler)

////////////////////////////////////////////////////////////////////////////////

class TPingHandler
    : public NHttp::IHttpHandler
{
public:
    // ● Потому что инфа «забанен я / в read-only / liveness просрочен» живёт именно в координаторе — он один следит за Cypress и за своим состоянием
    // @gearonixx @@DI
    explicit TPingHandler(TCoordinatorPtr coordinator);

    void HandleRequest(
        const NHttp::IRequestPtr& req,
        const NHttp::IResponseWriterPtr& rsp) override;

private:
    // dependeccyn
    const TCoordinatorPtr Coordinator_;
};

DEFINE_REFCOUNTED_TYPE(TPingHandler)

////////////////////////////////////////////////////////////////////////////////

class TDiscoverVersionsHandler
    : public NHttp::IHttpHandler
    , public TComponentDiscoverer
{
public:
    TDiscoverVersionsHandler(NApi::IClientPtr client, TComponentDiscoveryOptions componentDiscoveryOptions);

    void HandleRequest(
        const NHttp::IRequestPtr& req,
        const NHttp::IResponseWriterPtr& rsp) override;
};

DEFINE_REFCOUNTED_TYPE(TDiscoverVersionsHandler)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
