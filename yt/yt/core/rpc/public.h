#pragma once

#include <yt/yt/core/misc/configurable_singleton_decl.h>

#include <yt/yt/core/actions/callback.h>

#include <yt/yt/core/concurrency/public.h>

#include <yt/yt/core/bus/public.h>

#include <library/cpp/yt/misc/guid.h>

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

// @gearonixx proto-сообщение запроса Discover — служебный ping/reflection: "ты живой? какие методы поддерживаешь?"
class TReqDiscover;
// @gearonixx proto-ответ на Discover — содержит up-флаг и список поддерживаемых suggestion-ов/features
class TRspDiscover;
// @gearonixx мета-заголовок исходящего запроса: service, method, request_id, user, timeout, tracing и т.д.
class TRequestHeader;
// @gearonixx мета-заголовок ответа: request_id, ошибки, codec, формат
class TResponseHeader;
// @gearonixx proto-extension к header-у с user credentials (ticket, token, ...) для аутентификации
class TCredentialsExt;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

// @gearonixx параметры стримового RPC: размер окна, период ack-ов, read-timeout
struct TStreamingParameters;
// @gearonixx кусок данных, пересылаемый по streaming-каналу (sequence number + payload + attachments)
struct TStreamingPayload;
// @gearonixx обратная связь reader→writer: сколько байт прочитано, чтобы writer мог продвинуть окно
struct TStreamingFeedback;

// @gearonixx описатель сервиса (имя, namespace, версия протокола) — используется при регистрации сервиса
struct TServiceDescriptor;
// @gearonixx описатель метода сервиса (имя, очередь, handler, options) — строится через builder в TServiceBase
struct TMethodDescriptor;

// @gearonixx очередь входящих запросов для одного метода/пользователя — отвечает за порядок исполнения и throttling
DECLARE_REFCOUNTED_CLASS(TRequestQueue)

// @gearonixx стратегия выбора TRequestQueue для конкретного запроса (по user-у, по shard-у и т.п.)
DECLARE_REFCOUNTED_STRUCT(IRequestQueueProvider)
// @gearonixx реализация провайдера: отдельная очередь на каждого пользователя (изоляция нагрузок между users)
DECLARE_REFCOUNTED_CLASS(TPerUserRequestQueueProvider);

// @gearonixx callback, выбирающий IInvoker для обработки запроса по содержимому request header-а
using TInvokerProvider = TCallback<IInvokerPtr(const NRpc::NProto::TRequestHeader&)>;

// @gearonixx базовый класс client-side запроса (untyped) — хранит header, attachments, опции
DECLARE_REFCOUNTED_CLASS(TClientRequest)
// @gearonixx базовый класс client-side ответа (untyped) — десериализует body и attachments
DECLARE_REFCOUNTED_CLASS(TClientResponse)

// @gearonixx типизованная обёртка над TClientRequest — proto-message TRequestMessage задаёт схему body
template <class TRequestMessage, class TResponse>
class TTypedClientRequest;

class TClientResponse;

// @gearonixx типизованный ответ — десериализует body в proto-message TResponseMessage
template <class TResponseMessage>
class TTypedClientResponse;

// @gearonixx пара (ServiceName, RealmId), однозначно идентифицирует сервис на сервере
struct TServiceId;

// @gearonixx вход для аутентификатора: headers, ticket, user agent, адрес peer-а
struct TAuthenticationContext;
// @gearonixx результат аутентификации, "кто запрос делает": user, realm, user-ticket
struct TAuthenticationIdentity;
// @gearonixx обогащённый результат аутентификации (identity + опциональные доп.данные вроде tvm scopes)
struct TAuthenticationResult;

// @gearonixx публичный интерфейс отправляемого client-side запроса (Send, Cancel, Serialize…)
DECLARE_REFCOUNTED_STRUCT(IClientRequest)
// @gearonixx handle для управления запросом в полёте: Cancel(), SendStreamingPayload и т.п.
DECLARE_REFCOUNTED_STRUCT(IClientRequestControl)
// @gearonixx callback, в который пришёл ответ/ошибка — реализуется TClientResponse-ами
DECLARE_REFCOUNTED_STRUCT(IClientResponseHandler)
// @gearonixx RPC-сервер: слушает BUS, маршрутизирует входящие сообщения в зарегистрированные сервисы
DECLARE_REFCOUNTED_STRUCT(IServer)
// @gearonixx один RPC-сервис со своими методами — реализуется наследниками TServiceBase
DECLARE_REFCOUNTED_STRUCT(IService)
// @gearonixx IService + поддержка reflection (Discover) и описаний методов наружу
DECLARE_REFCOUNTED_STRUCT(IServiceWithReflection)
// @gearonixx server-side контекст обработки конкретного входящего запроса (Reply, attachments, identity, ...)
DECLARE_REFCOUNTED_STRUCT(IServiceContext)
// @gearonixx клиентский канал к одному peer-у — точка отправки RPC через TClientRequest::Invoke
DECLARE_REFCOUNTED_STRUCT(IChannel)
// @gearonixx IChannel-обёртка с локальным rate-limit-ом (ограничение исходящего RPS)
DECLARE_REFCOUNTED_STRUCT(IThrottlingChannel)
// @gearonixx фабрика IChannel-ов по адресу (TCP host:port или endpoint set)
DECLARE_REFCOUNTED_STRUCT(IChannelFactory)
// @gearonixx абстракция канала, у которого peer может меняться "под капотом" (service discovery, balancing)
DECLARE_REFCOUNTED_STRUCT(IRoamingChannelProvider)
// @gearonixx проверяет credentials из TAuthenticationContext и возвращает identity (или ошибку)
DECLARE_REFCOUNTED_STRUCT(IAuthenticator)
// @gearonixx кеш ответов на мутирующие запросы (по mutation_id) для идемпотентных ретраев
DECLARE_REFCOUNTED_STRUCT(IResponseKeeper)
// @gearonixx следит за метриками нагрузки сервера и решает, какие методы тормозить/дропать
DECLARE_REFCOUNTED_STRUCT(IOverloadController)

// @gearonixx client-side контекст отправки одного запроса (timeout, request_id, multiplexing band и т.п.)
DECLARE_REFCOUNTED_CLASS(TClientContext)
// @gearonixx базовый класс для пользовательских сервисов: регистрация методов, диспетчеризация
DECLARE_REFCOUNTED_CLASS(TServiceBase)
// @gearonixx общий decorator над IChannel — удобно для написания "прозрачных" обёрток
DECLARE_REFCOUNTED_CLASS(TChannelWrapper)
// @gearonixx фабрика, у которой mapping address → channel прописан явно (для тестов и фиксированных пиров)
DECLARE_REFCOUNTED_CLASS(TStaticChannelFactory)
// @gearonixx прокси-control: можно вызывать Cancel ещё до того, как реальный TIClientRequestControl создан
DECLARE_REFCOUNTED_CLASS(TClientRequestControlThunk)
// @gearonixx фабрика с LRU-кешем уже созданных каналов (повторное использование TCP-соединения)
DECLARE_REFCOUNTED_CLASS(TCachingChannelFactory)
// @gearonixx flow-control: динамически регулирует размер окна по сигналам перегрузки
DECLARE_REFCOUNTED_CLASS(TCongestionController)

// @gearonixx стрим входящих attachment-ов на сервере (server reader / client writer side)
DECLARE_REFCOUNTED_CLASS(TAttachmentsInputStream)
// @gearonixx стрим исходящих attachment-ов (server writer / client reader side)
DECLARE_REFCOUNTED_CLASS(TAttachmentsOutputStream)

// @gearonixx абстрактный источник приоритетов peer-ов для балансировщика (меньше число — выше приоритет)
DECLARE_REFCOUNTED_STRUCT(IPeerPriorityProvider)
// @gearonixx реализация поверх явной map-ы address → priority
DECLARE_REFCOUNTED_STRUCT(IMapPeerPriorityProvider)
// @gearonixx реестр "живых" peer-ов: добавление/удаление, выбор подходящего по hash/random/priority
DECLARE_REFCOUNTED_STRUCT(IViablePeerRegistry)
// @gearonixx хук, который можно вставить в запросы Discover (например, добавить proto-extension)
DECLARE_REFCOUNTED_STRUCT(IDiscoverRequestHook)
// @gearonixx алгоритм опроса peer-ов через Discover для обновления списка живых
DECLARE_REFCOUNTED_STRUCT(IPeerDiscovery)
// @gearonixx динамический пул каналов с автоматическим discovery и rebalancing
DECLARE_REFCOUNTED_CLASS(TDynamicChannelPool)

// @gearonixx шаблонный server-side контекст, параметризованный конкретными proto-message типами запроса/ответа
template <
    class TServiceContext,
    class TServiceContextWrapper,
    class TRequestMessage,
    class TResponseMessage
>
class TGenericTypedServiceContext;

// @gearonixx опции вызова handler-а: heavy/light, request/response codec, pooled storage
struct THandlerInvocationOptions;

// @gearonixx decorator-обёртка над IServiceContext: позволяет переопределить отдельные методы
class TServiceContextWrapper;

// @gearonixx удобный алиас типизованного контекста для обычного IServiceContext + TServiceContextWrapper
template <class TRequestMessage, class TResponseMessage>
using TTypedServiceContext = TGenericTypedServiceContext<
    IServiceContext,
    TServiceContextWrapper,
    TRequestMessage,
    TResponseMessage
>;

////////////////////////////////////////////////////////////////////////////////

// @gearonixx параметры экспоненциальной сетки границ histogram-а (min, base, count)
DECLARE_REFCOUNTED_STRUCT(THistogramExponentialBounds)
// @gearonixx config histogram-а времени (границы + единицы)
DECLARE_REFCOUNTED_STRUCT(TTimeHistogramConfig)
// @gearonixx config RPC-сервера (адреса, tracing, default-таймауты)
DECLARE_REFCOUNTED_STRUCT(TServerConfig)
// @gearonixx общая часть config-а сервиса, применяемая ко всем сервисам сервера
DECLARE_REFCOUNTED_STRUCT(TServiceCommonConfig)
// @gearonixx runtime-переконфигурируемая часть сервера (можно менять без рестарта)
DECLARE_REFCOUNTED_STRUCT(TServerDynamicConfig)
// @gearonixx runtime-переконфигурируемая общая часть для всех сервисов
DECLARE_REFCOUNTED_STRUCT(TServiceCommonDynamicConfig)
// @gearonixx конфиг одного сервиса (включает per-method config-и)
DECLARE_REFCOUNTED_STRUCT(TServiceConfig)
// @gearonixx конфиг одного метода (queue size, throttle, concurrency)
DECLARE_REFCOUNTED_STRUCT(TMethodConfig)
// @gearonixx политика ретраев на стороне клиента (backoff, max attempts)
DECLARE_REFCOUNTED_STRUCT(TRetryingChannelConfig)
// @gearonixx конфиг IViablePeerRegistry (max peers, hashing policy)
DECLARE_REFCOUNTED_STRUCT(TViablePeerRegistryConfig)
// @gearonixx конфиг динамического пула каналов (период discovery, размер пула)
DECLARE_REFCOUNTED_STRUCT(TDynamicChannelPoolConfig)
// @gearonixx настройки endpoints из service discovery (cluster, endpoint set id)
DECLARE_REFCOUNTED_STRUCT(TServiceDiscoveryEndpointsConfig)
// @gearonixx общая основа balancing-channel конфига (peers, discovery period)
DECLARE_REFCOUNTED_STRUCT(TBalancingChannelConfigBase)
// @gearonixx конкретный конфиг балансирующего канала (стратегия выбора peer-а)
DECLARE_REFCOUNTED_STRUCT(TBalancingChannelConfig)
// @gearonixx конфиг локального rate-limit-а на исходящие RPC (RPS, период)
DECLARE_REFCOUNTED_STRUCT(TThrottlingChannelConfig)
// @gearonixx динамическая часть throttling-конфига (можно менять на ходу)
DECLARE_REFCOUNTED_STRUCT(TThrottlingChannelDynamicConfig)
// @gearonixx конфиг response-keeper-а (expiration time, eviction)
DECLARE_REFCOUNTED_STRUCT(TResponseKeeperConfig)
// @gearonixx глобальные настройки RPC dispatcher-а (thread pools, multiplexing bands)
DECLARE_REFCOUNTED_STRUCT(TDispatcherConfig)
// @gearonixx динамически переконфигурируемая часть dispatcher-а
DECLARE_REFCOUNTED_STRUCT(TDispatcherDynamicConfig)
// @gearonixx привязка метода сервиса к tracker-у перегрузки (по какой метрике throttle-ить)
DECLARE_REFCOUNTED_STRUCT(TOverloadTrackedServiceMethodConfig)
// @gearonixx tracker перегрузки по среднему wait-time в очереди
DECLARE_REFCOUNTED_STRUCT(TOverloadTrackerMeanWaitTimeConfig)
// @gearonixx tracker перегрузки по заполненности backlog-очереди
DECLARE_REFCOUNTED_STRUCT(TOverloadTrackerBacklogQueueFillFractionConfig)
// @gearonixx общий конфиг IOverloadController (список tracker-ов и привязки к методам)
DECLARE_REFCOUNTED_STRUCT(TOverloadControllerConfig)

// @gearonixx пара throttler-конфигов для очереди запросов: по "весу" запросов и по байтам
struct TRequestQueueThrottlerConfigs
{
    NConcurrency::TThroughputThrottlerConfigPtr WeightThrottlerConfig;
    NConcurrency::TThroughputThrottlerConfigPtr BytesThrottlerConfig;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx общий "mixin" с опциональным таймаутом — встраивается в request options builder-ом
struct TTimeoutOptions
{
    std::optional<TDuration> Timeout;
};

////////////////////////////////////////////////////////////////////////////////
/// @gearonixx
/// Это перечисление приоритетных «полос» для YT-RPC (Bus): когда по одному TCP-соединению идёт много запросов, они разделяются на band'ы
/// и обрабатываются с разной…Это перечисление приоритетных «полос» для YT-RPC (Bus): когда по одному TCP-соединению идёт много запросов,
/// они разделяются на band'ы и обрабатываются с разной приоритетностью/изоляцией, чтобы тяжёлый трафик не давил лёгкий.

// @gearonixx импорт enum-а multiplexing band-ов из BUS (Default/Control/Heavy/RealTime) — приоритет в сети
using NBus::EMultiplexingBand;

// @gearonixx уникальный GUID каждого RPC-запроса; пробрасывается в логи/tracing на обеих сторонах
using TRequestId = TGuid;
extern const TRequestId NullRequestId;

// @gearonixx GUID "realm-а" — отдельного namespace-а сервисов на одном сервере (например, разные shard-ы)
using TRealmId = TGuid;
extern const TRealmId NullRealmId;

// @gearonixx GUID мутации — нужен, чтобы при ретрае идемпотентного запроса response-keeper отдал кешированный ответ
using TMutationId = TGuid;
extern const TMutationId NullMutationId;

// @gearonixx захардкоженное имя root-пользователя ("root") — обходит ACL-проверки
extern const std::string RootUserName;

// @gearonixx ожидаемое число частей в TSharedRefArray RPC-сообщения — для small-buffer оптимизации
constexpr int TypicalMessagePartCount = 8;

// COMPAT(nadya02): remove it when all timeouts are set
// @gearonixx заглушечный "огромный" таймаут — используется там, где ещё не проставили нормальный timeout
constexpr TDuration HugeDoNotUseRpcRequestTimeout = TDuration::Hours(24);

// @gearonixx указатель на функцию, которая преобразует numeric featureId сервиса в человекочитаемое имя
using TFeatureIdFormatter = const std::function<std::optional<TStringBuf>(int featureId)>*;

////////////////////////////////////////////////////////////////////////////////

// @gearonixx ключи стандартных tracing-аннотаций, которыми RPC помечает span-ы в jaeger/трейсах
extern const std::string RequestIdAnnotation;
extern const std::string EndpointAnnotation;
extern const std::string EndpointAddressAnnotation;
extern const std::string RequestInfoAnnotation;
extern const std::string RequestUser;
extern const std::string ResponseInfoAnnotation;

// @gearonixx ключи атрибутов ошибки, в которые кладётся id/имя неподдержанной feature при UnsupportedClientFeature/UnsupportedServerFeature
extern const std::string FeatureIdAttributeKey;
extern const std::string FeatureNameAttributeKey;

////////////////////////////////////////////////////////////////////////////////

// @gearonixx все коды ошибок RPC-уровня; код прилетает в TError и используется клиентом для retry-логики
YT_DEFINE_ERROR_ENUM(
    ((TransportError)               (static_cast<int>(NBus::EErrorCode::TransportError)))
    ((ProtocolError)                (101))
    ((NoSuchService)                (102))
    ((NoSuchMethod)                 (103))
    ((Unavailable)                  (105)) // The server is not capable of serving requests and
                                           // must not receive any more load.
    ((TransientFailure)             (116)) // Similar to Unavailable but indicates a transient issue,
                                           // which can be safely retried.
    ((PoisonPill)                   (106)) // The client must die upon receiving this error.
    ((RequestQueueSizeLimitExceeded)(108))
    ((AuthenticationError)          (109))
    ((InvalidCsrfToken)             (110))
    ((InvalidCredentials)           (111))
    ((StreamingNotSupported)        (112))
    ((UnsupportedClientFeature)     (113))
    ((UnsupportedServerFeature)     (114))
    ((PeerBanned)                   (115)) // The server is explicitly banned and thus must be dropped.
    ((NoSuchRealm)                  (117))
    ((Overloaded)                   (118)) // The server is currently overloaded and unable to handle additional requests.
                                           // The client should try to reduce their request rate until the server has had a chance to recover.
    ((SslError)                     (static_cast<int>(NBus::EErrorCode::SslError)))
    ((RequestMemoryPressure)        (120)) // There is no enough memory to handle RPC request.
    ((GlobalDiscoveryError)         (121)) // Single peer discovery interrupts discovery session.
    ((ResponseMemoryPressure)       (122)) // There is no enough memory to handle RPC response.
);

// @gearonixx формат сериализации body RPC-сообщения: бинарный protobuf по умолчанию, JSON/YSON — для HTTP-прокси
DEFINE_ENUM(EMessageFormat,
    ((Protobuf)    (0))
    ((Json)        (1))
    ((Yson)        (2))
);

// @gearonixx регистрирует TDispatcherConfig как глобальный singleton с возможностью динамической реконфигурации
YT_DECLARE_RECONFIGURABLE_SINGLETON(TDispatcherConfig, TDispatcherDynamicConfig);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc
