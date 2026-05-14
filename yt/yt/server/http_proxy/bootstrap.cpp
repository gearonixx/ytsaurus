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

// @gearonixx
// Старт прокси разделён на две фазы:
//   1) DoInitialize — СОЗДАТЬ все объекты (соединения, серверы, хендлеры, координатор).
//      Объекты сконструированы, но ещё ничего не делают: серверы не слушают порт,
//      синхронизаторы не крутятся.
//   2) DoStart — ВКЛЮЧИТЬ их (Start() у каждого подкомпонента). Только после этого
//      прокси реально начинает принимать запросы.
// Зачем разделять: если что-то упадёт на фазе init, мы упадём с понятной ошибкой
// до того, как порт начал принимать клиентов.
void TBootstrap::DoRun()
{
    DoInitialize();
    DoStart();
}

// @gearonixx
// Большая фаза «собрать все компоненты процесса». Идёт по слоям сверху вниз:
// сначала monitoring + orchid, потом memory tracker (учёт памяти), потом
// Connection к мастерам кластера, потом координатор/dynamic-config/RPC,
// потом аутентификация, драйверы, и в самом конце — HTTP/HTTPS-серверы и Api.
// Тут ничего не запускается — только конструируется и связывается.
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

    // @gearonixx
    // Опции соединения с кластером (Connection — наш клиент к мастер-серверам).
    // RetryRequestQueueSizeLimitExceeded — если мастер ответил «очередь запросов
    // переполнена», ретраить ли. CreateQueueConsumerRegistrationManager — включить
    // подсистему регистрации consumer'ов на YT-очередях (нужно, чтобы прокси умела
    // обрабатывать команды над очередями).
    NNative::TConnectionOptions connectionOptions;
    connectionOptions.RetryRequestQueueSizeLimitExceeded = Config_->RetryRequestQueueSizeLimitExceeded;
    connectionOptions.CreateQueueConsumerRegistrationManager = true;

    // Учётная система памяти процесса — отслеживает, сколько байт сейчас аллоцировано и под какую категорию
    MemoryUsageTracker_ = CreateNodeMemoryTracker(
        Config_->MemoryLimits->Total.value_or(std::numeric_limits<i64>::max()),
        New<TNodeMemoryTrackerConfig>(),
        /*limits*/ {},
        Logger(),
        HttpProxyProfiler().WithPrefix("/memory_usage"),
        GetControlInvoker());

    Connection_ = CreateConnection(
        //  ClusterConnection — конфиг нативного подключения к кластеру YT.
        Config_->ClusterConnection,
        connectionOptions,
        /*clusterDirectoryOverride*/ {},
        MemoryUsageTracker_);

    // @gearonixx
    // Часть конфига кластерного соединения (адреса мастеров, тайм-ауты)
    // тоже умеет обновляться динамически — эта функция подписывает Connection
    // на эти изменения, чтобы не перезапускать прокси при правке таких настроек.
    SetupClusterConnectionDynamicConfigUpdate(
        Connection_,
        Config_->ClusterConnectionDynamicConfigPolicy,
        ConfigNode_->AsMap()->GetChildOrThrow("cluster_connection"),
        Logger());

    // @gearonixx
    // Запускаем фоновые синхронизаторы каталогов кластера. «Синхронизатор» здесь —
    // это компонент, который раз в N секунд ходит на мастер и обновляет у себя
    // локальную копию какого-то каталога:
    //   ClusterDirectory  — какие ВНЕШНИЕ кластеры доступны (нужно для multiproxy).
    //   NodeDirectory     — кэш всех нод кластера (адреса для чтения чанков).
    //   QueueConsumerRegistration — регистрации потребителей YT-очередей.
    //   MasterCellDirectory — карта мастер-ячеек (мастер часто шардирован).
    // Без них клиент знал бы только адреса из конфига и не реагировал на изменения.
    Connection_->GetClusterDirectorySynchronizer()->Start();
    Connection_->GetNodeDirectorySynchronizer()->Start();
    Connection_->GetQueueConsumerRegistrationManagerOrThrow()->StartSync();
    Connection_->GetMasterCellDirectorySynchronizer()->Start();
    SetupClients();

    // @gearonixx
    // Coordinator — компонент прокси, отвечающий за её регистрацию в кластере.
    // Конкретно: он создаёт/обновляет ноду самой прокси в Cypress (//sys/proxies/...),
    // публикует туда свою роль (data/control/...), отслеживает здоровье соседних
    // прокси для балансировки. То есть «кто эта прокси и кто живые соседи».
    Coordinator_ = New<TCoordinator>(Config_, this);

    // @gearonixx
    // Маленький хелпер: проставляет роль прокси («proxy_role=control/data/...»)
    // как глобальный тег на ВСЕ метрики Solomon. Solomon — система метрик
    // Яндекса; «динамический тег» — лейбл, прибиваемый ко всем метрикам процесса
    // на лету. Дальше в Графане можно фильтровать «покажи только control-прокси».
    //
    // Применяем тег сразу с текущей ролью, и подписываемся на её смену
    // (роль может меняться: координатор переключает прокси между control и data).
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

    // @gearonixx
    // DynamicConfigManager — компонент, который читает динамическую часть
    // конфига из Cypress (ноду в //sys/proxies/.../@dynamic_config) и оповещает
    // подписчиков, когда админ её обновил. Сам TBootstrap подписывается на
    // изменения, чтобы пробросить их во все компоненты — см. OnDynamicConfigChanged.
    // MakeWeak(this) — слабая ссылка, чтобы менеджер не держал bootstrap живым.
    DynamicConfigManager_ = CreateDynamicConfigManager(this);
    DynamicConfigManager_->SubscribeBeforeConfigChanged(BIND_NO_PROPAGATE(&TBootstrap::OnDynamicConfigChanged, MakeWeak(this)));

    // @gearonixx
    // Опционально публикуем под Orchid три «окна» в живой процесс, чтобы
    // через /orchid с ноутбука можно было посмотреть, что сейчас загружено:
    //   /config                  — статический конфиг прокси (как при старте);
    //   /dynamic_config_manager  — текущее состояние менеджера динамики;
    //   /cluster_connection      — состояние подключения к мастерам.
    // По дефолту это выключено (ExposeConfigInOrchid=false), потому что в конфиге
    // могут лежать чувствительные вещи (токены, секреты).
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
    // @gearonixx
    // HotswapManager — следит за «горячей заменой» дисков на железе (диск
    // вынули/вставили). На прокси чаще всего не подключён, поэтому проверяем
    // через FindService (вернёт nullptr, если сервис не зарегистрирован).
    // Если есть — пробрасываем его состояние в Orchid.
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
        // @gearonixx
        // >> whyyy single handler?
        // Причина: CHYT — это ClickHouse-over-YT, и его HTTP-протокол это протокол ClickHouse. Клиенты ClickHouse шлют запросы на разные пути
        // (/, /?query=..., /ping, etc), и весь этот протокол целиком обрабатывает ClickHouseHandler сам —
        // внутри он смотрит на путь и query string и решает что делать.
        // Прокси не должна вмешиваться и роутить отдельные пути в свои хендлеры — иначе сломает совместимость с CH-клиентами.
        ChytApiHttpServer_->AddHandler("/", AllowCors(ClickHouseHandler_));
    }

    if (Config_->ChytHttpsServer) {
        Config_->ChytHttpsServer->ServerName = "ChytHttpsApi";
        ChytApiHttpsServer_ = NHttps::CreateServer(Config_->ChytHttpsServer, Poller_, Acceptor_, GetControlInvoker());
        ChytApiHttpsServer_->AddHandler("/", AllowCors(ClickHouseHandler_));
    }

    // ??
    // Виртуальный узел — это узел Orchid-дерева, содержимое которого вычисляется на лету в момент запроса, а не хранится статически.
    //  "в дереве orchidRoot по пути /http_proxy положи virtualNode".
    SetNodeByYPath(
        orchidRoot,
        "/http_proxy",
        CreateVirtualNode(Api_->CreateOrchidService()));

    // @gearonixx
    // Heap profiler — инструмент, который отвечает на вопрос "кто именно сейчас держит память в куче". Он сэмплирует ал
    // Профайлер — инструмент, который собирает данные о работе программы для анализа узких мест
    HttpProxyHeapUsageProfiler_ = New<TProxyHeapUsageProfiler>(
        GetControlInvoker(),
        Config_->HeapProfiler);
}

// @gearonixx
//  принадлежит ли переданный сетевой адрес одному из CHYT-серверов прокси (HTTP или HTTPS-варианту).
//  Используется чтобы по адресу входящего соединения понять "это запрос к CHYT-эндпоинту?"
bool TBootstrap::IsChytApiServerAddress(const NNet::TNetworkAddress& address) const
{
    // парсится как:
    // ChytApiHttpServer_ && (address == ChytApiHttpServer_->GetAddress())
    return (ChytApiHttpServer_ && address == ChytApiHttpServer_->GetAddress())
        || (ChytApiHttpsServer_ && address == ChytApiHttpsServer_->GetAddress());
}


// @gearonixx
// "Bootstrap" в инженерном смысле — "стартовая последовательность, которая поднимает систему с нуля до рабочего состояния"
//

// @gearonixx
// создает рутового YT-клиента — клиента, который ходит в кластер от имени системного пользователя root (с максимальными правами)
void TBootstrap::SetupClients()
{
    auto options = NApi::TClientOptions::FromUser(NSecurityClient::RootUserName);
    RootClient_ = Connection_->CreateClient(options);

    NLogging::GetDynamicTableLogWriterFactory()->SetClient(RootClient_);
}

// @gearonixx
// Перевыставляет лимиты памяти на лету. Memory tracker работает через «бюджеты»:
// есть общий лимит на процесс и подкатегории (например, HeavyRequest — большие
// запросы типа write_table). Конфиг задаёт не абсолютные числа, а доли (ratio),
// которые умножаются на общий memoryLimit. Пример: ratio=0.3 при limit=100 ГБ →
// 30 ГБ под HeavyRequest. Это позволяет менять лимиты «пропорционально» через
// один общий ползунок.
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

// @gearonixx
// Колбэк, который дёргается при смене динамического конфига кластера.
// «Динамический конфиг» в YT — это часть настроек, которую можно менять на лету,
// не перезапуская процесс: админ обновляет ноду в Cypress, DynamicConfigManager
// замечает это и зовёт всех подписчиков с новым значением.
//
// Здесь мы пробрасываем новые значения во все компоненты, которые умеют
// перенастраиваться: общий менеджер синглтонов YT, memory tracker, BusServer,
// trace sampler (сэмплинг для распределённого трейсинга), signature components,
// синхронизатор каталога мастер-ячеек.
//
// DynamicConfig_.Store(newConfig) — атомарная подмена текущего хранимого конфига
// (TAtomicIntrusivePtr делает это безопасно для читателей в других потоках).
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


// @gearonixx

// Переписать на userver значит сломать совместимость со всем остальным кодом YT (мастер, ноды, скедулер)
// и потерять оптимизации, которые делались под их сценарии нагрузки.

void TBootstrap::HandleRequest(
    const NHttp::IRequestPtr& req,
    const NHttp::IResponseWriterPtr& rsp)
{
    rsp->SetStatus(EStatusCode::OK);

    // service отдаёт JSON со временем старта и версией, всё остальное (включая /version) — просто строку с версией.
    if (req->GetUrl().Path == "/service") {
        // reply json is a helper in the http module
        ReplyJson(rsp, [&] (NYson::IYsonConsumer* json) {
            // oh ok
            BuildYsonFluently(json)
                .BeginMap()
                    .Item("start_time").Value(StartTime_)
                    .Item("version").Value(GetVersion())
                .EndMap();
        });
    } else {
        // @gearonixx
        // >> TSharedRef это аналог std::shared_ptr?
        // Не аналог, а построен поверх него. std::shared_ptr — это shared-ownership на один объект.
        // TSharedRef — это shared-ownership на диапазон байт: пара (указатель + размер) плюс shared_ptr-подобный холдер, который держит буфер живым.
        // write body returns the TfFuture<void> and accepts the TSharedRef small body
        WaitFor(rsp->WriteBody(TSharedRef::FromString(GetVersion())))
            .ThrowOnError();
    }
}

// @gearonixx
// Публичная точка запуска прокси (зовётся из main). Возвращает TFuture<void> —
// «обещание, которое завершится, когда прокси поднимется» (или с ошибкой).
//
// Разбор паттерна:
//   BIND(&DoRun, MakeStrong(this)) — оборачивает метод DoRun в TCallback с захватом
//      сильной ссылки на this (чтобы объект не умер, пока работа не закончилась).
//   .AsyncVia(GetControlInvoker()) — говорит «выполни этот колбэк в Control-потоке»
//      (control invoker — это та однопоточная очередь TActionQueue("Control"),
//      где у нас идут все управляющие операции).
//   .Run() — отправляет колбэк в очередь и сразу возвращает фьючер.
// То есть main вызывает Run(), сразу получает фьючер и может его дождаться,
// а реальный DoRun уже исполняется на отдельном потоке.
TFuture<void> TBootstrap::Run()
{
    return BIND(&TBootstrap::DoRun, MakeStrong(this))
        .AsyncVia(GetControlInvoker())
        .Run();
}

// @gearonixx
// Вторая фаза запуска: всё уже сконструировано в DoInitialize, теперь дёргаем
// Start() у каждого живого компонента в правильном порядке. После этого:
// серверы начинают слушать порт и принимать соединения, синхронизаторы
// крутятся в фоне, координатор регистрируется в Cypress как живая прокси.
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

// @gearonixx
// the constant getters

// компонент, который проверяет, имеет ли пользователь право ходить через эту конкретную прокси.
// Это отдельная проверка от обычных ACL на объекты Cypress: даже если у юзера есть права на чтение таблицы, прокси может его не пустить.
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

// we stopped here
// Да, ровно middleware. Б
//
// @gearonixx
// CORS (Cross-Origin Resource Sharing) — браузерный механизм безопасности:
// если страница на foo.com хочет дёрнуть API на bar.com, браузер сначала
// шлёт preflight-запрос (OPTIONS) и проверяет в ответных заголовках, разрешает ли
// сервер такой кросс-доменный вызов. Без правильных CORS-заголовков браузер
// просто заблокирует ответ.
//
// AllowCors — middleware-обёртка вокруг любого хендлера. Возвращает новый хендлер,
// который сначала пробует обработать CORS (MaybeHandleCors сам вернёт ответ для
// preflight'а или подмешает заголовки), а если запрос не CORS — пробрасывает
// дальше в nextHandler. Используется в RegisterRoutes, чтобы обернуть
// все API-эндпоинты разом.
IHttpHandlerPtr TBootstrap::AllowCors(IHttpHandlerPtr nextHandler) const
{
    //  Это удобно, но стоит CPU и памяти на каждый вызов.
    // TCallbackHandler ждёт TCallback<void(IRequestPtr, IResponseWriterPtr)> — тип YT-коллбека. Голую лямбду туда не передашь, нужен BIND или BIND_NO_PROPAGATE, чтобы получить TCallback.
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

// @gearonixx
// Навешивает на HTTP-сервер карту «путь → хендлер». Вызывается отдельно для
// каждого варианта сервера (HTTP, HTTPS, TVM-only), поэтому одна функция —
// одна точка правды по роутам.
//
// Что куда:
//   /auth/whoami         — узнать, кем меня видит прокси (отдаёт HttpAuthenticator).
//   /api/                — основной YT API (этот префикс ловит весь /api/v3/...,
//                          /api/v4/...; обрабатывает TApi).
//   /hosts/              — список живых прокси (для клиентского балансировщика).
//   /cluster_connection/ — раздаёт конфиг подключения к кластеру.
//   /ping/               — health check.
//   /login/              — логин через Cypress-cookie (если включён).
//   /internal/discover_versions/v2 — версии всех компонентов кластера.
//   /solomon_proxy       — проксирование метрик в Solomon.
//   /version, /service   — отдаёт сам TBootstrap (см. HandleRequest).
//   /query, /chyt        — ClickHouse over YT.
//   /, /ui, /auth        — редиректы в UI, если задан UIRedirectUrl.
//
// Все API-роуты обёрнуты в AllowCors, чтобы работали из браузера.
void TBootstrap::RegisterRoutes(const NHttp::IServerPtr& server)
{
    server->AddHandler("/auth/whoami", AllowCors(HttpAuthenticator_));
    // @gearonixx @AI_GENERATED@
    // AddHandler внутри использует TRequestPathMatcher (core/http/server.cpp).
    // Паттерны с / на конце пишутся в Subtrees_ (две версии — "/api/" и "/api").
    // Match делает longest-prefix по сегментам (находит самый длинный из
    // зарегистрированных префиксов, покрывающий путь): для /api/v3/read_table
    // сначала смотрит Exact_, потом режет path с конца по слэшам, пока не
    // найдёт совпадение в Subtrees_, и вернёт сюда AllowCors(Api_).
    // AllowCors — обёртка: на OPTIONS сама отдаёт CORS-заголовки, иначе зовёт
    // Api_->HandleRequest, откуда уходит в TContext::TryPrepare/Run/Finalize.
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

    // "Если в конфиге задан UIRedirectUrl" (непустая строка). Только тогда регистрируется хендлер с редиректами.
    if (!Config_->UIRedirectUrl.empty()) {
        server->AddHandler("/", New<TCallbackHandler>(BIND([config = Config_] (
            const IRequestPtr& req,
            const IResponseWriterPtr& rsp)
        {
            // Это отражает разницу в их роли. Запрос — уже готовая штука: ядро/HTTP-парсер прочитал заголовки, тело, оформил всё в IRequest,
            // ты только читаешь его поля. Поэтому имя простое — IRequest.
            if (req->GetUrl().Path == "/auth" || req->GetUrl().Path == "/auth/") {
                rsp->SetStatus(EStatusCode::SeeOther);
                rsp->GetHeaders()->Add("Location", "https://oauth.yt.yandex.net");
            } else if (req->GetUrl().Path == "/" || req->GetUrl().Path == "/ui") {
                rsp->SetStatus(EStatusCode::SeeOther);
                // UIRedirectUrl — строка из конфига прокси, URL фронтенда YT-кластера, куда редиректить пользователей,
                // которые случайно открыли API-прокси в браузере. Например, на проде это что-то типа https://yt.yandex-team.ru/hahn.
                rsp->GetHeaders()->Add("Location", config->UIRedirectUrl + "?" + req->GetUrl().RawQuery);
            } else {
                rsp->SetStatus(EStatusCode::NotFound);
            }

            WaitFor(rsp-> Close())
                .ThrowOnError();
        })));
    }
}

////////////////////////////////////////////////////////////////////////////////
/// @gearonixx
// Зачем так: спрятать детали реализации. Кому-то снаружи не нужно знать про все 30 полей TBootstrap, инклюдить тяжёлые хедеры и зависеть от них
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
