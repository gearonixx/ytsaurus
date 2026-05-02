#include "access_checker.h"

#include "bootstrap.h"
#include "config.h"
#include "coordinator.h"
#include "dynamic_config_manager.h"

#include <yt/yt/ytlib/security_client/permission_cache.h>

#include <library/cpp/yt/threading/atomic_object.h>

namespace NYT::NHttpProxy {

using namespace NConcurrency;
// NSecurityClient — неймспейс YT с клиентскими типами для авторизации (TPermissionKey, EPermission, TPermissionCache и т

// ● - EPermission — enum: какие бывают права (Read, Write, Use, Administer...).
//   - TPermissionKey — вопрос для проверки: «юзер X, путь Y, право Z, можно?».
    //   - TPermissionCache — кэш ответов на эти вопросы, чтобы не дёргать мастер каждый раз.
using namespace NSecurityClient;
using namespace NYTree;

///
    // inline const NLogging::TLogger& Logger() {
    //     static const NLogging::TLogger result{"AccessChecker"};
    //     return result;
    // }

    //● Да. Это локальный логгер этого .cpp — все YT_LOG_* в access_checker.cpp пишут с категорией "AccessChecker", чтобы в логах было видно откуда строка.
//
// @gearonixx @AI_GENERATED@
// Почему лежит на уровне файла, а не внутри класса:
//   1) YT_LOG_INFO/ERROR — макрос, разворачивается в Logger().Write(...). В точке вызова
//      имя Logger должно быть видно компилятору.
//   2) Если положить Logger в класс — его увидят только методы класса. А лог нередко
//      пишут свободные функции/хелперы прямо в этом же .cpp, им класс не указ.
//   3) Файловый scope = видно из любого места этого .cpp.
//   4) static — внутренняя линковка: запирает имя внутри файла, чтобы это Logger()
//      не столкнулось с одноимённым Logger() из соседних .cpp (у каждого свой логгер
//      со своей категорией: "AccessChecker", "HttpProxy" и т.д.).
static YT_DEFINE_GLOBAL(const NLogging::TLogger, Logger, "AccessChecker");

////////////////////////////////////////////////////////////////////////////////

class TAccessChecker
    : public IAccessChecker
{
public:
    // @gearonixx @AI_GENERATED@
    // HTTP-прокси — входная дверь в кластер YT по обычному HTTP/HTTPS: CLI (yt), Python SDK
    // по HTTP, веб-морда, внешние интеграции стучатся сюда, а прокси переводит их запросы
    // в бинарный RPC к мастерам и нодам внутри кластера. Мастера и ноды между собой говорят
    // по приватному RPC и наружу не выставлены — прокси даёт стабильный публичный HTTP-API,
    // плюс на нём висит аутентификация (TVM/blackbox/токены), rate limiting и вот этот самый
    // access check по ролям.
    //
    // Проверяет, можно ли конкретному пользователю ходить запросами через эту HTTP-прокси.
    // Каждая прокси-машина имеет "роль" — логическую группу прокси (обычно одна группа
    // обслуживает один тип нагрузки или одного клиента). Право использовать роль выражается
    // через permission Use на узле в Cypress (дерево метаданных YT, единая точка хранения ACL):
    // либо PathPrefix/Role, либо PathPrefix/Role/principal — второе для нового режима ACO
    // (Access Control Object: ACL хранится не на самом узле, а на отдельном объекте-принципале).
    // Дёргается из TApi::CheckAccess на каждом HTTP-запросе после аутентификации.
    //
    // Конструктор: тянет статический конфиг и текущую роль из bootstrap, подписывается на
    // (1) смену собственной роли — coordinator может переназначить прокси в другую группу
    // на лету при ребалансировке; (2) изменение динамического конфига — включить/выключить
    // чекер без рестарта процесса.
    // BIND_NO_PROPAGATE — обёртка YT для callback, не таскающая trace context родительского
    // запроса (иначе все срабатывания подписки висели бы в трейсе того запроса, который
    // случайно успел раньше всех). MakeWeak(this) — слабая TIntrusivePtr-ссылка, чтобы
    // подписка не держала checker живым после shutdown (стандартный приём против циклов
    // владения и use-after-free на выключении).
    explicit TAccessChecker(TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
        , Config_(Bootstrap_->GetConfig()->AccessChecker)
    // Берём у координатора нашу собственную запись в реестре (TProxyEntry этой прокси) и читаем поле Role — строку текущей роли ("data", "control" и т.п.).

   //  ● Self_ — не «одна из многих», а указатель на свою же запись среди них. Координатор — компонент прокси A, в его Proxies_ лежат [A, B, C, D], и Self_ показывает на A. Это просто шорткат, чтобы не искать себя в
   // списке каждый раз.

    //
        , ProxyRole_(Bootstrap_->GetCoordinator()->GetSelfEntry()->Role)
    // is access checker enabled?
        , Enabled_(Config_->Enabled)
    {
        // ● Сервис прокси, который держит в Cypress (//sys/proxies) реестр всех живых HTTP-прокси — их роли, баны, нагрузку — и стреляет сигналом OnSelfRoleChanged, когда роль текущей прокси меняется. Здесь
        //   TAccessChecker подписывается на этот сигнал, чтобы пересчитать ACL когда прокси переедет в другую роль.
  //       Поддерживает в Cypress (//sys/proxies) живой реестр HTTP-прокси: периодически пишет туда свою запись (роль, нагрузку, бан) и читает чужие, чтобы знать кто ещё живой. По этому реестру отвечает на вопросы
  // «кому отдать запрос», «я забанен?», «моя роль сменилась — оповестить подписчиков сигналом».

 //        Поддерживает в Cypress (//sys/proxies) живой реестр HTTP-прокси: периодически пишет туда свою запись (роль, нагрузку, бан) и читает чужие, чтобы знать кто ещё живой. По этому реестру отвечает на вопросы
 // «кому отдать запрос», «я забанен?», «моя роль сменилась — оповестить подписчиков сигналом».


        // ● Нет центрального босса. Каждая прокси внутри себя крутит свой TCoordinator, и все они общаются через общую доску — Cypress (//sys/proxies в мастере). Каждый координатор пишет туда свою запись и читает
        //   чужие. Имя обманчивое: он не координирует других, он координирует свою прокси с остальными через мастер.

  //   ● Да, по одному на прокси. Так делают, чтобы не было single point of failure — иначе центральный координатор упал, и весь флот прокси ослеп. А так состояние живёт в мастере (он и так реплицирован), а прокси
  // stateless и взаимозаменяемы.

        const auto& coordinator = Bootstrap_->GetCoordinator();
        coordinator->SubscribeOnSelfRoleChanged(BIND_NO_PROPAGATE(&TAccessChecker::OnProxyRoleUpdated, MakeWeak(this)));

        const auto& dynamicConfigManager = Bootstrap_->GetDynamicConfigManager();
        dynamicConfigManager->SubscribeBeforeConfigChanged(BIND_NO_PROPAGATE(&TAccessChecker::OnDynamicConfigChanged, MakeWeak(this)));
    }

    // @gearonixx @AI_GENERATED@
    // Основной метод. Возвращает OK — пускаем, иначе — отказ с описанием.
    //
    // YT_ASSERT_THREAD_AFFINITY_ANY — макрос-маркер "звать допустимо из любого потока"
    // (в YT много проверок thread affinity, эта говорит "ограничений нет"). Атомарный
    // флаг Enabled_ позволяет динамическим конфигом мгновенно отключить чекер без рестарта.
    //
    // Path строится как PathPrefix/Role (или PathPrefix/Role/principal в ACO-режиме) —
    // это путь в Cypress, где лежит ACL для этой роли прокси. PermissionCache —
    // клиентский кэш ответов master-сервера на проверки прав; запросы к мастеру дорогие
    // и идут по сети, поэтому ответы кэшируются с TTL. Get возвращает TFuture<void>:
    // future — обещание результата, который придёт асинхронно (концепция как std::future,
    // но с YT-овскими continuations и интеграцией с файберами).
    //
    // WaitFor приостанавливает текущий файбер (легковесный кооперативный поток YT,
    // как корутина — стек хранится отдельно, переключение делает scheduler) до готовности
    // future, затем возвращает её результат. Сам OS-поток при этом не блокируется — он
    // исполняет другие готовые файберы.
    //
    // Три исхода: OK — пускаем; AuthorizationError (мастер явно сказал "нет права Use") —
    // честный отказ юзеру с понятным сообщением; любая другая ошибка (мастер недоступен,
    // таймаут, сеть моргнула) — логируем INFO и всё равно пускаем. Это fail-open: проблема
    // инфраструктуры авторизации не должна валить продовый трафик прокси.
    TError CheckAccess(const std::string& user) const override
    {
        YT_ASSERT_THREAD_AFFINITY_ANY();

        if (!Enabled_.load()) {
            return TError();
        }

        auto proxyRole = Bootstrap_->GetCoordinator()->GetSelfEntry()->Role;
        auto path = Config_->UseAccessControlObjects
            ? Format("%v/%v/principal", Config_->PathPrefix, proxyRole)
            : Format("%v/%v", Config_->PathPrefix, proxyRole);
        const auto& cache = Bootstrap_->GetNativeConnection()->GetPermissionCache();
        auto error = WaitFor(cache->Get(TPermissionKey{
            .Path = path,
            .User = user,
            .Permission = EPermission::Use,
        }));

        if (error.IsOK()) {
            return TError();
        }

        if (error.FindMatching(NSecurityClient::EErrorCode::AuthorizationError)) {
            return TError("User %Qv is not allowed to use HTTP proxies with role %Qv", user, proxyRole)
                << error;
        }

        YT_LOG_INFO(error, "Failed to check if user is allowed to use HTTP proxy (User: %v, Role: %v)",
            user,
            proxyRole);

        return TError();
    }

private:
    TBootstrap const* Bootstrap_;
    const TAccessCheckerConfigPtr Config_;

    NThreading::TAtomicObject<std::string> ProxyRole_;

    std::atomic<bool> Enabled_;

    // @gearonixx @AI_GENERATED@
    // Coordinator — сервис прокси, держащий в Cypress (//sys/proxies) актуальный список
    // всех живых HTTP-прокси с их ролями (data/control/кастом), банами и нагрузкой;
    // периодически обновляет свою запись и читает чужие. Когда админ переписывает роль
    // в Cypress, coordinator стреляет сигналом OnSelfRoleChanged — на него тут и подписаны.
    //
    // То есть зовётся это, когда прокси переводят в другую роль (ребалансировка
    // proxy-пула или ручная переконфигурация). TAtomicObject<std::string> — обёртка
    // YT для безопасного чтения/записи нетривиального объекта без явного мьютекса
    // снаружи (внутри использует spinlock — короткий busy-wait вместо засыпания потока).
    // К слову, в текущей редакции CheckAccess само поле ProxyRole_ не читает — берёт роль
    // напрямую из coordinator каждый раз; поле осталось закэшированным значением,
    // потенциально для метрик/логов.
    void OnProxyRoleUpdated(const std::string& newRole)
    {
        YT_ASSERT_THREAD_AFFINITY_ANY();

        ProxyRole_.Store(newRole);
    }

    // @gearonixx @AI_GENERATED@
    // Динамический конфиг тянется dynamic config manager'ом из специального узла Cypress
    // и обновляется в рантайме без рестарта. Поле Enabled там — optional<bool>: если
    // задано — применяем динамическое значение, иначе откатываемся к статическому из
    // конфига при запуске. Стандартный YT-паттерн "динамический оверрайд поверх статика".
    void OnDynamicConfigChanged(
        const TProxyDynamicConfigPtr& /*oldConfig*/,
        const TProxyDynamicConfigPtr& newConfig)
    {
        YT_ASSERT_THREAD_AFFINITY_ANY();

        Enabled_.store(newConfig->AccessChecker->Enabled.value_or(Config_->Enabled));
    }
};

////////////////////////////////////////////////////////////////////////////////

// @gearonixx @AI_GENERATED@
// Фабрика, вызывается из TBootstrap::DoRun на старте процесса. New<T> — YT-аналог
// std::make_shared, возвращает TIntrusivePtr: счётчик ссылок встроен в сам объект через
// базу TRefCounted, без отдельного control block (как у std::shared_ptr) — на одну
// аллокацию меньше, и из голого T* всегда можно безопасно сделать ещё один Ptr.
IAccessCheckerPtr CreateAccessChecker(TBootstrap* bootstrap)
{
    return New<TAccessChecker>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
