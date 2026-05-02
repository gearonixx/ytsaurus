#include "user_access_validator.h"

#include "config.h"

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/connection.h>
#include <yt/yt/ytlib/api/native/config.h>

#include <yt/yt/ytlib/hive/cluster_directory.h>

#include <yt/yt/core/misc/async_expiring_cache.h>

#include <yt/yt/core/rpc/dispatcher.h>

namespace NYT::NSecurityServer {

using namespace NApi;
using namespace NConcurrency;
using namespace NLogging;
using namespace NSecurityClient;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

// @gearonixx
// Ключ кэша = (имя юзера, опциональное имя кластера). Кластер опциональный,
// потому что обычно проверяем юзера на локальном кластере (cluster = nullopt),
// но при multiproxy-запросах ещё и на удалённом — тогда в ключе сидит имя того кластера.
using TUserBanCacheKey = std::pair<std::string, std::optional<std::string>>;

DECLARE_REFCOUNTED_CLASS(TUserBanCache)

// @gearonixx
// Кэш бан-флагов юзеров. TAsyncExpiringCache — это YT-шный шаблон асинхронного
// кэша с TTL: ты переопределяешь DoGet (как сходить за значением), а кэш сам решает,
// что просрочено и что надо перезапросить. Value тут void — нам не нужно само значение,
// нам нужен сам факт «успех или ошибка»: если юзер забанен / не существует, DoGet кидает
// исключение, кэш запоминает Failed-результат, и любой ValidateUser сразу падает.
class TUserBanCache
    : public TAsyncExpiringCache<TUserBanCacheKey, void>
{
public:
    TUserBanCache(
        TAsyncExpiringCacheConfigPtr config,
        NNative::IConnectionPtr connection,
        TLogger logger)
        : TAsyncExpiringCache(std::move(config), GetInvoker(), logger.WithTag("Cache: UserBan"))
        , Connection_(std::move(connection))
        , Logger(std::move(logger))
        // @gearonixx
        // IConnection — это YT-овая «связь с кластером» (знает мастеры, маршруты).
        // CreateNativeClient(Root()) делает из неё клиент, который ходит к мастерам
        // от имени root — нужно, потому что мы будем читать //sys/users/.../@banned
        // (системная ветка Cypress, обычным юзерам не положено читать чужие флаги).
        , Client_(Connection_->CreateNativeClient(NNative::TClientOptions::Root()))
    { }

private:
    const NNative::IConnectionPtr Connection_;
    const TLogger Logger;
    const NNative::IClientPtr Client_;

    // @gearonixx
    // Heavy invoker — общий пул потоков YT для «тяжёлых» (блокирующих/CPU-bound) задач.
    // Кэшу нужен инвокер, чтобы запускать перезапросы DoGet не на текущем потоке вызова.
    static IInvokerPtr GetInvoker()
    {
        return NRpc::TDispatcher::Get()->GetHeavyInvoker();
    }

    // @gearonixx
    // DoGet — точка, которую TAsyncExpiringCache дёргает, когда в кэше нет валидного значения
    // под этот ключ. Возвращает TFuture<void> (асинхронный «обещанный результат» в YT).
    // TFuture<void> здесь = «когда-нибудь в будущем либо OK, либо ошибка» — кэш использует
    // именно ошибку как сигнал «юзер забанен / не существует».
    //
    // Логика: если cluster не задан — просто проверяем юзера на локальном кластере.
    // Если задан (multiproxy-кейс) — проверяем сразу в двух местах: локально И на удалённом
    // кластере, и обе проверки должны успешно пройти (AllSucceeded).
    TFuture<void> DoGet(const TUserBanCacheKey& cacheKey, bool /*isPeriodicUpdate*/) noexcept override
    {
        const auto& [user, cluster] = cacheKey;
        YT_LOG_DEBUG("Getting user ban flag (User: %v, Cluster: %v)",
            user,
            cluster);

        if (!cluster) {
            return CheckUser(Client_, user);
        }

        std::vector<TFuture<void>> futures;
        // Check user on local cluster first.
        // @gearonixx
        // Get(...) — это метод родительского TAsyncExpiringCache: он либо вернёт
        // уже закэшированный фьючер для (user, nullopt), либо запустит свежий DoGet.
        // То есть локальная проверка переиспользует тот же кэш, не делает лишний RPC.
        futures.push_back(Get(TUserBanCacheKey(user, std::nullopt)));

        // @gearonixx
        // Получаем IConnection до удалённого кластера через cluster directory
        // (InsistentGetRemoteConnection ждёт, пока кластер появится в каталоге, а не падает
        // сразу). Затем .Apply(BIND(...)) — это YT-шный аналог .then(): когда фьючер с
        // соединением резолвится, на нём строится IClient от root. BIND оборачивает лямбду
        // в callable, которое умеют дёргать YT-фьючеры/инвокеры.
        auto multiproxyClientFuture = NNative::InsistentGetRemoteConnection(
            Connection_,
            *cluster,
            NNative::EInsistentGetRemoteConnectionMode::WaitFirstSuccessfulSync)
            .Apply(BIND([cluster=*cluster] (const TErrorOr<NNative::IConnectionPtr>& connectionOrError) {
                if (connectionOrError.IsOK()) {
                    auto connection = connectionOrError.Value();
                    auto client = connection->CreateNativeClient(NNative::TClientOptions::Root());
                    return client;
                } else {
                    THROW_ERROR_EXCEPTION("Cannot resolve multiproxy target cluster %Qv", cluster)
                        << connectionOrError;
                }
            }));

        // @gearonixx
        // Когда клиент к удалённому кластеру готов — дёргаем CheckUser уже с ним.
        // MakeStrong(this) — берём сильную ссылку на себя в лямбду, чтобы кэш не умер
        // пока асинхронный запрос летит (TIntrusivePtr работает по подсчёту ссылок).
        auto remoteBan = multiproxyClientFuture.Apply(BIND([this_ = MakeStrong(this), user=user] (const NNative::IClientPtr& client) {
            return this_->CheckUser(client, user);
        }));

        futures.push_back(std::move(remoteBan));

        // @gearonixx
        // AllSucceeded — фьючер, который OK только если ОБА (локальный и удалённый)
        // успешны. Если хоть один кинет «забанен» — общий фьючер тоже упадёт, и кэш
        // запомнит этот провал.
        return AllSucceeded(std::move(futures));
    }

    // @gearonixx
    // Реальная проверка одного юзера на одном кластере. Читает атрибут @banned ноды
    // //sys/users/{user} в Cypress (это файловое дерево YT, /sys/users/ — системная
    // папка со всеми юзерами, у каждого есть атрибут banned: bool).
    //
    // Опции запроса: ReadFrom = Cache — читаем из мастер-кэша (быстрее, допускаем
    // лёгкое отставание); три Suppress* флага отключают тяжёлые синхронизации
    // транзакций — для read-only проверки бан-флага они не нужны.
    TFuture<void> CheckUser(const NNative::IClientPtr& client, const std::string& user) noexcept
    {
        auto options = TGetNodeOptions();
        options.ReadFrom = EMasterChannelKind::Cache;
        options.SuppressUpstreamSync = true;
        options.SuppressTransactionCoordinatorSync = true;
        options.SuppressStronglyOrderedTransactionBarrier = true;
        auto clusterName = client->GetNativeConnection()->GetStaticConfig()->ClusterName;
        // @gearonixx
        // ToYPathLiteral экранирует имя юзера для YPath (Cypress-овый аналог пути).
        // GetNode возвращает TFuture с YSON-значением, дальше .Apply разбирает результат:
        // - если ResolveError — ноды нет, значит юзера нет → кидаем «No such user»;
        // - иначе парсим bool и кидаем «User banned», если true.
        // THROW_ERROR в .Apply превращает фьючер в Failed, что и ловит TAsyncExpiringCache.
        return client->GetNode("//sys/users/" + ToYPathLiteral(user) + "/@banned", options).Apply(
            BIND([user, clusterName, this, this_ = MakeStrong(this)] (const TErrorOr<TYsonString>& resultOrError) {
                if (!resultOrError.IsOK()) {
                    TError wrappedError;
                    if (resultOrError.FindMatching(NYTree::EErrorCode::ResolveError)) {
                        wrappedError = TError("No such user %Qv",
                            user);
                    } else {
                        wrappedError = TError("Error getting user info for user %Qv",
                            user);
                    }
                    wrappedError <<= resultOrError;
                    YT_LOG_WARNING(wrappedError);
                    THROW_ERROR wrappedError;
                }

                auto banned = ConvertTo<bool>(resultOrError.Value());

                YT_LOG_DEBUG("Got user ban flag (User: %v, Banned: %v)",
                    user,
                    banned);

                if (banned) {
                    THROW_ERROR_EXCEPTION("User %Qv is banned on cluster %Qv",
                        user,
                        clusterName.value_or("unknown"));
                }
            }));
    }
};

DEFINE_REFCOUNTED_TYPE(TUserBanCache)

////////////////////////////////////////////////////////////////////////////////

// @gearonixx
// TUserAccessValidator — публичная обёртка над TUserBanCache, реализующая интерфейс
// IUserAccessValidator (см. .h). Тонкая прослойка: всё, что она делает — превращает
// синхронный вызов ValidateUser(user, cluster) в поход через кэш. Ровно её через
// фабрику CreateUserAccessValidator берёт TApi и зовёт в TApi::ValidateUser.
class TUserAccessValidator
    : public IUserAccessValidator
{
public:
    TUserAccessValidator(
        TUserAccessValidatorDynamicConfigPtr config,
        NNative::IConnectionPtr connection,
        TLogger logger)
        : UserCache_(New<TUserBanCache>(
            config->BanCache,
            std::move(connection),
            std::move(logger)))
    { }

    // @gearonixx
    // YT_ASSERT_THREAD_AFFINITY_ANY — декларирует, что метод можно звать с любого потока
    // (в YT методы часто привязаны к конкретному инвокеру; здесь — нет ограничений).
    //
    // Логика: сначала Find — это синхронный «глянь, нет ли уже готового результата
    // в кэше», без сетевых походов. Если есть — сразу ThrowOnError (если был фейл —
    // кидаем). Если нет — Get запускает асинхронный DoGet, а WaitForFast блокирующе
    // ждёт фьючер (Fast = без переключения файбера, если уже готов). В итоге
    // ValidateUser синхронный снаружи, но внутри умный кэш гасит большинство походов.
    void ValidateUser(const std::string& user, const std::optional<std::string>& cluster) override
    {
        YT_ASSERT_THREAD_AFFINITY_ANY();

        auto result = UserCache_->Find({user, cluster});
        if (result) {
            result->ThrowOnError();
        }

        WaitForFast(UserCache_->Get({user, cluster}))
            .ThrowOnError();
    }

    // @gearonixx
    // Применить новый конфиг кэша на лету (TTL, размеры) — вызывается, когда
    // dynamic config manager в http_proxy получает обновлённый конфиг.
    void Reconfigure(const TUserAccessValidatorDynamicConfigPtr& config) override
    {
        YT_ASSERT_THREAD_AFFINITY_ANY();

        UserCache_->Reconfigure(config->BanCache);
    }

private:
    const TUserBanCachePtr UserCache_;
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx
// Фабрика — типовой YT-паттерн: класс TUserAccessValidator живёт в .cpp (приватный),
// наружу торчит только интерфейс IUserAccessValidator (.h) и эта функция, которая
// его создаёт. Так клиенты не зависят от внутренностей реализации.
IUserAccessValidatorPtr CreateUserAccessValidator(
    TUserAccessValidatorDynamicConfigPtr config,
    NNative::IConnectionPtr connection,
    NLogging::TLogger logger)
{
    return New<TUserAccessValidator>(
        std::move(config),
        std::move(connection),
        std::move(logger));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSecurityServer
