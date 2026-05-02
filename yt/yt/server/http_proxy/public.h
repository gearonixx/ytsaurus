#pragma once

#include <yt/yt/core/misc/public.h>

namespace NYT::NHttpProxy {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TBootstrap)


DECLARE_REFCOUNTED_STRUCT(TLiveness)
//
// ● Запись об одной прокси в реестре //sys/proxies/<host>: её endpoint, роль, флаг бана и Liveness (CPU, load, число запросов). YSON-структура — сериализуется в Cypress и читается оттуда другими прокси, чтобы
//   знать состояние соседей.

    //
// ● TProxyEntry — то, что лежит в Cypress (сериализуемое: endpoint, role, liveness, бан). TCoordinatorProxy — рантайм-обёртка над ним для координатора: добавляет Dampening (счётчик-дампфер для балансировки,
  // чтобы запросы не сыпались подряд в одну прокси).



    // ● Каждый раз, когда координатор отдал клиенту прокси X через AllocateProxyEntry, он бампит X.Dampening++. В формуле «фитнеса» это слагаемое со штрафом — в следующий раз X будет менее привлекательной, выберут
    //   другую. Так нагрузка размазывается, а не валится в одну прокси, у которой случайно оказался самый низкий load average.


    // ● Ни тот, ни другой — реальная прокси это процесс. Обе — описания: TProxyEntry это её паспорт в Cypress, TCoordinatorProxy — её карточка в памяти координатора.

//     Каждый раз, когда координатор отдал клиенту прокси X через AllocateProxyEntry, он бампит X.Dampening++. В формуле «фитнеса» это слагаемое со штрафом — в следующий раз X будет менее привлекательной,
//   выберут
//     другую. Так нагрузка размазывается, а не валится в одну прокси, у которой случайно оказался самый низкий load average.
//
//   те это просто счетчик? мол сколько раз он отдал прокси лкиенту
//
// ● Да, просто счётчик «сколько раз я недавно тебя выбрал». Влияет на следующие выборы через вес DampeningWeight в формуле.


DECLARE_REFCOUNTED_STRUCT(TProxyEntry)
DECLARE_REFCOUNTED_STRUCT(TCoordinatorProxy)

DECLARE_REFCOUNTED_STRUCT(IAccessChecker)

DECLARE_REFCOUNTED_STRUCT(TProxyBootstrapConfig)
DECLARE_REFCOUNTED_STRUCT(TProxyProgramConfig)
DECLARE_REFCOUNTED_STRUCT(TProxyDynamicConfig)
DECLARE_REFCOUNTED_STRUCT(TCoordinatorConfig)
DECLARE_REFCOUNTED_STRUCT(TSolomonProxyConfig)
DECLARE_REFCOUNTED_STRUCT(TProfilingEndpointProviderConfig)
DECLARE_REFCOUNTED_STRUCT(TFramingConfig)
DECLARE_REFCOUNTED_STRUCT(TTracingConfig)
DECLARE_REFCOUNTED_STRUCT(TApiConfig)
DECLARE_REFCOUNTED_STRUCT(TApiDynamicConfig)
DECLARE_REFCOUNTED_STRUCT(TAccessCheckerConfig)
DECLARE_REFCOUNTED_STRUCT(TAccessCheckerDynamicConfig)
DECLARE_REFCOUNTED_STRUCT(TMemoryLimitRatiosConfig)
DECLARE_REFCOUNTED_STRUCT(TMemoryLimitsConfig)

DECLARE_REFCOUNTED_STRUCT(IDynamicConfigManager)

DECLARE_REFCOUNTED_CLASS(TApi)
DECLARE_REFCOUNTED_CLASS(TCoordinator)
DECLARE_REFCOUNTED_CLASS(THostsHandler)
DECLARE_REFCOUNTED_CLASS(TClusterConnectionHandler)
DECLARE_REFCOUNTED_CLASS(TProxyHeapUsageProfiler)
DECLARE_REFCOUNTED_CLASS(TPingHandler)
DECLARE_REFCOUNTED_CLASS(TDiscoverVersionsHandler)
DECLARE_REFCOUNTED_CLASS(THttpAuthenticator)
DECLARE_REFCOUNTED_CLASS(TCompositeHttpAuthenticator)

DECLARE_REFCOUNTED_CLASS(TContext)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
