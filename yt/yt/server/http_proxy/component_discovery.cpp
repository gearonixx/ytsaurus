#include "component_discovery.h"
#include "coordinator.h"

#include <yt/yt/client/api/client.h>

#include <util/string/split.h>

#include <library/cpp/iterator/zip.h>

namespace NYT::NHttpProxy {

using namespace NApi;
using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

namespace {

////////////////////////////////////////////////////////////////////////////////

void FillMasterReadOptions(TMasterReadOptions& options, const TMasterReadOptions& value)
{
    options = value;
}

// @gearonixx @AI_GENERATED@
// «Опциональные» компоненты — те, чьё отсутствие в кластере не ошибка
// (старые/новые установки, где этих сервисов просто нет). Если ListNode
// по их пути упал — не падаем, возвращаем пустой список вместо ошибки.
bool IsComponentOptional(EClusterComponentType component)
{
    switch (component) {
        case EClusterComponentType::MasterCache:
        case EClusterComponentType::Discovery:
        case EClusterComponentType::TabletBalancer:
        case EClusterComponentType::ReplicatedTableTracker:
        case EClusterComponentType::QueueAgent:
        case EClusterComponentType::QueryTracker:
        case EClusterComponentType::CypressProxy:
            return true;
        default:
            return false;
    }
}

// @gearonixx @AI_GENERATED@
// COMPAT(koloshmet) — пометка YT-стиля «костыль на время миграции
// Эти компоненты в старых сборках кластера не публиковали версию через orchid
// напрямую, поэтому если запрос orchid'а упал — пробуем достать версию через
// fallback-путь /orchid/build_info/binary_version (см. GetCompatBinaryVersion).
// COMPAT(koloshmet)
bool IsComponentCompat(EClusterComponentType component)
{
    switch (component) {
        case EClusterComponentType::TabletBalancer:
        case EClusterComponentType::ReplicatedTableTracker:
            return true;
        default:
            return false;
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

// @gearonixx @AI_GENERATED@
// Сериализатор одного TClusterComponentInstance в YSON. YT использует ADL-функцию
// Serialize(value, consumer) для всех типов, которые могут уехать в YSON-вывод —
// тут она вызовется когда хендлер /internal/discover_versions будет рендерить
// ответ клиенту. BuildYsonFluently — fluent-API: пишет map с полями address/type,
// дальше ветка через .DoIf — если ошибка пустая, кладём version/start_time/banned
// (+ state если он есть), иначе кладём только error. То есть instance публикуется
// либо «здоровым», либо «битым», без mix-а.
void Serialize(const TClusterComponentInstance& instance, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("address").Value(instance.Address)
            .Item("type").Value(instance.Type)
            .DoIf(instance.Error.IsOK(), [&] (auto fluent) {
                fluent
                    .Item("version").Value(instance.Version)
                    .Item("start_time").Value(instance.StartTime)
                    .Item("banned").Value(instance.Banned);

                if (!instance.State.empty()) {
                    fluent.Item("state").Value(instance.State);
                }
            })
            .DoIf(!instance.Error.IsOK(), [&] (auto fluent) {
                fluent.Item("error").Value(instance.Error);
            })
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

// @gearonixx @AI_GENERATED@
// TComponentDiscoverer — собирает срез по живым компонентам кластера (мастеры,
// ноды, прокси, шедулеры, query tracker и т.д.) с их версиями/состоянием.
// Используется хендлером /internal/discover_versions: наружу отдаётся «вот что
// у нас крутится и каких версий» — для мониторинга, апгрейдов, проверки что во
// всём кластере одна и та же версия.
//
// Конструктор просто запоминает зависимости:
//   Client_                    — нативный YT-клиент, через который ходим в Cypress
//                                и в orchid компонентов.
//   MasterReadOptions_         — настройки чтения мастера (из какого реплики
//                                читать, таймауты), навешиваются на каждый запрос.
//   ComponentDiscoveryOptions_ — настройки самого discoverer'а; сейчас там
//                                один колбэк ProxyDeathAgeCallback, возвращающий
//                                «сколько секунд без liveness-апдейта считаем
//                                прокси мёртвой» — приходит из координатора,
//                                поэтому колбэк а не значение (оно может меняться).
// YT_VERIFY — макрос-assert, падает в любой сборке (release тоже), если колбэк
// не задан; здесь это контракт «без него нельзя».
TComponentDiscoverer::TComponentDiscoverer(
    IClientPtr client,
    TMasterReadOptions masterReadOptions,
    TComponentDiscoveryOptions componentDiscoveryOptions)
    : Client_(std::move(client))
    , MasterReadOptions_(std::move(masterReadOptions))
    , ComponentDiscoveryOptions_(std::move(componentDiscoveryOptions))
{
    YT_VERIFY(ComponentDiscoveryOptions_.ProxyDeathAgeCallback);
}

// @gearonixx @AI_GENERATED@
// Тянет список «нод хранения» (cluster/data/tablet/exec) — это машины-исполнители
// данных и джоб, не путать с «нодой Cypress» (узел дерева). Их атрибуты лежат
// прямо в Cypress на самих узлах: ListNode по //sys/<тип>s даёт сразу список
// плюс все интересующие атрибуты в одном запросе (options.Attributes).
//
// WaitFor — YT-шный sync-wait: запрос асинхронный (TFuture), но мы блокируемся
// на текущем файбере до ответа. ConvertToNode превращает YSON в дерево IListNode/
// IMapNode для удобного обхода. Атрибуты читаются как Find<T> (опциональный)
// или Get<T>(default). Для exec-нод также вытаскиваем job_proxy_build_version —
// он понадобится в ListJobProxies, чтобы отдельной типизированной строкой
// показать «джоб-прокси, едущие на этих нодах».
//
// Если нода online но потеряла version/start_time — вместо успеха пишем Error;
// сериализатор тогда отдаст её как «битую» (см. Serialize выше).
std::vector<TClusterComponentInstance> TComponentDiscoverer::ListClusterNodes(EClusterComponentType component) const
{
    switch (component) {
        case EClusterComponentType::ClusterNode:
        case EClusterComponentType::DataNode:
        case EClusterComponentType::TabletNode:
        case EClusterComponentType::ExecNode:
            break;
        default:
            YT_ABORT();
    }

    TListNodeOptions options;
    FillMasterReadOptions(options, MasterReadOptions_);
    options.Attributes = {
        "register_time",
        "version",
        "banned",
        "state",
        "job_proxy_build_version",
    };

    auto rsp = WaitFor(Client_->ListNode(GetCypressDirectory(component), options))
        .ValueOrThrow();
    auto rspList = ConvertToNode(rsp)->AsList();

    std::vector<TClusterComponentInstance> instances;
    instances.reserve(rspList->GetChildren().size());

    for (const auto& node : rspList->GetChildren()) {
        auto version = node->Attributes().Find<std::string>("version");
        auto nodeState = node->Attributes().Get<std::string>("state", /*defaultValue*/ "");
        auto startTime = node->Attributes().Find<std::string>("register_time");

        TClusterComponentInstance instance{
            .Type = component,
            .Address = node->GetValue<std::string>(),
            .Version = version.value_or(""),
            .StartTime = startTime.value_or(""),
            .Banned = node->Attributes().Get<bool>("banned", /*defaultValue*/ false),
            .Online = nodeState == "online",
            .State = nodeState,
            .Error = TError(),
            .JobProxyVersion = node->Attributes().Find<std::string>("job_proxy_build_version"),
        };

        if (instance.Online && (!version || !startTime)) {
            instance.Error = TError("Component is missing some of the required attributes in response")
                << TErrorAttribute("version", version)
                << TErrorAttribute("start_time", startTime);
        }

        instances.push_back(instance);
    }

    return instances;
}

// @gearonixx @AI_GENERATED@
// Аналог ListClusterNodes, но для HTTP/RPC-прокси. Принципиальная разница —
// определение «жива ли»:
//   RpcProxy  — у каждой прокси под её Cypress-узлом висит дочерний узел "alive"
//               (lock-нода, исчезающая при потере сессии с мастером). Просто
//               проверяем «есть alive — жив».
//   HttpProxy — пишет в свой атрибут "liveness" структуру TLiveness с UpdatedAt
//               (см. coordinator.h, тот самый Liveness что отдаёт TCoordinator
//               в свою запись). Считаем живой, если последнее обновление было
//               не позже sчем ProxyDeathAgeCallback() назад.
//
// Тут используется GetNode (а не ListNode), потому что нам нужен не плоский
// список имён, а map «адрес → узел с атрибутами и потомками» — это позволяет
// для RPC заглянуть в дочерний "alive" и одновременно прочитать атрибуты.
std::vector<TClusterComponentInstance> TComponentDiscoverer::ListProxies(EClusterComponentType component) const
{
    TGetNodeOptions options;
    FillMasterReadOptions(options, MasterReadOptions_);

    switch (component) {
        case EClusterComponentType::RpcProxy:
            options.Attributes = {
                "start_time",
                "version",
                "banned",
            };
            break;
        case EClusterComponentType::HttpProxy:
            options.Attributes = {
                "liveness",
                "start_time",
                "version",
                "banned",
            };
            break;
        default:
            YT_ABORT();
    }

    auto nodeYson = WaitFor(Client_->GetNode(GetCypressDirectory(component), options))
        .ValueOrThrow();
    auto addressToNode = ConvertTo<THashMap<std::string, IMapNodePtr>>(nodeYson);

    std::vector<TClusterComponentInstance> instances;
    instances.reserve(addressToNode.size());

    auto timeNow = TInstant::Now();
    for (const auto& [address, node] : addressToNode) {
        auto version = node->Attributes().Find<std::string>("version");
        auto banned = node->Attributes().Find<bool>("banned");
        auto startTime = node->Attributes().Find<std::string>("start_time");

        TClusterComponentInstance instance{
            .Type = component,
            .Address = address,
        };

        if (version && startTime) {
            instance.Version = *version;
            instance.StartTime = *startTime;
            instance.Banned = banned.value_or(false);
        } else {
            instance.Error = TError("Cannot find required attributes in response")
                << TErrorAttribute("version", version)
                << TErrorAttribute("start_time", startTime);
        }

        if (component == EClusterComponentType::RpcProxy) {
            auto alive = node->AsMap()->FindChild("alive");
            instance.Online = static_cast<bool>(alive);
        } else if (auto livenessPtr = node->Attributes().Find<TLivenessPtr>("liveness")) {
            instance.Online = (livenessPtr->UpdatedAt + ComponentDiscoveryOptions_.ProxyDeathAgeCallback() >= timeNow);
        } else {
            instance.Error = TError("Liveness attribute is missing");
        }

        if (instance.Online) {
            instance.State = "online";
        } else {
            instance.State = "offline";
        }
        instances.push_back(instance);
    }

    return instances;
}

// @gearonixx @AI_GENERATED@
// Возвращает относительные пути инстансов от GetCypressDirectory(component) —
// то есть для PrimaryMaster это будут "/<address1>", "/<address2>" и т.д.
// (полный путь склеит GetCypressPaths ниже).
//
// Спецкейс — SecondaryMaster: вторичных мастер-серверов в YT несколько *ячеек*
// (cells), каждая со своими репликами, поэтому //sys/secondary_masters
// двухуровневая: //sys/secondary_masters/<cell_tag>/<address>. Здесь
// делаем GetNode (не Listдвa уровня сразу) и в двух вложенных циклах собираем
// все пути вида "/<cell_tag>/<address>".
//
// Для остальных — обычный ListNode. Если ListNode фейлится и компонент опционален,
// возвращаем пустой список вместо проброса ошибки (старые/новые кластера, где
// этого сервиса просто нет).
std::vector<TYPath> TComponentDiscoverer::GetCypressSubpaths(
    const NApi::IClientPtr& client,
    const NApi::TMasterReadOptions& masterReadOptions,
    EClusterComponentType component)
{
    std::vector<TYPath> paths;
    if (component == EClusterComponentType::SecondaryMaster) {
        TGetNodeOptions options;
        FillMasterReadOptions(options, masterReadOptions);
        auto directory = WaitFor(client->GetNode(GetCypressDirectory(component), options))
            .ValueOrThrow();
        for (const auto& [subdirectory, instances] : ConvertToNode(directory)->AsMap()->GetChildren()) {
            for (const auto& [instance, _] : instances->AsMap()->GetChildren()) {
                paths.push_back(Format("/%v/%v", subdirectory, instance));
            }
        }
    } else {
        TListNodeOptions options;
        FillMasterReadOptions(options, masterReadOptions);
        auto rspOrError = WaitFor(client->ListNode(GetCypressDirectory(component)));
        if (!rspOrError.IsOK() && IsComponentOptional(component)) {
            return paths;
        }
        auto rsp = std::move(rspOrError).ValueOrThrow();
        auto rspList = ConvertToNode(rsp)->AsList();
        for (const auto& node : rspList->GetChildren()) {
            paths.push_back(Format("/%v", node->GetValue<std::string>()));
        }
    }
    return paths;
}

std::vector<TYPath> TComponentDiscoverer::GetCypressPaths(
    const NApi::IClientPtr& client,
    const NApi::TMasterReadOptions& masterReadOptions,
    EClusterComponentType component)
{
    auto paths = GetCypressSubpaths(client, masterReadOptions, component);
    auto cypressDirectory = GetCypressDirectory(component);

    for (auto& path : paths) {
        path = cypressDirectory + path;
    }

    return paths;
}

// @gearonixx @AI_GENERATED@
// Fallback-путь для COMPAT-компонентов: если стандартный orchid-запрос упал,
// пробуем достать только версию из <path>/orchid/build_info/binary_version
// (orchid — это виртуальное YSON-дерево, которое компонент сам выставляет;
// build_info там более стабильный, чем кастомные поля). Возвращает TErrorOr —
// либо строку с версией, либо TError. static_cast<TError&> — приводит TErrorOr
// к его базовой части TError, чтобы вернуть только ошибку без оставшегося value.
TErrorOr<std::string> TComponentDiscoverer::GetCompatBinaryVersion(const TYPath& path) const
{
    auto rspOrError = WaitFor(Client_->GetNode(path + "/orchid/build_info/binary_version"));
    if (!rspOrError.IsOK()) {
        return std::move(static_cast<TError&>(rspOrError));
    }

    try {
        return ConvertTo<std::string>(rspOrError.Value());
    } catch (const std::exception& ex) {
        return ex;
    }
}

// @gearonixx @AI_GENERATED@
// Универсальный «параллельный обход orchid'ов»: для компонентов, у которых
// version/start_time лежат не атрибутами Cypress-узла, а внутри их собственного
// orchid-поддерева (мастеры, шедулер, query-tracker и т.д.).
//
// Идея: на каждый subpath стартуем GetNode (асинхронно, без WaitFor) — копим
// std::vector<TFuture<...>>, потом во втором цикле WaitFor'им каждый по очереди.
// Так все запросы летят одновременно, а ждём суммарно ~max времени, а не сумму.
// Таймаут 1 сек — чтобы один зависший компонент не заблокировал весь discover.
//
// suffix — позволяет добавить общий «хвост» к пути (например, "/orchid/service").
// instanceType — иногда отличается от component (например, для job-proxy
// сначала ходим как ExecNode, а в результате тип ставим JobProxy).
//
// Ответы парсим: если в orchid есть error — кладём её, иначе вытаскиваем
// version/start_time. Для COMPAT-компонентов при фейле основного запроса
// идём в fallback через GetCompatBinaryVersion.
std::vector<TClusterComponentInstance> TComponentDiscoverer::GetAttributes(
    EClusterComponentType component,
    const std::vector<TYPath>& subpaths,
    EClusterComponentType instanceType,
    const TYPath& suffix) const
{
    const auto OrchidTimeout = TDuration::Seconds(1);

    TGetNodeOptions options;
    FillMasterReadOptions(options, MasterReadOptions_);
    options.Timeout = OrchidTimeout;

    std::vector<TFuture<TYsonString>> responses;
    responses.reserve(subpaths.size());
    for (const auto& subpath : subpaths) {
        responses.push_back(Client_->GetNode(GetCypressDirectory(component) + subpath + suffix));
    }

    std::vector<TClusterComponentInstance> results;
    results.reserve(subpaths.size());
    for (size_t index = 0; index < subpaths.size(); ++index) {
        auto ysonOrError = WaitFor(responses[index]);

        auto& result = results.emplace_back();
        result.Type = instanceType;

        result.Address = StringSplitter(subpaths[index]).Split('/').ToList<std::string>().back();
        if (!ysonOrError.IsOK()) {
            if (IsComponentCompat(component)) {
                auto versionOrError = GetCompatBinaryVersion(GetCypressDirectory(component) + subpaths[index]);
                if (versionOrError.IsOK()) {
                    result.Version = std::move(versionOrError).Value();
                } else {
                    result.Error = std::move(versionOrError);
                }
            } else {
                result.Error = std::move(ysonOrError);
            }
            continue;
        }

        try {
            auto rspMap = ConvertToNode(ysonOrError.Value())->AsMap();

            if (auto errorNode = rspMap->FindChild("error")) {
                result.Error = ConvertTo<TError>(errorNode);
                continue;
            }

            auto version = ConvertTo<std::string>(rspMap->GetChildOrThrow("version"));
            auto startTime = ConvertTo<std::string>(rspMap->GetChildOrThrow("start_time"));

            result.Version = std::move(version);
            result.StartTime = std::move(startTime);
        } catch (const std::exception& ex) {
            result.Error = ex;
        }
    }
    return results;
}

// @gearonixx @AI_GENERATED@
// JobProxy — это отдельный процесс-обёртка, в котором запускается пользовательский
// код map/reduce-джобы. Они не регистрируются в Cypress сами; вместо этого
// каждый exec-нод в своём атрибуте "job_proxy_build_version" сообщает версию
// бинаря джоб-прокси, которого она будет запускать. Поэтому здесь мы берём
// список exec-нод (ListClusterNodes) и переписываем им Type=JobProxy + Version
// из job_proxy_build_version. Забаненные exec-ноды пропускаем — на них джобы
// не пойдут, так что их job-proxy-версия неинтересна.
std::vector<TClusterComponentInstance> TComponentDiscoverer::ListJobProxies() const
{
    auto execNodeInstances = ListClusterNodes(EClusterComponentType::ExecNode);

    std::vector<TClusterComponentInstance> instances;
    instances.reserve(execNodeInstances.size());

    for (auto& instance : execNodeInstances) {
        if (instance.Banned) {
            continue;
        }

        instance.Type = EClusterComponentType::JobProxy;

        if (instance.JobProxyVersion) {
            instance.Version = *instance.JobProxyVersion;
        } else {
            instance.Version = {};
            instance.Error = TError("Attribute \"job_proxy_build_version\" is missing");
        }

        instances.emplace_back(std::move(instance));
    }

    return instances;
}

// @gearonixx @AI_GENERATED@
// Таблица соответствий «тип компонента → его корневая директория в Cypress».
// Format("//sys/%lvs", component) — печать YT-енума его «lowercase»-именем плюс
// "s" в конце (PrimaryMaster → "primary_masters"); работает для случаев, где
// имя директории — просто множественное число от типа. Для остальных хардкод
// конкретного пути. RpcProxy/HttpProxy используют константы из заголовка —
// их пути исторически без префикса //sys/.
TYPath TComponentDiscoverer::GetCypressDirectory(EClusterComponentType component)
{
    switch (component) {
        case EClusterComponentType::PrimaryMaster:
        case EClusterComponentType::SecondaryMaster:
        case EClusterComponentType::ClusterNode:
        case EClusterComponentType::DataNode:
        case EClusterComponentType::TabletNode:
        case EClusterComponentType::ExecNode:
        case EClusterComponentType::TimestampProvider:
        case EClusterComponentType::MasterCache:
            return Format("//sys/%lvs", component);
        case EClusterComponentType::Scheduler:
            return "//sys/scheduler/instances";
        case EClusterComponentType::ControllerAgent:
            return "//sys/controller_agents/instances";
        case EClusterComponentType::Discovery:
            return "//sys/discovery_servers";
         case EClusterComponentType::TabletBalancer:
            return "//sys/tablet_balancer/instances";
        case EClusterComponentType::BundleController:
            return "//sys/cell_balancers/instances";
        case EClusterComponentType::ReplicatedTableTracker:
            return "//sys/replicated_table_tracker/instances";
        case EClusterComponentType::QueueAgent:
            return "//sys/queue_agents/instances";
        case EClusterComponentType::QueryTracker:
            return "//sys/query_tracker/instances";
        case EClusterComponentType::CypressProxy:
            return "//sys/cypress_proxies";
        case EClusterComponentType::RpcProxy:
            return RpcProxiesPath;
        case EClusterComponentType::HttpProxy:
            return HttpProxiesPath;
        default:
            YT_ABORT();
    }
}

// @gearonixx @AI_GENERATED@
// Диспатчер: по типу компонента выбирает нужный способ собрать инстансы.
// Три ветки:
//   1) «orchid-based»  — мастеры/шедулер/query-tracker и т.п.: список путей
//      берётся из Cypress (GetCypressSubpaths), детали — из orchid (GetAttributes).
//   2) «cluster nodes» — ноды хранения: всё нужное лежит атрибутами Cypress-узла,
//      ListClusterNodes одним запросом.
//   3) «proxies»       — RPC/HTTP-прокси: ListProxies со своей логикой live/dead.
//   + JobProxy строится производно от exec-нод, см. ListJobProxies.
std::vector<TClusterComponentInstance> TComponentDiscoverer::GetInstances(EClusterComponentType component) const
{
    switch (component) {
        case EClusterComponentType::PrimaryMaster:
        case EClusterComponentType::SecondaryMaster:
        case EClusterComponentType::Scheduler:
        case EClusterComponentType::ControllerAgent:
        case EClusterComponentType::TimestampProvider:
        case EClusterComponentType::Discovery:
        case EClusterComponentType::MasterCache:
        case EClusterComponentType::TabletBalancer:
        case EClusterComponentType::BundleController:
        case EClusterComponentType::ReplicatedTableTracker:
        case EClusterComponentType::QueueAgent:
        case EClusterComponentType::QueryTracker:
        case EClusterComponentType::CypressProxy:
            return GetAttributes(
                component,
                GetCypressSubpaths(Client_, MasterReadOptions_, component),
                /*instanceType*/ component);
        case EClusterComponentType::ClusterNode:
        case EClusterComponentType::DataNode:
        case EClusterComponentType::TabletNode:
        case EClusterComponentType::ExecNode:
            return ListClusterNodes(component);
        case EClusterComponentType::JobProxy:
            return ListJobProxies();
        case EClusterComponentType::HttpProxy:
        case EClusterComponentType::RpcProxy:
            return ListProxies(component);
        default:
            THROW_ERROR_EXCEPTION("Unknown component type %Qlv", component);
    }
}

// @gearonixx @AI_GENERATED@
// Точка входа для /internal/discover_versions. Параллельно запускает GetInstances
// для каждого значения енума и собирает результаты в один вектор.
//
// BIND(...).AsyncVia(invoker).Run() — YT-шный паттерн: BIND склеивает указатель
// на метод и аргументы в callback, AsyncVia говорит «выполни это на указанном
// invoker'е» (тут — текущий, то есть на каком-нибудь воркер-потоке), Run()
// триггерит и сразу возвращает TFuture<...>, не дожидаясь. Unretained(this) —
// «не держать сильную ссылку на меня в callback'е»; используется когда автор
// уверен что объект переживёт асинхронную операцию (тут переживёт, потому что
// сразу WaitFor'им AllSucceeded ниже, на том же стеке).
//
// AllSucceeded — комбинатор фьючей, ждёт пока все завершатся, и фейлит весь
// набор если хоть одна упала. Zip(domain_values, responses) — параллельно
// итерируется по двум диапазонам, чтобы для каждого компонента взять его
// результат; ranges::move — перемещает элементы (не копирует) в общий вектор.
std::vector<TClusterComponentInstance> TComponentDiscoverer::GetAllInstances() const
{
    std::vector<TFuture<std::vector<TClusterComponentInstance>>> asyncInstances;
    for (auto component : TEnumTraits<EClusterComponentType>::GetDomainValues()) {
        asyncInstances.push_back(
            BIND(&TComponentDiscoverer::GetInstances, Unretained(this), component)
                .AsyncVia(GetCurrentInvoker())
                .Run());
    }

    auto responses = WaitFor(AllSucceeded(asyncInstances))
        .ValueOrThrow();

    std::vector<TClusterComponentInstance> instances;

    // Да, ровно как std::iter::zip в Rust или zip в Python: параллельно итерируется по нескольким диапазонам и на каждом шаге выдаёт
    // кортеж элементов с одинаковым индексом. Тут — (component_i, responses_i), чтобы
    // для каждого типа компонента взять соответствующий результат фьючи. Длина — по самому короткому контейнеру (это написано в //!-комменте над Zip).


    // for каждого component_type из enum EClusterComponentType:
    //     instances_этого_типа = responses[index_of(component_type)]
    //     instances.append_all(instances_этого_типа)

//     for (const [component, componentInstances] of zip(
//     getDomainValues(EClusterComponentType),
//     responses,
// )) {
//         instances.push(...componentInstances);
// }

    for (auto&& [component, componentInstances] :
        Zip(TEnumTraits<EClusterComponentType>::GetDomainValues(), responses))
    {
        // а for (auto& x : componentInstances) instances.push_back(std::move(x));
        std::ranges::move(componentInstances, std::back_inserter(instances));
    }

    return instances;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
