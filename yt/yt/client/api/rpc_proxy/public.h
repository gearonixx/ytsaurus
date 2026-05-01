#pragma once

#include <yt/yt/client/api/public.h>

namespace NYT::NApi::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IRowStreamEncoder)
DECLARE_REFCOUNTED_STRUCT(IRowStreamDecoder)

DECLARE_REFCOUNTED_STRUCT(TConnectionConfig)

DECLARE_REFCOUNTED_CLASS(TClusterDirectory)
DECLARE_REFCOUNTED_CLASS(TClientDirectory)

extern const std::string ApiServiceName;
extern const std::string DiscoveryServiceName;

constexpr int CurrentWireFormatVersion = 1;

////////////////////////////////////////////////////////////////////////////////

// COMPAT(babenko): get rid of this in favor of NRpc::EErrorCode::PeerBanned
YT_DEFINE_ERROR_ENUM(
    ((ProxyBanned) (2100))
);

DEFINE_ENUM(ERpcProxyFeature,
    ((GetInSyncWithoutKeys)(0))
    ((WideLocks)           (1))
);

////////////////////////////////////////////////////////////////////////////////
/// @gearonixx
///
/// TVM по сути и работает как файрвол на уровне приложений между микросервисами,
/// только вместо IP-адресов и портов он оперирует идентичностями сервисов.
///
///  но в современной инфраструктуре с динамическими подами,
///  общими кластерами и оркестраторами IP уже ничего не значит, любой сервис в кластере может подключиться к любому другому по сети.
///
///  TVM закрывает эту дыру на уровень выше: даже если злоумышленник попал внутрь сети, без валидного тикета с правильным src→dst он никуда не пройдёт.

DEFINE_ENUM(EAddressType,
    ((InternalRpc)        (0))
    ((MonitoringHttp)     (1))
    ((TvmOnlyInternalRpc) (2))
    ((Http)               (3))
    ((Https)              (4))
    ((TvmOnlyHttp)        (5))
    ((TvmOnlyHttps)       (6))
    ((PublicRpc)          (7))
    ((ChytHttp)           (8))
    ((ChytHttps)          (9))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy
