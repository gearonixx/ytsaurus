#include "bootstrap.h"

#include "access_checker.h"
#include "config.h"
#include "coordinator.h"
#include "dynamic_config_manager.h"
#include "api.h"
#include "http_authenticator.h"
#include "private.h"
#include "solomon_proxy.h"

#include <yt/yt/server/http_proxy/clickhouse/handler.h>
#include <yt/yt/server/http_proxy/profilers.h>

#include <yt/yt/server/lib/admin/admin_service.h>

#include <yt/yt/server/lib/misc/bootstrap.h>

#include <yt/yt/server/lib/signature/components.h>

#include <yt/yt/library/disk_manager/hotswap_manager.h>

#include <yt/yt/library/coredumper/public.h>

#include <yt/yt/ytlib/api/native/config.h>
#include <yt/yt/ytlib/api/native/connection.h>
#include <yt/yt/ytlib/api/native/helpers.h>

#include <yt/yt/ytlib/cell_master_client/cell_directory_synchronizer.h>

#include <yt/yt/ytlib/hive/cluster_directory_synchronizer.h>

#include <yt/yt/ytlib/misc/memory_usage_tracker.h>

#include <yt/yt/ytlib/node_tracker_client/node_directory_synchronizer.h>

#include <yt/yt/ytlib/queue_client/registration_manager.h>

#include <yt/yt/library/orchid/orchid_service.h>

#include <yt/yt/library/auth_server/authentication_manager.h>
#include <yt/yt/library/auth_server/config.h>
#include <yt/yt/library/auth_server/cypress_cookie_login.h>
#include <yt/yt/library/auth_server/cypress_cookie_manager.h>

#include <yt/yt/library/monitoring/http_integration.h>
#include <yt/yt/library/monitoring/monitoring_manager.h>

#include <yt/yt/library/profiling/solomon/proxy.h>
#include <yt/yt/library/profiling/solomon/registry.h>
#include <yt/yt/library/profiling/solomon/exporter.h>

#include <yt/yt/library/program/build_attributes.h>
#include <yt/yt/library/program/helpers.h>

#include <yt/yt/library/fusion/service_locator.h>

#include <yt/yt/client/driver/driver.h>
#include <yt/yt/client/driver/config.h>

#include <yt/yt/client/logging/dynamic_table_log_writer.h>

#include <yt/yt/core/bus/tcp/server.h>

#include <yt/yt/core/concurrency/thread_pool_poller.h>
#include <yt/yt/core/concurrency/thread_pool.h>

#include <yt/yt/core/http/helpers.h>
#include <yt/yt/core/http/server.h>

#include <yt/yt/core/https/config.h>
#include <yt/yt/core/https/server.h>

#include <yt/yt/core/misc/ref_counted_tracker_statistics_producer.h>
#include <yt/yt/core/misc/ref_counted_tracker.h>
#include <yt/yt/core/misc/configurable_singleton_def.h>

#include <yt/yt/core/rpc/bus/server.h>

#include <yt/yt/core/bus/server.h>
#include <yt/yt/core/bus/tcp/config.h>

#include <yt/yt/core/ytree/fluent.h>
#include <yt/yt/core/ytree/virtual.h>
#include <yt/yt/core/ytree/ypath_client.h>

#include <yt/yt/build/build.h>

namespace NYT::NHttpProxy {
    // @gearonixx
    //  В коде, где из 30 разных подсистем YT используется по 5-10 типов, это огромная разница в читаемости.
    // using namespace NApi; говорит компилятору: «когда увидишь IClient без префикса, попробуй найти его ещё и в NApi::». Это подсказка для разрешения имён на этапе парсинга, не более.
using namespace NApi;
using namespace NAuth;
using namespace NConcurrency;
using namespace NDriver;
using namespace NHttp;
using namespace NMonitoring;
using namespace NNative;
using namespace NOrchid;
using namespace NProfiling;
using namespace NSignature;
using namespace NYson;
using namespace NYTree;
using namespace NAdmin;
using namespace NFusion;

////////////////////////////////////////////////////////////////////////////////

    // @gearonixx contsinit???

constinit const auto Logger = HttpProxyLogger;

////////////////////////////////////////////////////////////////////////////////

// Поэтому http-proxy и нужны Acceptor_ (принимать новые TCP-подключения от клиентов) и Poller_ (читать/писать байты по уже принятым подключениям через epoll). Это I/O-фундамент любого HTTP-сервера, "HTTP" — это просто формат байтов, которые по этим сокетам летят.

// @gearonixx @@http_proxy
// HTTP — это прикладной протокол поверх TCP. TCP-соединение = сокет
//  http-proxy, под капотом происходит ровно это: клиент открывает TCP-коннект к порту прокси (для ядра это сокет), по нему пишет байты HTTP-запроса, прокси читает байты, парсит их как HTTP, обрабатывает, пишет байты ответа обратно в тот же сокет
TBootstrap::TBootstrap(
    TProxyBootstrapConfigPtr config,
    // сырое YSON-дерево (нужно когда какой-то компонент хочет вытащить свою секцию сам или конфиг динамический
    INodePtr configNode,
    //  реестр синглтонов/сервисов процесса, через который компоненты находят друг друга без жёстких зависимостей в конструкторах.
    // реестр для получения синглтонов процесса по типу, чтобы не тащить их явно через конструкторы.

    // "Процесс" здесь — это конкретный бинарник ytserver-http-proxy, запущенный как один OS-процесс.

    // ServiceLocator нужен потому, что часть синглтонов (
    // Solomon exporter, core dumper, hotswap manager) живёт на уровне
    // процесса и инициализируется до бутстрапа конкретной роли — http-proxy просто достаёт их по типу, а не пересоздаёт.

    // (Connection к мастеру, серверы на разных портах, координатор, аутентификацию, drivers v3/v4 и т.д.
    // короче аналог RequestContext из userver
    IServiceLocatorPtr serviceLocator)
    // Параметры приходят по значению (TProxyBootstrapConfigPtr config — это TIntrusivePtr), std::move переносит их в поля без лишнего инкремента/декремента счётчика ссылок
    // . Стандартный паттерн "sink parameter": берёшь по значению — мувай в поле.
    : Config_(std::move(config))
    , ConfigNode_(std::move(configNode))
    , ServiceLocator_(std::move(serviceLocator))
    //  однопоточная очередь действий ("control thread"), куда сериализуются все управляющие операции бутстрапа
    //  (init, реакция на смену динамического конфига, реконфиги) — гарантирует, что состояние меняется в одном потоке без локов.
    , Control_(New<TActionQueue>("Control"))
    // Один поток с epoll спокойно тянет десятки тысяч соединений: он спит в epoll_wait, просыпается на готовом сокете, обрабатывает событие, снова спит.
    , Poller_(CreateThreadPoolPoller(Config_->ThreadCount, "Poller"))
    // Когда клиент стучится к серверу, ядро ставит его в очередь. `accept()` — это "забрать следующего из очереди и начать с ним общаться". Без `accept()` клиент висит и ждёт.
    , Acceptor_(CreateThreadPoolPoller(1, "Acceptor"))
{
    // TODO(gepardo): Pass native authenticator here.
    if (Config_->AbortOnUnrecognizedOptions) {
        AbortOnUnrecognizedOptions(Logger(), Config_);
    } else {
        WarnForUnrecognizedOptions(Logger(), Config_);
    }
}

TBootstrap::~TBootstrap() = default;

void TBootstrap::DoRun()
{
    DoInitialize();
    DoStart();
}

void TBootstrap::DoInitialize()
{
    MonitoringServer_ = NHttp::CreateServer(Config_->CreateMonitoringHttpServerConfig());

    // @gearonixx
    // IMapNode — узел YSON-дерева типа "map" (ключ → дочерний узел).

    // ит у себя структуру с текущим состоянием — какой конфиг загружен, какие соединения открыты,
    // какие задачи выполняются, счётчики.
    // Orchid делает эту структуру читаемой снаружи: ты с ноутбука выполняешь

    // !!!
    // Orchid — это просто способ заглянуть внутрь живого процесса через привычный интерфейс Cypress.
    IMapNodePtr orchidRoot;

    NMonitoring::Initialize(
        MonitoringServer_,
        ServiceLocator_->GetServiceOrThrow<NProfiling::TSolomonExporterPtr>(),
        &MonitoringManager_,
        &orchidRoot);

    SetBuildAttributes(
        orchidRoot,
        "http_proxy");

    NNative::TConnectionOptions connectionOptions;
    connectionOptions.RetryRequestQueueSizeLimitExceeded = Config_->RetryRequestQueueSizeLimitExceeded;
    connectionOptions.CreateQueueConsumerRegistrationManager = true;

    MemoryUsageTracker_ = CreateNodeMemoryTracker(
        Config_->MemoryLimits->Total.value_or(std::numeric_limits<i64>::max()),
        New<TNodeMemoryTrackerConfig>(),
        /*limits*/ {},
        Logger(),
        HttpProxyProfiler().WithPrefix("/memory_usage"),
        GetControlInvoker());

    Connection_ = CreateConnection(
        Config_->ClusterConnection,
        connectionOptions,
        /*clusterDirectoryOverride*/ {},
        MemoryUsageTracker_);

    SetupClusterConnectionDynamicConfigUpdate(
        Connection_,
        Config_->ClusterConnectionDynamicConfigPolicy,
        ConfigNode_->AsMap()->GetChildOrThrow("cluster_connection"),
        Logger());

    Connection_->GetClusterDirectorySynchronizer()->Start();
    Connection_->GetNodeDirectorySynchronizer()->Start();
    Connection_->GetQueueConsumerRegistrationManagerOrThrow()->StartSync();
    Connection_->GetMasterCellDirectorySynchronizer()->Start();
    SetupClients();

    Coordinator_ = New<TCoordinator>(Config_, this);

    auto setGlobalRoleTag = [] (const std::string& role) {
        TSolomonRegistry::Get()->SetDynamicTags({TTag{"proxy_role", role}});
    };
    setGlobalRoleTag(Coordinator_->GetSelfEntry()->Role);
    // BIND(f, args.BIND(f, args...) создаёт TCallback, который при вызове в будущем восстановит контекст момента создания:
    // текущий trace span (для распределённого трейсинга через Jaeger), logging tags (чтобы логи из коллбека имели те же теги, что и код, который его создал),
    // fiber-локальные переменные. Это нужно, когда коллбек логически продолжает текущую операцию в другом треде/файбере — например, ты постишь продолжение запроса в invoker, и хочешь, чтобы трейс не разорвался.


    // BIND_NO_PROPAGATE всё равно делает TCallback — тип YT, который умеет в weak/strong refs аргументов, складывается в IInvoker, подписывается на TCallbackList,
    // возвращается из TFuture::Subscribe и т.д.
    // Просто лямбду в эти места не сунешь — нужен именно TCallback.
    Coordinator_->SubscribeOnSelfRoleChanged(BIND_NO_PROPAGATE(setGlobalRoleTag));

    DynamicConfigManager_ = CreateDynamicConfigManager(this);
    DynamicConfigManager_->SubscribeBeforeConfigChanged(BIND_NO_PROPAGATE(&TBootstrap::OnDynamicConfigChanged, MakeWeak(this)));

    if (Config_->ExposeConfigInOrchid) {
        SetNodeByYPath(
            orchidRoot,
            "/config",
            CreateVirtualNode(ConfigNode_));
        SetNodeByYPath(
            orchidRoot,
            "/dynamic_config_manager",
            CreateVirtualNode(DynamicConfigManager_->GetOrchidService()));
        SetNodeByYPath(
            orchidRoot,
            "/cluster_connection",
            CreateVirtualNode(Connection_->GetOrchidService()));
    }
    if (auto hotswapManager = ServiceLocator_->FindService<NDiskManager::IHotswapManagerPtr>()) {
        SetNodeByYPath(
            orchidRoot,
            "/disk_monitoring",
            CreateVirtualNode(hotswapManager->GetOrchidService()));
    }

    Config_->BusServer->Port = Config_->RpcPort;
    BusServer_ = CreateBusServer(Config_->BusServer);
    RpcServer_ = NRpc::NBus::CreateBusServer(BusServer_);

    RpcServer_->RegisterService(CreateOrchidService(
        orchidRoot,
        GetControlInvoker(),
        /*authenticator*/ nullptr));

    RpcServer_->RegisterService(CreateAdminService(
        GetControlInvoker(),
        ServiceLocator_->FindService<NCoreDump::ICoreDumperPtr>(),
        /*authenticator*/ nullptr));

    HostsHandler_ = New<THostsHandler>(Coordinator_);
    ClusterConnectionHandler_ = New<TClusterConnectionHandler>(RootClient_);
    PingHandler_ = New<TPingHandler>(Coordinator_);
    DiscoverVersionsHandler_ = New<TDiscoverVersionsHandler>(
        RootClient_,
        TComponentDiscoveryOptions{.ProxyDeathAgeCallback = BIND(&TCoordinator::GetDeathAge, Coordinator_)});

    SolomonProxy_ = CreateSolomonProxy(
        Config_->SolomonProxy,
        TComponentDiscoveryOptions{.ProxyDeathAgeCallback = BIND(&TCoordinator::GetDeathAge, Coordinator_)},
        RootClient_,
        Poller_);

    ClickHouseHandler_ = New<NClickHouse::TClickHouseHandler>(this);
    ClickHouseHandler_->Start();

    AccessChecker_ = CreateAccessChecker(this);
    auto ownerId = TOwnerId(Coordinator_->GetSelfEntry()->Endpoint);
    SignatureComponents_ = New<TSignatureComponents>(
        Config_->SignatureComponents,
        std::move(ownerId),
        Connection_,
        GetControlInvoker());
    Connection_->SetSignatureGenerator(SignatureComponents_->GetSignatureGenerator());

    auto driverV3Config = CloneYsonStruct(Config_->Driver);
    driverV3Config->ApiVersion = ApiVersion3;
    DriverV3_ = CreateDriver(
        Connection_,
        driverV3Config,
        SignatureComponents_->GetSignatureValidator());

    auto driverV4Config = CloneYsonStruct(Config_->Driver);
    driverV4Config->ApiVersion = ApiVersion4;
    DriverV4_ = CreateDriver(
        Connection_,
        driverV4Config,
        SignatureComponents_->GetSignatureValidator());

    AuthenticationManager_ = CreateAuthenticationManager(
        Config_->Auth,
        Poller_,
        RootClient_);

    if (Config_->Auth->CypressCookieManager) {
        CypressCookieLoginHandler_ = CreateCypressCookieLoginHandler(
            Config_->Auth->CypressCookieManager->CookieGenerator,
            RootClient_,
            AuthenticationManager_->GetCypressCookieManager()->GetCookieStore());
    }

    auto httpAuthenticator = New<THttpAuthenticator>(
        this,
        Config_->Auth,
        AuthenticationManager_);
    THashMap<int, THttpAuthenticatorPtr> authenticators{{Config_->HttpServer->Port, httpAuthenticator}};

    if (Config_->HttpsServer) {
        authenticators[Config_->HttpsServer->Port] = httpAuthenticator;
    }

    THttpAuthenticatorPtr tvmOnlyAuthenticator = nullptr;
    if (Config_->TvmOnlyAuth) {
        TvmOnlyAuthenticationManager_ = CreateAuthenticationManager(
            Config_->TvmOnlyAuth,
            Poller_,
            RootClient_);
        tvmOnlyAuthenticator = New<THttpAuthenticator>(
            this,
            Config_->TvmOnlyAuth,
            TvmOnlyAuthenticationManager_);
    }

    if (Config_->TvmOnlyHttpServer) {
        authenticators[Config_->TvmOnlyHttpServer->Port] = tvmOnlyAuthenticator;
    }

    if (Config_->TvmOnlyHttpsServer) {
        authenticators[Config_->TvmOnlyHttpsServer->Port] = tvmOnlyAuthenticator;
    }

    if (Config_->ChytHttpServer) {
        authenticators[Config_->ChytHttpServer->Port] = httpAuthenticator;
    }

    if (Config_->ChytHttpsServer) {
        authenticators[Config_->ChytHttpsServer->Port] = httpAuthenticator;
    }

    HttpAuthenticator_ = New<TCompositeHttpAuthenticator>(authenticators);

    Api_ = New<TApi>(this);
    Config_->HttpServer->ServerName = "HttpApi";
    ApiHttpServer_ = NHttp::CreateServer(
        Config_->HttpServer,
        Poller_,
        Acceptor_);
    RegisterRoutes(ApiHttpServer_);

    if (Config_->HttpsServer) {
        Config_->HttpsServer->ServerName = "HttpsApi";
        ApiHttpsServer_ = NHttps::CreateServer(
            Config_->HttpsServer,
            Poller_,
            Acceptor_,
            GetControlInvoker());
        RegisterRoutes(ApiHttpsServer_);
    }

    if (Config_->TvmOnlyHttpServer) {
        Config_->TvmOnlyHttpServer->ServerName = "TvmOnlyHttpApi";
        TvmOnlyApiHttpServer_ = NHttp::CreateServer(
            Config_->TvmOnlyHttpServer,
            Poller_,
            Acceptor_);
        RegisterRoutes(TvmOnlyApiHttpServer_);
    }

    if (Config_->TvmOnlyHttpsServer) {
        Config_->TvmOnlyHttpsServer->ServerName = "TvmOnlyHttpsApi";
        TvmOnlyApiHttpsServer_ = NHttps::CreateServer(
            Config_->TvmOnlyHttpsServer,
            Poller_,
            Acceptor_,
            GetControlInvoker());
        // RegisterRoutes навешивает на сервер обработчики для всех путей API
        // — /api/, /auth/whoami, /hosts/, /ping/, /version, /query (ClickHouse) и т
        // .RegisterRoutes навешивает на сервер обработчики для всех путей API — /api/,
        // /auth/whoami, /hosts/, /ping/, /version, /query (ClickHouse) и т.д. Без него сервер бы стартовал, принимал соединения и отвечал 404 на всё.
        RegisterRoutes(TvmOnlyApiHttpsServer_);
    }

    if (Config_->ChytHttpServer) {
        Config_->ChytHttpServer->ServerName = "ChytHttpApi";
        ChytApiHttpServer_ = NHttp::CreateServer(Config_->ChytHttpServer, Poller_, Acceptor_);
        // Single handler.
        ChytApiHttpServer_->AddHandler("/", AllowCors(ClickHouseHandler_));
    }

    if (Config_->ChytHttpsServer) {
        Config_->ChytHttpsServer->ServerName = "ChytHttpsApi";
        ChytApiHttpsServer_ = NHttps::CreateServer(Config_->ChytHttpsServer, Poller_, Acceptor_, GetControlInvoker());
        ChytApiHttpsServer_->AddHandler("/", AllowCors(ClickHouseHandler_));
    }

    // ??
    SetNodeByYPath(
        orchidRoot,
        "/http_proxy",
        CreateVirtualNode(Api_->CreateOrchidService()));

    HttpProxyHeapUsageProfiler_ = New<TProxyHeapUsageProfiler>(
        GetControlInvoker(),
        Config_->HeapProfiler);
}

bool TBootstrap::IsChytApiServerAddress(const NNet::TNetworkAddress& address) const
{
    return (ChytApiHttpServer_ && address == ChytApiHttpServer_->GetAddress())
        || (ChytApiHttpsServer_ && address == ChytApiHttpsServer_->GetAddress());
}

void TBootstrap::SetupClients()
{
    auto options = NApi::TClientOptions::FromUser(NSecurityClient::RootUserName);
    RootClient_ = Connection_->CreateClient(options);

    NLogging::GetDynamicTableLogWriterFactory()->SetClient(RootClient_);
}

void TBootstrap::ReconfigureMemoryUsageTracker(
    i64 memoryLimit,
    const TMemoryLimitRatiosConfigPtr& memoryLimitRatios,
    const TNodeMemoryTrackerConfigPtr& newConfig)
{
    auto totalMemoryLimit = static_cast<i64>(memoryLimit * memoryLimitRatios->TotalMemoryLimitRatio);
    MemoryUsageTracker_->SetTotalLimit(totalMemoryLimit);

    auto heavyRequestMemoryLimit = static_cast<i64>(memoryLimit * memoryLimitRatios->HeavyRequestMemoryLimitRatio);
    MemoryUsageTracker_->SetCategoryLimit(
        EMemoryCategory::HeavyRequest,
        heavyRequestMemoryLimit);

    MemoryUsageTracker_->Reconfigure(newConfig);
}

void TBootstrap::OnDynamicConfigChanged(
    const TProxyDynamicConfigPtr& /*oldConfig*/,
    const TProxyDynamicConfigPtr& newConfig)
{
    TSingletonManager::Reconfigure(newConfig);

    i64 memoryLimit = Config_->MemoryLimits->Total.value_or(std::numeric_limits<i64>::max());
    if (newConfig->MemoryLimits->Total) {
        memoryLimit = *newConfig->MemoryLimits->Total;
    }

    auto role = Coordinator_->GetSelfEntry()->Role;

    ReconfigureMemoryUsageTracker(
        memoryLimit,
        GetOrDefault(
            newConfig->Api->RoleToMemoryLimitRatios,
            role,
            newConfig->Api->DefaultMemoryLimitRatios),
        newConfig->MemoryTracker);

    DynamicConfig_.Store(newConfig);

    BusServer_->OnDynamicConfigChanged(newConfig->BusServer);

    Coordinator_->GetTraceSampler()->UpdateConfig(newConfig->Tracing);

    if (newConfig->SignatureComponents) {
        YT_UNUSED_FUTURE(SignatureComponents_->Reconfigure(newConfig->SignatureComponents));
    }

    Connection_->GetMasterCellDirectorySynchronizer()->Reconfigure(
        newConfig->MasterCellDirectorySynchronizer.value_or(Config_->ClusterConnection->Static->MasterCellDirectorySynchronizer));
}

void TBootstrap::HandleRequest(
    const NHttp::IRequestPtr& req,
    const NHttp::IResponseWriterPtr& rsp)
{
    rsp->SetStatus(EStatusCode::OK);
    if (req->GetUrl().Path == "/service") {
        ReplyJson(rsp, [&] (NYson::IYsonConsumer* json) {
            BuildYsonFluently(json)
                .BeginMap()
                    .Item("start_time").Value(StartTime_)
                    .Item("version").Value(GetVersion())
                .EndMap();
        });
    } else {
        WaitFor(rsp->WriteBody(TSharedRef::FromString(GetVersion())))
            .ThrowOnError();
    }
}

TFuture<void> TBootstrap::Run()
{
    return BIND(&TBootstrap::DoRun, MakeStrong(this))
        .AsyncVia(GetControlInvoker())
        .Run();
}

void TBootstrap::DoStart()
{
    DynamicConfigManager_->Start();

    MonitoringServer_->Start();

    // NB(pavook):
    // We don't wait for key rotation completion anywhere in bootstrap, because proxy bootstrap
    // should be possible even in master read-only mode.
    // So, we just throw on all signature-requiring operations until the key rotation actually happens.
    YT_UNUSED_FUTURE(SignatureComponents_->StartRotation());

    ApiHttpServer_->Start();
    if (ApiHttpsServer_) {
        ApiHttpsServer_->Start();
    }
    if (TvmOnlyApiHttpServer_) {
        TvmOnlyApiHttpServer_->Start();
    }
    if (TvmOnlyApiHttpsServer_) {
        TvmOnlyApiHttpsServer_->Start();
    }
    if (ChytApiHttpServer_) {
        ChytApiHttpServer_->Start();
    }
    if (ChytApiHttpsServer_) {
        ChytApiHttpsServer_->Start();
    }
    Coordinator_->Start();

    AuthenticationManager_->Start();

    MemoryUsageTracker_->Start();

    RpcServer_->Start();
}

const IInvokerPtr& TBootstrap::GetControlInvoker() const
{
    return Control_->GetInvoker();
}

const TProxyBootstrapConfigPtr& TBootstrap::GetConfig() const
{
    return Config_;
}

TProxyDynamicConfigPtr TBootstrap::GetDynamicConfig() const
{
    return DynamicConfig_.Acquire();
}

const NApi::IClientPtr& TBootstrap::GetRootClient() const
{
    return RootClient_;
}

const NApi::NNative::IConnectionPtr& TBootstrap::GetNativeConnection() const
{
    return Connection_;
}

const NDriver::IDriverPtr& TBootstrap::GetDriverV3() const
{
    return DriverV3_;
}

const NDriver::IDriverPtr& TBootstrap::GetDriverV4() const
{
    return DriverV4_;
}

const TCoordinatorPtr& TBootstrap::GetCoordinator() const
{
    return Coordinator_;
}

const IAccessCheckerPtr& TBootstrap::GetAccessChecker() const
{
    return AccessChecker_;
}

const TCompositeHttpAuthenticatorPtr& TBootstrap::GetHttpAuthenticator() const
{
    return HttpAuthenticator_;
}

const IAuthenticationManagerPtr& TBootstrap::GetAuthenticationManager() const
{
    return AuthenticationManager_;
}

const IDynamicConfigManagerPtr& TBootstrap::GetDynamicConfigManager() const
{
    return DynamicConfigManager_;
}

const IPollerPtr& TBootstrap::GetPoller() const
{
    return Poller_;
}

const INodeMemoryTrackerPtr& TBootstrap::GetMemoryUsageTracker() const
{
    return MemoryUsageTracker_;
}

const TApiPtr& TBootstrap::GetApi() const
{
    return Api_;
}

IHttpHandlerPtr TBootstrap::AllowCors(IHttpHandlerPtr nextHandler) const
{
    //  Это удобно, но стоит CPU и памяти на каждый вызов.
    return New<TCallbackHandler>(BIND_NO_PROPAGATE([config = Config_, nextHandler] (
        const IRequestPtr& req,
        const IResponseWriterPtr& rsp)
    {
        if (MaybeHandleCors(req, rsp, config->Api->Cors)) {
            return;
        }

        nextHandler->HandleRequest(req, rsp);
    }));
}

void TBootstrap::RegisterRoutes(const NHttp::IServerPtr& server)
{
    server->AddHandler("/auth/whoami", AllowCors(HttpAuthenticator_));
    server->AddHandler("/api/", AllowCors(Api_));
    server->AddHandler("/hosts/", AllowCors(HostsHandler_));
    server->AddHandler("/cluster_connection/", AllowCors(ClusterConnectionHandler_));
    server->AddHandler("/ping/", AllowCors(PingHandler_));
    if (CypressCookieLoginHandler_) {
        server->AddHandler("/login/", CypressCookieLoginHandler_);
    }

    server->AddHandler("/internal/discover_versions/v2", AllowCors(DiscoverVersionsHandler_));

    SolomonProxy_->Register("/solomon_proxy", server);

    server->AddHandler("/version", AllowCors(MakeStrong(this)));
    server->AddHandler("/service", AllowCors(MakeStrong(this)));

    // ClickHouse.
    server->AddHandler("/query", AllowCors(ClickHouseHandler_));
    server->AddHandler("/chyt", AllowCors(ClickHouseHandler_));

    if (!Config_->UIRedirectUrl.empty()) {
        server->AddHandler("/", New<TCallbackHandler>(BIND([config = Config_] (
            const IRequestPtr& req,
            const IResponseWriterPtr& rsp)
        {
            if (req->GetUrl().Path == "/auth" || req->GetUrl().Path == "/auth/") {
                rsp->SetStatus(EStatusCode::SeeOther);
                rsp->GetHeaders()->Add("Location", "https://oauth.yt.yandex.net");
            } else if (req->GetUrl().Path == "/" || req->GetUrl().Path == "/ui") {
                rsp->SetStatus(EStatusCode::SeeOther);
                rsp->GetHeaders()->Add("Location", config->UIRedirectUrl + "?" + req->GetUrl().RawQuery);
            } else {
                rsp->SetStatus(EStatusCode::NotFound);
            }

            WaitFor(rsp->Close())
                .ThrowOnError();
        })));
    }
}

////////////////////////////////////////////////////////////////////////////////

TBootstrapPtr CreateHttpProxyBootstrap(
    TProxyBootstrapConfigPtr config,
    INodePtr configNode,
    IServiceLocatorPtr serviceLocator)
{
    return New<TBootstrap>(
        std::move(config),
        std::move(configNode),
        std::move(serviceLocator));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
