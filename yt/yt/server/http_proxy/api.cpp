#include "api.h"

#include "access_checker.h"
#include "bootstrap.h"
#include "config.h"
#include "context.h"
#include "dynamic_config_manager.h"
#include "private.h"

#include <yt/yt/server/lib/misc/profiling_helpers.h>

#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/core/http/helpers.h>

#include <yt/yt/core/misc/finally.h>

namespace NYT::NHttpProxy {

using namespace NConcurrency;
using namespace NFormats;
using namespace NHttp;
using namespace NNet;
using namespace NProfiling;
using namespace NSecurityClient;
using namespace NSecurityServer;
using namespace NYson;
using namespace NYTree;
using namespace NServer;

////////////////////////////////////////////////////////////////////////////////

// @gearonixx
// RAII-обёртка над «занятым слотом» в семафоре параллельных запросов TApi.
// AcquireSemaphore возвращает такой guard, и пока он жив — слот считается занятым;
// деструктор автоматически зовёт ReleaseSemaphore. Сырой TApi* (не TIntrusivePtr),
// потому что guard живёт строго внутри обработки одного запроса, где TApi гарантированно
// существует — лишний счётчик ссылок не нужен.
TSemaphoreGuard::TSemaphoreGuard(TApi* api, const TUserCommandPair& key)
    : Api_(api)
    , Key_(key)
{ }

TSemaphoreGuard::~TSemaphoreGuard()
{
    if (Api_) {
        Api_->ReleaseSemaphore(Key_);
    }
}

////////////////////////////////////////////////////////////////////////////////

// @gearonixx
// TApi — обработчик HTTP-запросов на /api/*. Сам ничего не создаёт, в конструкторе
// только вытягивает из бутстрапа указатели на нужные подсистемы (драйверы, аутентификатор,
// координатор, пуллер, memory tracker).
//
// Когда вызывается: HTTP-сервер диспатчит сюда любой запрос с префиксом /api/ —
// это весь публичный YT API клиентов (Python SDK, CLI `yt`, C++ клиенты через HTTP).
// Например POST /api/v4/get, /api/v3/list, /api/v4/write_table — каждая команда драйвера
// приходит как HTTP-запрос и попадает в TApi::HandleRequest через хендлер,
// зарегистрированный в bootstrap.cpp: server->AddHandler("/api/", AllowCors(Api_)).
//
// Задачи: HandleRequest (создать TContext и прогнать запрос), семафоры на параллельность
// (AcquireSemaphore + TSemaphoreGuard), авторизация (ValidateUser, CheckAccess, BanCache_),
// метрики (BytesIn_/Out_, HttpCodes_), классификация сети (Networks_), подписка на
// динамический конфиг и публикация состояния в Orchid.
//
// Зачем в конструктор передаём целый TBootstrap*, а не отдельные подсистемы:
// TBootstrap работает как локатор сервисов (GetDriverV4, GetCoordinator, GetPoller, ...),
// и TApi берёт у него готовые указатели вместо того, чтобы перечислять 10+ параметров
// в сигнатуре конструктора.
TApi::TApi(TBootstrap* bootstrap)
    : Config_(bootstrap->GetConfig()->Api)
    , DynamicConfig_(New<TApiDynamicConfig>())
    , DriverV3_(bootstrap->GetDriverV3())
    , DriverV4_(bootstrap->GetDriverV4())
    , HttpAuthenticator_(bootstrap->GetHttpAuthenticator())
    , Coordinator_(bootstrap->GetCoordinator())
    , AccessChecker_(bootstrap->GetAccessChecker())
    , ControlInvoker_(bootstrap->GetControlInvoker())
    , Poller_(bootstrap->GetPoller())
    , MemoryUsageTracker_(bootstrap->GetMemoryUsageTracker())
    // @gearonixx
    // Единственный филд, который не тянется готовым из бутстрапа, а создаётся
    // прямо тут через фабрику CreateUserAccessValidator (лежит в yt/yt/server/lib/security_server/).
    // Проверяет, существует ли такой юзер в кластере и не забанен ли он, перед тем
    // как пустить его запрос дальше — используется в TApi::ValidateUser.
    , UserAccessValidator_(CreateUserAccessValidator(
        Config_->UserAccessValidator,
        bootstrap->GetNativeConnection(),
        HttpProxyLogger()))
    , DefaultNetworkName_(bootstrap->GetConfig()->DefaultNetwork)
{
    // @gearonixx
    // Конфиг хранит карту «имя_сети -> [список IPv6-префиксов]», а нам удобнее
    // обратное: «префикс -> имя сети», чтобы быстро тегировать клиентский адрес
    // при сборе метрик. Разворачиваем в плоский вектор пар.
    for (const auto& network : bootstrap->GetConfig()->Networks) {
        for (const auto& prefix : network.second) {
            Networks_.emplace_back(prefix, network.first);
        }
    }

    // @gearonixx
    // Сортируем по длине маски от большей к меньшей — это longest-prefix match:
    // адрес может попадать сразу в /48 и в /32, а нам нужен самый специфичный.
    // Дальше в GetNetworkNameForAddress линейный проход берёт первое попадание.
    std::sort(Networks_.begin(), Networks_.end(), [] (auto&& lhs, auto&& rhs) {
        return lhs.first.GetMaskSize() > rhs.first.GetMaskSize();
    });

    // @gearonixx
    // Подписываемся на изменения динамического конфига кластера. BIND_NO_PROPAGATE —
    // оборачивает метод в callable без проброса trace-контекста (нам тут не нужен).
    // MakeWeak(this) — слабая ссылка на себя в подписке, чтобы менеджер конфигов
    // не держал TApi живым искусственно (циклы между TApi и менеджером не нужны).
    const auto& dynamicConfigManager = bootstrap->GetDynamicConfigManager();
    dynamicConfigManager->SubscribeBeforeConfigChanged(BIND_NO_PROPAGATE(&TApi::OnDynamicConfigChanged, MakeWeak(this)));
}

// @gearonixx
// По IP клиента возвращает имя сети для тегирования метрик (например, «backbone»,
// «external»). IPv4 не классифицируем — сразу дефолт. Для IPv6 идём по
// предсортированному списку префиксов (см. конструктор) и берём первый матч —
// он же самый специфичный (longest-prefix).
std::string TApi::GetNetworkNameForAddress(const TNetworkAddress& address) const
{
    if (!address.IsIP6()) {
        return DefaultNetworkName_;
    }

    auto ip6Address = address.ToIP6Address();
    for (const auto& network : Networks_) {
        if (network.first.Contains(ip6Address)) {
            return network.second;
        }
    }

    return DefaultNetworkName_;
}

const NDriver::IDriverPtr& TApi::GetDriverV3() const
{
    return DriverV3_;
}

const NDriver::IDriverPtr& TApi::GetDriverV4() const
{
    return DriverV4_;
}

const TCompositeHttpAuthenticatorPtr& TApi::GetHttpAuthenticator() const
{
    return HttpAuthenticator_;
}

const TCoordinatorPtr& TApi::GetCoordinator() const
{
    return Coordinator_;
}

const TApiConfigPtr& TApi::GetConfig() const
{
    return Config_;
}

// @gearonixx
// DynamicConfig_ — это TAtomicIntrusivePtr, потокобезопасный «слот» под shared-ptr.
// Acquire() — атомарно скопировать текущий указатель. Под нагрузкой это lock-free
// чтение; писатель (OnDynamicConfigChanged) делает .Store() с новым конфигом —
// читатели, успевшие захватить старый, спокойно дочитывают, новые получают свежий.
TApiDynamicConfigPtr TApi::GetDynamicConfig() const
{
    return DynamicConfig_.Acquire();
}

const IPollerPtr& TApi::GetPoller() const
{
    return Poller_;
}

const INodeMemoryTrackerPtr& TApi::GetMemoryUsageTracker() const
{
    return MemoryUsageTracker_;
}

void TApi::ValidateUser(const std::string& user)
{
    UserAccessValidator_->ValidateUser(user);
}

TError TApi::CheckAccess(const std::string& user)
{
    return AccessChecker_->CheckAccess(user);
}

int TApi::GetNumberOfConcurrentRequests()
{
    return GlobalSemaphore_.load();
}

// @gearonixx
// Лимитирование параллельных запросов на двух уровнях:
//   1) Глобальный — общий счётчик GlobalSemaphore_ на всю прокси, потолок = ConcurrencyLimit*2.
//      Зачем *2: чтобы под пиками не отрезать всех сразу — небольшой запас сверху лимита.
//   2) Локальный — на пару (user, command). У каждого юзера своя доля от ConcurrencyLimit
//      (DefaultUserConcurrencyLimitRatio либо персональный коэффициент из dynamic config),
//      это защищает «одного шумного юзера от съедения всей прокси».
//
// Если оба лимита прошли — счётчики +1, возвращаем TSemaphoreGuard (RAII-слот).
// Если нет — возвращаем nullopt, и TApi::HandleRequest на это смотрит как на «занято».
std::optional<TSemaphoreGuard> TApi::AcquireSemaphore(const std::string& user, const TString& command)
{
    // @gearonixx
    // Lock-free инкремент глобального счётчика через CAS-цикл: загружаем текущее value,
    // если уже >= 2*limit — отказ; иначе compare_exchange_weak пытается сменить
    // value -> value+1. weak может ложно фейлиться, поэтому в цикле; это нормально.
    auto value = GlobalSemaphore_.load();
    do {
        if (value >= Config_->ConcurrencyLimit * 2) {
            return {};
        }
    } while (!GlobalSemaphore_.compare_exchange_weak(value, value + 1));

    auto key = std::pair(user, command);
    auto counters = GetProfilingCounters(key);

    auto userConcurrencyLimitRatio = GetOrDefault(
        DynamicConfig_.Acquire()->UserToConcurrencyLimitRatio,
        user,
        DynamicConfig_.Acquire()->DefaultUserConcurrencyLimitRatio);

    // @gearonixx
    // Если перебрали локальный лимит юзера — откатываем уже инкрементированный
    // глобальный счётчик и возвращаем «занято».
    auto userConcurrencyLimit = static_cast<int>(static_cast<double>(Config_->ConcurrencyLimit) * userConcurrencyLimitRatio);
    if (counters->LocalSemaphore >= userConcurrencyLimit) {
        GlobalSemaphore_.fetch_add(-1);
        return {};
    }

    counters->ConcurrencySemaphore.Update(++counters->LocalSemaphore);

    return TSemaphoreGuard(this, key);
}

void TApi::ReleaseSemaphore(const TUserCommandPair& key)
{
    auto counters = GetProfilingCounters(key);

    GlobalSemaphore_.fetch_add(-1);
    counters->ConcurrencySemaphore.Update(--counters->LocalSemaphore);
}

// @gearonixx
// Ленивое создание набора метрик на пару (user, command). Counters_ — TSyncMap,
// потокобезопасная хэш-мапа YT с FindOrInsert: либо вернёт существующее,
// либо вызовет лямбду создания ровно один раз. SparseProfiler — variant профайлера,
// который не публикует метрики, пока их не инкрементировали (экономит сенсоры,
// иначе на каждую (user, command)-пару заведётся куча нулевых тайм-серий).
//
// WithTag добавляет лейблы; имена вида "/request_count" — пути в Solomon-дереве метрик.
TApi::TProfilingCounters* TApi::GetProfilingCounters(const TUserCommandPair& key)
{
    return Counters_.FindOrInsert(key, [&, this] {
        auto profiler = SparseProfiler_
            .WithTag("user", key.first)
            .WithTag("command", key.second);

        auto counters = std::make_unique<TProfilingCounters>();
        counters->ConcurrencySemaphore = profiler.Gauge("/concurrency_semaphore");
        counters->RequestCount = profiler.Counter("/request_count");
        counters->RequestWallTime = profiler.Timer("/request_duration");
        counters->CumulativeRequestCpuTime = profiler.TimeCounter("/cumulative_request_cpu_time");
        return counters;
    }).first->get();
}

// @gearonixx
// Глобальный счётчик ответов по HTTP-коду (200, 404, 500, ...). Один счётчик на код,
// без разреза по юзеру — общая картина «сколько каких ответов отдала прокси».
void TApi::IncrementHttpCode(EStatusCode httpStatusCode)
{
    auto counter = HttpCodes_.FindOrInsert(httpStatusCode, [&] {
        return HttpProxyProfiler()
            .WithTag("http_code", ToString(static_cast<int>(httpStatusCode)))
            .Counter("/http_code_count");
    }).first;

    counter->Increment();
}

// @gearonixx
// Универсальный хелпер: лениво заводит счётчик с тегами (user, <tagName>=<tagValue>)
// и инкрементирует. Используется в IncrementBytesIn/Out для разрезов по сети,
// формату данных, типу сжатия.
void TApi::IncrementUserCounter(
    TUserCounterMap* counterMap,
    const std::string& user,
    const std::string& networkName,
    const std::string& counterName,
    const std::string& tagName,
    const std::string& tagValue,
    i64 value)
{
    counterMap->FindOrInsert(std::pair(user, networkName), [&, this] {
        return SparseProfiler_
            .WithTag("user", user)
            .WithTag(tagName, tagValue)
            .Counter(counterName);
    }).first->Increment(value);
}

// @gearonixx
// Регистрирует исходящий трафик одного запроса в трёх разрезах:
// total bytes_out по (user, network), bytes_out по формату ответа (json/yson/...)
// и по типу сжатия (gzip/brotli/...). Вход — TFormat и TContentEncoding опциональны,
// потому что не у каждого ответа есть формат/сжатие (например, бинарные стримы).
void TApi::IncrementBytesOutProfilingCounters(
    const std::string& user,
    const TNetworkAddress& clientAddress,
    i64 bytesOut,
    const std::optional<TFormat>& outputFormat,
    const std::optional<TContentEncoding>& outputCompression)
{
    auto networkName = GetNetworkNameForAddress(clientAddress);

    IncrementUserCounter(&BytesOut_, user, networkName, "/bytes_out", "network", networkName, bytesOut);

    if (outputFormat) {
        IncrementUserCounter(
            &OutputFormatBytes_,
            user,
            networkName,
            "/bytes_out_by_format",
            "format",
            FormatEnum(outputFormat->GetType()),
            bytesOut);
    }

    if (outputCompression) {
        IncrementUserCounter(
            &OutputCompressionBytes_,
            user,
            networkName,
            "/bytes_out_by_compression",
            "compression",
            *outputCompression,
            bytesOut);
    }
}

// @gearonixx
// Симметрично IncrementBytesOut, только для входящего тела запроса
// (тело POST: например, write_table). Те же три разреза.
void TApi::IncrementBytesInProfilingCounters(
    const std::string& user,
    const TNetworkAddress& clientAddress,
    i64 bytesIn,
    const std::optional<TFormat>& inputFormat,
    const std::optional<TContentEncoding>& inputCompression)
{
    auto networkName = GetNetworkNameForAddress(clientAddress);

    IncrementUserCounter(&BytesIn_, user, networkName, "/bytes_in", "network", networkName, bytesIn);

    if (inputFormat) {
        IncrementUserCounter(
            &InputFormatBytes_,
            user,
            networkName,
            "/bytes_in_by_format",
            "format",
            FormatEnum(inputFormat->GetType()),
            bytesIn);
    }

    if (inputCompression) {
        IncrementUserCounter(
            &InputCompressionBytes_,
            user,
            networkName,
            "/bytes_in_by_compression",
            "compression",
            *inputCompression,
            bytesIn);
    }
}

// @gearonixx
// «Главный» хук метрик одного запроса: дёргается из TContext в самом конце
// (см. context.cpp::LogAndProfile). Инкрементит request_count, пишет wall-time
// и cpu-time, затем разрезает HTTP-код по командам и по юзерам, и отдельно
// считает ошибки YT-уровня по api error code (например, NoSuchUser).
void TApi::IncrementProfilingCounters(
    const std::string& user,
    const TString& command,
    std::optional<EStatusCode> httpStatusCode,
    TErrorCode apiErrorCode,
    TDuration wallTime,
    TDuration cpuTime,
    const TNetworkAddress& clientAddress)
{
    auto networkName = GetNetworkNameForAddress(clientAddress);

    auto* counters = GetProfilingCounters({user, command});

    counters->RequestCount.Increment();
    counters->RequestWallTime.Record(wallTime);
    counters->CumulativeRequestCpuTime.Add(cpuTime);

    if (httpStatusCode) {
        HttpCodesByCommand_.FindOrInsert({command, *httpStatusCode}, [&, this] {
            return SparseProfiler_
                .WithTag("http_code", ToString(static_cast<int>(*httpStatusCode)))
                .WithTag("command", command)
                .Counter("/command_http_code_count");
        }).first->Increment();

        HttpCodesByUser_.FindOrInsert({user, *httpStatusCode}, [&, this] {
            return SparseProfiler_
                .WithTag("http_code", ToString(static_cast<int>(*httpStatusCode)))
                .WithTag("user", user)
                .Counter("/user_http_code_count");
        }).first->Increment();
    }

    if (apiErrorCode) {
        auto errorCodeInfo = TErrorCodeRegistry::Get()->Get(apiErrorCode);
        counters->ApiErrors.FindOrInsert(apiErrorCode, [&, this] {
            return SparseProfiler_
                .WithTag("user", user)
                .WithTag("command", command)
                .WithTag("error_code", ToString(errorCodeInfo))
                .Counter("/api_error_count");
        }).first->Increment();
    }
}

// @gearonixx
// Промежуточное досыпание CPU-time в счётчик команды — зовётся периодически
// в течение выполнения долгого запроса (см. context.cpp с CpuUpdatePeriod),
// чтобы метрика «росла онлайн», а не появлялась только в самом конце.
void TApi::IncrementCpuProfilingCounter(
    const std::string& user,
    const TString& command,
    TDuration cpuTime)
{
    auto* counters = GetProfilingCounters({user, command});
    counters->CumulativeRequestCpuTime.Add(cpuTime);
}

// @gearonixx
// Точка входа из HTTP-сервера: AddHandler("/api/", ...) → сюда. Жизненный цикл запроса:
//   1) Создаётся TContext — он держит весь стейт обработки одного запроса
//      (req, rsp, парсинг команды, аутентификация, авторизация, формат ввода/вывода).
//   2) TryPrepare — распарсить URL и заголовки, понять команду, проверить юзера,
//      сходить в семафор. Если что-то не так (400/401/...) — фиксируем prepare-error
//      и выходим, ответ уже записан внутри TContext.
//   3) FinishPrepare → Run — собственно вызвать драйвер (DriverV3/V4) и пропихать
//      запрос/ответ через стримы. Любое исключение тут оборачиваем в TError.
//   4) Finally + Finalize — гарантированно записать LogAndProfile (метрики, лог)
//      и закрыть ответ, даже если Run упал. Finally — RAII-обёртка, которая зовёт
//      лямбду в деструкторе.
void TApi::HandleRequest(
    const IRequestPtr& req,
    const IResponseWriterPtr& rsp)
{
    auto context = New<TContext>(MakeStrong(this), req, rsp);
    try {
        if (!context->TryPrepare()) {
            PrepareErrorCount_.Increment();
            auto statusCode = rsp->GetStatus();
            if (statusCode) {
                IncrementHttpCode(*statusCode);
            }
            return;
        }

        context->FinishPrepare();
        context->Run();
    } catch (const std::exception& ex) {
        context->SetEnrichedError(TError(ex));
    }

    auto finally = Finally([&] {
        context->LogAndProfile();
    });
    context->Finalize();
}

// @gearonixx
// Колбэк подписки SubscribeBeforeConfigChanged. Атомарно подменяет хранимый
// API-конфиг (.Store) и пробрасывает новые настройки в UserAccessValidator
// (там — TTL/размеры ban-кэша). Старые читатели DynamicConfig_ дочитывают
// предыдущий конфиг безопасно — TAtomicIntrusivePtr этим и хорош.
void TApi::OnDynamicConfigChanged(
    const TProxyDynamicConfigPtr& /*oldConfig*/,
    const TProxyDynamicConfigPtr& newConfig)
{
    YT_ASSERT_THREAD_AFFINITY_ANY();

    DynamicConfig_.Store(newConfig->Api);
    UserAccessValidator_->Reconfigure(newConfig->Api->UserAccessValidator);
}

////////////////////////////////////////////////////////////////////////////////

// @gearonixx
// Поставщик данных для Orchid (это YT-овая «онлайн-инспекция» процесса:
// дерево YPath-сервисов, доступное по /orchid/.../api). Сюда дампим срез
// глобальной памяти с разрезами по тегам user/command — DumpGlobalMemoryUsageSnapshot
// сам обходит memory tracker и пишет YSON-карту.
//
// BuildYsonFluently — fluent-API для пошагового формирования YSON-объекта в consumer.
void TApi::BuildOrchid(IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .DoMap([] (TFluentMap fluent) {
            DumpGlobalMemoryUsageSnapshot(
                fluent.GetConsumer(),
                {
                    HttpProxyUserAllocationTagKey,
                    HttpProxyCommandAllocationTagKey,
                });
        });
}

// @gearonixx
// Регистрация в Orchid: оборачиваем BuildOrchid в IYPathService.
// FromProducer создаёт service, который при чтении вызывает BIND-callback.
// ->Via(ControlInvoker_) — гарантирует, что обработка запроса к этому
// orchid-узлу пойдёт на control-потоке (а не на потоке HTTP-сервера).
// Дёргается из bootstrap.cpp:433 при сборке orchid-дерева прокси.
IYPathServicePtr TApi::CreateOrchidService()
{
    return IYPathService::FromProducer(BIND_NO_PROPAGATE(&TApi::BuildOrchid, MakeStrong(this)))
        ->Via(ControlInvoker_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
