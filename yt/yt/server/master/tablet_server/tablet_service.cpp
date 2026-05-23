#include "private.h"
#include "tablet_manager.h"
#include "tablet_service.h"

#include <yt/yt/server/master/cell_master/config.h>
#include <yt/yt/server/master/cell_master/config_manager.h>
#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>

#include <yt/yt/server/master/cypress_server/cypress_manager.h>

#include <yt/yt/server/master/security_server/security_manager.h>
#include <yt/yt/server/master/security_server/access_log.h>

#include <yt/yt/ytlib/tablet_client/master_tablet_service.h>

#include <yt/yt/core/rpc/authentication_identity.h>

namespace NYT::NTabletServer {

using namespace NCellMaster;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NCypressServer;
using namespace NHiveServer;
using namespace NHydra;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NSecurityServer;
using namespace NTableClient;
using namespace NTableServer;
using namespace NTabletClient::NProto;
using namespace NTabletClient;
using namespace NTabletNode::NProto;
using namespace NTabletServer::NProto;
using namespace NTransactionServer;
using namespace NTransactionSupervisor;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;

using NTransactionServer::TTransaction;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

constinit const auto Logger = TabletServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TTabletService
    : public ITabletService
    , public TMasterAutomatonPart
{
public:
    explicit TTabletService(TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap, EAutomatonThreadQueue::TabletManager)
    {
        YT_ASSERT_INVOKER_THREAD_AFFINITY(Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(EAutomatonThreadQueue::Default), AutomatonThread);
    }

    void Initialize() override
    {
        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->RegisterTransactionActionHandlers<TReqMount>({
            .Prepare = BIND_NO_PROPAGATE(&TTabletService::HydraPrepareMount, Unretained(this)),
            .Commit = BIND_NO_PROPAGATE(&TTabletService::HydraCommitMount, Unretained(this)),
            .Abort = BIND_NO_PROPAGATE(&TTabletService::HydraAbortMount, Unretained(this)),
        });

        transactionManager->RegisterTransactionActionHandlers<TReqUnmount>({
            .Prepare = BIND_NO_PROPAGATE(&TTabletService::HydraPrepareUnmount, Unretained(this)),
            .Commit = BIND_NO_PROPAGATE(&TTabletService::HydraCommitUnmount, Unretained(this)),
            .Abort = BIND_NO_PROPAGATE(&TTabletService::HydraAbortUnmount, Unretained(this)),
        });

        //  Да, по сути хеш-мапа TransactionId → TTransaction* (живёт в TransactionManager из transaction_server).
        transactionManager->RegisterTransactionActionHandlers<TReqFreeze>({
            .Prepare = BIND_NO_PROPAGATE(&TTabletService::HydraPrepareFreeze, Unretained(this)),
            .Commit = BIND_NO_PROPAGATE(&TTabletService::HydraCommitFreeze, Unretained(this)),
            .Abort = BIND_NO_PROPAGATE(&TTabletService::HydraAbortFreeze, Unretained(this)),
        });

        transactionManager->RegisterTransactionActionHandlers<TReqUnfreeze>({
            .Prepare = BIND_NO_PROPAGATE(&TTabletService::HydraPrepareUnfreeze, Unretained(this)),
            .Commit = BIND_NO_PROPAGATE(&TTabletService::HydraCommitUnfreeze, Unretained(this)),
            .Abort = BIND_NO_PROPAGATE(&TTabletService::HydraAbortUnfreeze, Unretained(this)),
        });

        transactionManager->RegisterTransactionActionHandlers<TReqRemount>({
            .Prepare = BIND_NO_PROPAGATE(&TTabletService::HydraPrepareRemount, Unretained(this)),
            .Commit = BIND_NO_PROPAGATE(&TTabletService::HydraCommitRemount, Unretained(this)),
            .Abort = BIND_NO_PROPAGATE(&TTabletService::HydraAbortRemount, Unretained(this)),
        });

        transactionManager->RegisterTransactionActionHandlers<TReqReshard>({
            .Prepare = BIND_NO_PROPAGATE(&TTabletService::HydraPrepareReshard, Unretained(this)),
            .Commit = BIND_NO_PROPAGATE(&TTabletService::HydraCommitReshard, Unretained(this)),
            .Abort = BIND_NO_PROPAGATE(&TTabletService::HydraAbortReshard, Unretained(this)),
        });
    }

private:
    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    static void ValidateNoParentTransaction(TTransaction* transaction)
    {
        // Это запрет на вложенные транзакции: GetParent() непустой только у дочерней транзакции. Многие табличные операции (mount/unmount, alter, freeze и т.п.) меняют глобальное состояние таблетов через 2PC между
        // мастером и tablet node — если бы их разрешили внутри пользовательской транзакции, пришлось бы откатывать tablet-side эффекты при её abort'е, а инфраструктуры для этого нет. Поэтому такие операции делают
        // только в top-level транзакции (которую мастер сам коммитит атомарно).
        if (transaction->GetParent()) {
            THROW_ERROR_EXCEPTION("Operation cannot be performed in transaction");
        }
    }

    static TTabletOwnerBase* AsTabletOwnerSafe(TCypressNode* node)
    {
        if (!node) {
            return nullptr;
        }
        if (!IsTabletOwnerType(node->GetType())) {
            THROW_ERROR_EXCEPTION("%v is not a tablet owner", node->GetId());
        }
        return node->As<TTabletOwnerBase>();
    }


    void ValidateUsePermissionOnCellBundle(TTabletOwnerBase* table)
    {
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        const auto& cellBundle = table->TabletCellBundle();
        // i have seen that somethere before
        securityManager->ValidatePermission(cellBundle.Get(), EPermission::Use);
    }


    void HydraPrepareMount(
        TTransaction* transaction,
        NTabletClient::NProto::TReqMount* request,
        const TTransactionPrepareOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto hintCellId = FromProto<TTabletCellId>(request->cell_id());
        bool freeze = request->freeze();
        auto mountTimestamp = static_cast<TTimestamp>(request->mount_timestamp());
        auto tableId = FromProto<TTableId>(request->table_id());
        const auto& path = request->path();
        auto targetCellIds = FromProto<std::vector<TTabletCellId>>(request->target_cell_ids());

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager);

        //  Tablet owner — любой объект мастера, который владеет таблетами (шардами). Базовый класс TTabletOwnerBase, о
        YT_LOG_DEBUG("Preparing table mount (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v, CellId: %v, TargetCellIds: %v, Freeze: %v, MountTimestamp: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex,
            hintCellId,
            targetCellIds,
            freeze,
            mountTimestamp);

        ValidateNoParentTransaction(transaction);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        //  Tablet owner — любой объект мастера, который владеет таблетами (шардами). Базовый класс TTabletOwnerBase, о
        auto* table = AsTabletOwnerSafe(cypressManager->GetNodeOrThrow(TVersionedNodeId(tableId)));

        table->ValidateNoCurrentMountTransaction(Format("Cannot mount %v", table->GetLowercaseObjectName()));

        if (table->IsNative()) {
            auto currentPath = cypressManager->GetNodePath(table, nullptr);
            if (path != currentPath) {
                THROW_ERROR_EXCEPTION("%v path mismatch", table->GetCapitalizedObjectName())
                    << TErrorAttribute("requested_path", path)
                    << TErrorAttribute("resolved_path", currentPath);
            }

            ValidateUsePermissionOnCellBundle(table);

            cypressManager->LockNode(table, transaction, ELockMode::Exclusive, false, true);
        }

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->PrepareMount(
            table,
            firstTabletIndex,
            lastTabletIndex,
            hintCellId,
            targetCellIds,
            freeze);

        // CurrentMountTransactionId is used to prevent primary master to copy/move node when
        // secondary master has already committed mount (this causes an unexpected error in CloneTable).
        // Primary master is lazy coordinator of 2pc, thus clone command and participant commit command are
        // serialized. Moreover secondary master (participant) commit happens strictly before primary commit.
        // CurrentMountTransactionId mechanism ensures that clone command can be sent only before
        // primary master has been started participating in 2pc. Thus clone command cannot appear
        // on the secondary master after commit. It can however arrive between prepare and commit
        // so we don't call this validation on secondary master. Note that this deals with
        // clone command 'before' mount. Refer to UpdateTabletState to see how we deal with it 'after' mount.
        //
        // We also lock node on secondary master to prevent resharding tablet actions to change table structure
        // during two phase mount.
        table->LockCurrentMountTransaction(transaction->GetId());

        YT_LOG_ACCESS(tableId, request->path(), transaction, "PrepareMount");
    }

    void HydraCommitMount(
        TTransaction* transaction,
        NTabletClient::NProto::TReqMount* request,
        const TTransactionCommitOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto hintCellId = FromProto<TTabletCellId>(request->cell_id());
        bool freeze = request->freeze();
        auto mountTimestamp = static_cast<TTimestamp>(request->mount_timestamp());
        auto tableId = FromProto<TTableId>(request->table_id());
        const auto& path = request->path();
        auto targetCellIds = FromProto<std::vector<TTabletCellId>>(request->target_cell_ids());

        YT_LOG_DEBUG("Committing table mount (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v, CellId: %v, TargetCellIds: %v, Freeze: %v, MountTimestamp: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex,
            hintCellId,
            targetCellIds,
            freeze,
            mountTimestamp);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());

        table->SetLastMountTransactionId(transaction->GetId());
        table->UpdateExpectedTabletState(freeze ? ETabletState::Frozen : ETabletState::Mounted);

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->Mount(
            table,
            path,
            firstTabletIndex,
            lastTabletIndex,
            hintCellId,
            targetCellIds,
            freeze,
            mountTimestamp);

        YT_LOG_ACCESS(tableId, request->path(), transaction, "CommitMount");
    }

    void HydraAbortMount(
        TTransaction* transaction,
        NTabletClient::NProto::TReqMount* request,
        const TTransactionAbortOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto hintCellId = FromProto<TTabletCellId>(request->cell_id());
        bool freeze = request->freeze();
        auto mountTimestamp = static_cast<TTimestamp>(request->mount_timestamp());
        auto tableId = FromProto<TTableId>(request->table_id());
        auto targetCellIds = FromProto<std::vector<TTabletCellId>>(request->target_cell_ids());

        YT_LOG_DEBUG("Aborting table mount (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v, CellId: %v, TargetCellIds: %v, Freeze: %v, MountTimestamp: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex,
            hintCellId,
            targetCellIds,
            freeze,
            mountTimestamp);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());

        YT_LOG_ACCESS(tableId, request->path(), transaction, "AbortMount");
    }

    void HydraPrepareUnmount(
        TTransaction* transaction,
        NTabletClient::NProto::TReqUnmount* request,
        const TTransactionPrepareOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        bool force = request->force();
        auto tableId = FromProto<TTableId>(request->table_id());

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager);

        YT_LOG_DEBUG("Preparing table unmount (TableId: %v, TransactionId: %v, %v, "
            "Force: %v, FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            force,
            firstTabletIndex,
            lastTabletIndex);

        ValidateNoParentTransaction(transaction);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->GetNodeOrThrow(TVersionedNodeId(tableId)));

        ValidateUsePermissionOnCellBundle(table);

        if (force) {
            const auto& cellBundle = table->TabletCellBundle();
            securityManager->ValidatePermission(cellBundle.Get(), EPermission::Administer);
        }

        table->ValidateNoCurrentMountTransaction(Format("Cannot unmount %v", table->GetLowercaseObjectName()));

        if (table->IsNative()) {
            cypressManager->LockNode(table, transaction, ELockMode::Exclusive, false, true);
        }

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->PrepareUnmount(
            table,
            force,
            firstTabletIndex,
            lastTabletIndex);

        table->LockCurrentMountTransaction(transaction->GetId());

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "PrepareUnmount");
    }

    void HydraCommitUnmount(
        TTransaction* transaction,
        NTabletClient::NProto::TReqUnmount* request,
        const TTransactionCommitOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        bool force = request->force();
        auto tableId = FromProto<TTableId>(request->table_id());

        YT_LOG_DEBUG("Committing table unmount (TableId: %v, TransactionId: %v, %v, "
            "Force: %v, FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            force,
            firstTabletIndex,
            lastTabletIndex);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());

        table->SetLastMountTransactionId(transaction->GetId());

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->Unmount(
            table,
            force,
            firstTabletIndex,
            lastTabletIndex);

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "CommitUnmount");
    }

    void HydraAbortUnmount(
        TTransaction* transaction,
        NTabletClient::NProto::TReqUnmount* request,
        const TTransactionAbortOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        bool force = request->force();
        auto tableId = FromProto<TTableId>(request->table_id());

        YT_LOG_DEBUG("Aborting table unmount (TableId: %v, TransactionId: %v, %v, "
            "Force: %v, FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            force,
            firstTabletIndex,
            lastTabletIndex);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "AbortUnmount");
    }

 //    «Cell» здесь = группа из нескольких реплик мастера, работающих
 // через Hydra

    void HydraPrepareFreeze(
        TTransaction* transaction,
        NTabletClient::NProto::TReqFreeze* request,
        const TTransactionPrepareOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto tableId = FromProto<TTableId>(request->table_id());

        YT_LOG_DEBUG("@@gearonixx_master HydraPrepareFreeze entered "
            "(TableId: %v, TransactionId: %v, FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            firstTabletIndex,
            lastTabletIndex);

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager);

        YT_LOG_DEBUG("Preparing table freeze (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            //   - User: root
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex);

        ValidateNoParentTransaction(transaction);
        YT_LOG_DEBUG("@@gearonixx_master no-parent-transaction check passed (TransactionId: %v)",
            transaction->GetId());

        // CypressManager — подсистема мастера, владеющая Cypress-деревом
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        // каст к TTabletOwnerBase* (базовый класс для динтаблиц/hunk storage/replicated tables) и кидает исключение, если нода не tablet-owner.
        auto* table = AsTabletOwnerSafe(cypressManager->GetNodeOrThrow(TVersionedNodeId(tableId)));
        YT_LOG_DEBUG("@@gearonixx_master resolved tablet-owner node (TableId: %v, IsNative: %v)",
            tableId,
            // true
            // IsNative() — таблица «родная» этому кластеру (а не реплика чужой через cross-cluster механизмы); важно, потому что многие операции имеет
            table->IsNative());

        ValidateUsePermissionOnCellBundle(table);
        // Это список других мастер-cell'ов, с которыми надо синхронизировать Hydra-state перед prepare-фазой транзакции/мутации.
        YT_LOG_DEBUG("@@gearonixx_master bundle Use-permission validated (TableId: %v)",
            tableId);

        // Проверка: у этой таблицы сейчас не идёт другая mount/unmount/freeze/unfreeze операция. Такие операции на мастере не мгновенные
        // — они стартуют отдельную системную «mount transaction», шлют запросы в tablet
        // cell'ы, ждут ответов. Пока эта транзакция не закоммитилась, на таблице висит её id.
        table->ValidateNoCurrentMountTransaction(Format("Cannot freeze %v", table->GetLowercaseObjectName()));
        YT_LOG_DEBUG("@@gearonixx_master no-current-mount-transaction check passed (TableId: %v)",
            tableId);

        if (table->IsNative()) {
            //  Лок в Cypress — это запись «эта транзакция держит такое-то право на эту ноду». Пока лок жив, мастер не даст другим транзакциям делать конфликтующие изменения этой же ноды.
            //  Лок = «застолбить» ноду в транзакции. Пока транзакция жива, другие не могут трогать эту ноду (или конкретный её кусок — зависит от режима: Exclusive = вообще никто, Shared = можно писать в разные места,
            // Snapshot = просто читать зафиксированное). Закончилась транзакция — лок снят.
            cypressManager->LockNode(table, transaction, ELockMode::Exclusive, false, true);
            YT_LOG_DEBUG("@@gearonixx_master Cypress node locked Exclusive (TableId: %v, TransactionId: %v)",
                tableId,
                transaction->GetId());
        } else {
            YT_LOG_DEBUG("@@gearonixx_master skipped LockNode: external cell (TableId: %v)",
                tableId);
        }

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        // Это список других мастер-cell'ов, с которыми надо синхронизировать Hydra-state перед prepare-фазой транзакции/мутации.
        // mount / remount table manager + tablet cell bundles
        tabletManager->PrepareFreeze(
            table,
            firstTabletIndex,
            lastTabletIndex);
        YT_LOG_DEBUG("@@gearonixx_master tabletManager->PrepareFreeze done (TableId: %v)",
            tableId);

        //  Лок в Cypress — это запись «эта транзакция держит такое-то право на эту ноду». Пока лок жив, мастер не даст другим транзакциям делать конфликтующие изменения этой же ноды.
        table->LockCurrentMountTransaction(transaction->GetId());
        YT_LOG_DEBUG("@@gearonixx_master LockCurrentMountTransaction done (TableId: %v, TransactionId: %v)",
            tableId,
            transaction->GetId());

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "PrepareFreeze");

        YT_LOG_DEBUG("@@gearonixx_master HydraPrepareFreeze exiting OK (TableId: %v, TransactionId: %v)",
            tableId,
            transaction->GetId());
    }

    void HydraCommitFreeze(
        TTransaction* transaction,
        NTabletClient::NProto::TReqFreeze* request,
        const TTransactionCommitOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto tableId = FromProto<TTableId>(request->table_id());

        YT_LOG_DEBUG("@@gearonixx_master HydraCommitFreeze entered "
            "(TableId: %v, TransactionId: %v, FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            firstTabletIndex,
            lastTabletIndex);

        YT_LOG_DEBUG("Committing table freeze (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            YT_LOG_DEBUG("@@gearonixx_master HydraCommitFreeze: table not alive, returning (TableId: %v)",
                tableId);
            return;
        }

        // Тут после коммита транзакции монтирования таблицу «отвязывают» от неё: UnlockCurrentMountTransaction снимает блокировку на таблице, а SetLastMountTransactionId запоминает id этой транзакции, чтобы потом
        // отличать «свежие» mount/unmount-операции от устаревших (например, прилетевших с опозданием от tablet cell). Логи нужны, чтобы видеть порядок этих шагов в master при отладке.


        //  Тут после коммита транзакции монтирования таблицу «отвязывают» от неё: UnlockCurrentMountTransaction снимает блокировку на таблице, а SetLastMountTransactionId запоминает id этой транзакции, чтобы потом
        // отличать «свежие» mount/unmount-операции от устаревших (например, прилетевших с опозданием от tablet cell). Логи нужны, чтобы видеть порядок этих шагов в master при отладке.

        // асимметрия: Lock строгий (ассерт, что блокировки ещё нет), а Unlock мягкий (молча ничего не делает, если id не совпадает). Unlock может прилететь от уже «протухшей»
        // транзакции (например, после повторного mount другой транзакцией), и такой запоздалый вызов не должен сбрасывать актуальную блокировку.
        table->UnlockCurrentMountTransaction(transaction->GetId());
        YT_LOG_DEBUG("@@gearonixx_master UnlockCurrentMountTransaction done (TableId: %v)",
            tableId);

        table->SetLastMountTransactionId(transaction->GetId());
        YT_LOG_DEBUG("@@gearonixx_master SetLastMountTransactionId done (TableId: %v, TransactionId: %v)",
            tableId,
            transaction->GetId());

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->Freeze(
            table,
            firstTabletIndex,
            lastTabletIndex);
        YT_LOG_DEBUG("@@gearonixx_master tabletManager->Freeze done — Hive messages to tablet cells dispatched (TableId: %v)",
            tableId);

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "CommitFreeze");

        YT_LOG_DEBUG("@@gearonixx_master HydraCommitFreeze exiting OK (TableId: %v, TransactionId: %v)",
            tableId,
            transaction->GetId());
    }

    // Abort случается, когда транзакция freeze не доехала до commit'а: либо prepare упал, либо клиент сам её отменил,
    // либо она протухла по таймауту. Тогда мастер откатывает то, что зарезервировал PrepareFreeze.
    void HydraAbortFreeze(
        TTransaction* transaction,
        NTabletClient::NProto::TReqFreeze* request,
        const TTransactionAbortOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto tableId = FromProto<TTableId>(request->table_id());

        YT_LOG_DEBUG("@@gearonixx_master HydraAbortFreeze entered "
            "(TableId: %v, TransactionId: %v, FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            firstTabletIndex,
            lastTabletIndex);

        YT_LOG_DEBUG("Aborting table freeze (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));


        // @gearonixx
        // Persistent state мастера можно трогать только на automaton thread. `FlushObjectUnrefs` сбрасывает накопленные отложенные unref'ы объектов именно на нём,
        // где разрушать объекты безопасно. Две `Verify*` просто проверяют, что мы в правильном потоке — жёстко или с поблажкой для snapshot fork.

        if (!IsObjectAlive(table)) {
            YT_LOG_DEBUG("@@gearonixx_master HydraAbortFreeze: table not alive, returning (TableId: %v)",
                tableId);
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());
        YT_LOG_DEBUG("@@gearonixx_master UnlockCurrentMountTransaction done on abort (TableId: %v)",
            tableId);

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "AbortFreeze");

        YT_LOG_DEBUG("@@gearonixx_master HydraAbortFreeze exiting (TableId: %v, TransactionId: %v)",
            tableId,
            transaction->GetId());
    }

    void HydraPrepareUnfreeze(
        TTransaction* transaction,
        NTabletClient::NProto::TReqUnfreeze* request,
        const TTransactionPrepareOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto tableId = FromProto<TTableId>(request->table_id());

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager);

        YT_LOG_DEBUG("Preparing table unfreeze (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex);

        ValidateNoParentTransaction(transaction);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->GetNodeOrThrow(TVersionedNodeId(tableId)));

        ValidateUsePermissionOnCellBundle(table);

        table->ValidateNoCurrentMountTransaction(Format("Cannot unfreeze %v", table->GetLowercaseObjectName()));

        if (table->IsNative()) {
            cypressManager->LockNode(table, transaction, ELockMode::Exclusive, false, true);
        }

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->PrepareUnfreeze(
            table,
            firstTabletIndex,
            lastTabletIndex);

        table->LockCurrentMountTransaction(transaction->GetId());

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "PrepareUnfreeze");
    }

    void HydraCommitUnfreeze(
        TTransaction* transaction,
        NTabletClient::NProto::TReqUnfreeze* request,
        const TTransactionCommitOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto tableId = FromProto<TTableId>(request->table_id());

        YT_LOG_DEBUG("Committing table unfreeze (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());

        table->SetLastMountTransactionId(transaction->GetId());
        table->UpdateExpectedTabletState(ETabletState::Mounted);

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->Unfreeze(
            table,
            firstTabletIndex,
            lastTabletIndex);

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "CommitUnfreeze");
    }

    void HydraAbortUnfreeze(
        TTransaction* transaction,
        NTabletClient::NProto::TReqUnfreeze* request,
        const TTransactionAbortOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto tableId = FromProto<TTableId>(request->table_id());

        YT_LOG_DEBUG("Aborting table unfreeze (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "AbortUnfreeze");
    }

    void HydraPrepareRemount(
        TTransaction* transaction,
        NTabletClient::NProto::TReqRemount* request,
        const TTransactionPrepareOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto tableId = FromProto<TTableId>(request->table_id());

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager);

        YT_LOG_DEBUG("Preparing table remount (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex);

        ValidateNoParentTransaction(transaction);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->GetNodeOrThrow(TVersionedNodeId(tableId)));

        ValidateUsePermissionOnCellBundle(table);

        table->ValidateNoCurrentMountTransaction(Format("Cannot remount %v", table->GetLowercaseObjectName()));

        if (table->IsNative()) {
            cypressManager->LockNode(table, transaction, ELockMode::Exclusive, false, true);
        }

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->PrepareRemount(
            table,
            firstTabletIndex,
            lastTabletIndex);

        table->LockCurrentMountTransaction(transaction->GetId());

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "PrepareRemount");
    }

    void HydraCommitRemount(
        TTransaction* transaction,
        NTabletClient::NProto::TReqRemount* request,
        const TTransactionCommitOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto tableId = FromProto<TTableId>(request->table_id());

        YT_LOG_DEBUG("Committing table remount (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->Remount(
            table,
            firstTabletIndex,
            lastTabletIndex);

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "CommitRemount");
    }

    void HydraAbortRemount(
        TTransaction* transaction,
        NTabletClient::NProto::TReqRemount* request,
        const TTransactionAbortOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        auto tableId = FromProto<TTableId>(request->table_id());

        YT_LOG_DEBUG("Aborting table remount (TableId: %v, TransactionId: %v, %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            firstTabletIndex,
            lastTabletIndex);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "AbortRemount");
    }

    void HydraPrepareReshard(
        TTransaction* transaction,
        NTabletClient::NProto::TReqReshard* request,
        const TTransactionPrepareOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        int tabletCount = request->tablet_count();
        auto pivotKeys = FromProto<std::vector<TLegacyOwningKey>>(request->pivot_keys());
        auto tableId = FromProto<TTableId>(request->table_id());
        auto trimmedRowCounts = FromProto<std::vector<i64>>(request->trimmed_row_counts());

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager);

        YT_LOG_DEBUG("Preparing table reshard (TableId: %v, TransactionId: %v, %v, "
            "TabletCount: %v, PivotKeysSize: %v, TrimmedRowCountsSize: %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            tabletCount,
            pivotKeys.size(),
            trimmedRowCounts.size(),
            firstTabletIndex,
            lastTabletIndex);

        ValidateNoParentTransaction(transaction);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->GetNodeOrThrow(TVersionedNodeId(tableId)));

        ValidateUsePermissionOnCellBundle(table);

        table->ValidateNoCurrentMountTransaction(Format("Cannot reshard %v", table->GetLowercaseObjectName()));

        if (table->IsNative()) {
            cypressManager->LockNode(table, transaction, ELockMode::Exclusive, false, true);
        }

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->PrepareReshard(
            table,
            firstTabletIndex,
            lastTabletIndex,
            tabletCount,
            pivotKeys,
            trimmedRowCounts);

        table->LockCurrentMountTransaction(transaction->GetId());

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "PrepareReshard");
    }

    void HydraCommitReshard(
        TTransaction* transaction,
        NTabletClient::NProto::TReqReshard* request,
        const TTransactionCommitOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        int tabletCount = request->tablet_count();
        auto pivotKeys = FromProto<std::vector<TLegacyOwningKey>>(request->pivot_keys());
        auto tableId = FromProto<TTableId>(request->table_id());
        auto trimmedRowCounts = FromProto<std::vector<i64>>(request->trimmed_row_counts());

        YT_LOG_DEBUG("Committing table reshard (TableId: %v, TransactionId: %v, %v, "
            "TabletCount: %v, PivotKeysSize: %v, TrimmedRowCountsSize: %v, "
            "FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            tabletCount,
            pivotKeys.size(),
            trimmedRowCounts.size(),
            firstTabletIndex,
            lastTabletIndex);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());

        table->SetLastMountTransactionId(transaction->GetId());

        const auto& tabletManager = Bootstrap_->GetTabletManager();
        tabletManager->Reshard(
            table,
            firstTabletIndex,
            lastTabletIndex,
            tabletCount,
            pivotKeys,
            trimmedRowCounts);

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "CommitReshard");
    }

    void HydraAbortReshard(
        TTransaction* transaction,
        NTabletClient::NProto::TReqReshard* request,
        const TTransactionAbortOptions& /*options*/)
    {
        int firstTabletIndex = request->first_tablet_index();
        int lastTabletIndex = request->last_tablet_index();
        int tabletCount = request->tablet_count();
        auto pivotKeys = FromProto<std::vector<TLegacyOwningKey>>(request->pivot_keys());
        auto tableId = FromProto<TTableId>(request->table_id());

        YT_LOG_DEBUG("Aborting table reshard (TableId: %v, TransactionId: %v, %v, "
            "TabletCount: %v, PivotKeysSize: %v, FirstTabletIndex: %v, LastTabletIndex: %v)",
            tableId,
            transaction->GetId(),
            NRpc::GetCurrentAuthenticationIdentity(),
            tabletCount,
            pivotKeys.size(),
            firstTabletIndex,
            lastTabletIndex);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* table = AsTabletOwnerSafe(cypressManager->FindNode(TVersionedNodeId(tableId)));

        if (!IsObjectAlive(table)) {
            return;
        }

        table->UnlockCurrentMountTransaction(transaction->GetId());

        YT_LOG_ACCESS(tableId, cypressManager->GetNodePath(table, nullptr), transaction, "AbortReshard");
    }
};

////////////////////////////////////////////////////////////////////////////////

ITabletServicePtr CreateTabletService(TBootstrap* bootstrap)
{
    return New<TTabletService>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
