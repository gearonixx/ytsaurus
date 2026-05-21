#include "client_impl.h"

#include "ypath_helpers.h"

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/security_client/helpers.h>
#include <yt/yt/client/security_client/acl.h>

#include <yt/yt/ytlib/cypress_client/rpc_helpers.h>
#include <yt/yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/ytlib/security_client/account_ypath_proxy.h>
#include <yt/yt/ytlib/security_client/group_ypath_proxy.h>
#include <yt/yt/ytlib/security_client/acl.h>

#include <yt/yt/ytlib/scheduler/helpers.h>

#include <yt/yt/ytlib/scheduler/proto/resources.pb.h>

#include <yt/yt/ytlib/transaction_client/helpers.h>

#include <yt/yt/core/ypath/tokenizer.h>

#include <yt/yt/core/yson/protobuf_helpers.h>

namespace NYT::NApi::NNative {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NTabletClient;
using namespace NSecurityClient;
using namespace NTransactionClient;

using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

TGetCurrentUserResult TClient::DoGetCurrentUser(const TGetCurrentUserOptions& /*options*/)
{
    TGetCurrentUserResult result;
    result.User = Options_.GetAuthenticatedUser();
    return result;
}

TCheckPermissionByAclResult TClient::DoCheckPermissionByAcl(
    const std::optional<std::string>& user,
    EPermission permission,
    INodePtr acl,
    const TCheckPermissionByAclOptions& options)
{
    auto proxy = CreateObjectServiceReadProxy(options);
    auto batchReq = proxy.ExecuteBatch();
    SetBalancingHeader(batchReq, options);
    batchReq->SetSuppressTransactionCoordinatorSync(true);

    auto req = TMasterYPathProxy::CheckPermissionByAcl();
    if (user) {
        req->set_user(ToProto(*user));
    }
    req->set_permission(ToProto(permission));
    req->set_acl(ToProto(ConvertToYsonString(acl)));
    req->set_ignore_missing_subjects(options.IgnoreMissingSubjects);
    req->set_ignore_pending_removal_subjects(options.IgnorePendingRemovalSubjects);
    SetCachingHeader(req, options);

    batchReq->AddRequest(req);

    auto batchRsp = WaitFor(batchReq->Invoke())
        .ValueOrThrow();
    auto rsp = batchRsp->GetResponse<TMasterYPathProxy::TRspCheckPermissionByAcl>(0)
        .ValueOrThrow();

    TCheckPermissionByAclResult result;
    result.Action = CheckedEnumCast<ESecurityAction>(rsp->action());
    result.SubjectId = FromProto<TSubjectId>(rsp->subject_id());
    result.SubjectName = rsp->has_subject_name() ? std::make_optional(rsp->subject_name()) : std::nullopt;
    result.MissingSubjects = FromProto<std::vector<std::string>>(rsp->missing_subjects());
    result.PendingRemovalSubjects = FromProto<std::vector<std::string>>(rsp->pending_removal_subjects());
    return result;
}

void TClient::DoAddMember(
    const std::string& group,
    const std::string& member,
    const TAddMemberOptions& options)
{
    auto proxy = CreateObjectServiceWriteProxy();
    auto batchReq = proxy.ExecuteBatch();
    SetPrerequisites(batchReq, options);

    auto req = TGroupYPathProxy::AddMember(GetGroupPath(group));
    req->set_name(member);
    SetMutationId(req, options);

    batchReq->AddRequest(req);

    auto batchRsp = WaitFor(batchReq->Invoke())
        .ValueOrThrow();
    batchRsp->GetResponse<TGroupYPathProxy::TRspAddMember>(0)
        .ThrowOnError();
}

void TClient::DoRemoveMember(
    const std::string& group,
    const std::string& member,
    const TRemoveMemberOptions& options)
{
    auto proxy = CreateObjectServiceWriteProxy();
    auto batchReq = proxy.ExecuteBatch();
    SetPrerequisites(batchReq, options);

    auto req = TGroupYPathProxy::RemoveMember(GetGroupPath(group));
    req->set_name(member);
    SetMutationId(req, options);

    batchReq->AddRequest(req);

    auto batchRsp = WaitFor(batchReq->Invoke())
        .ValueOrThrow();
    batchRsp->GetResponse<TGroupYPathProxy::TRspRemoveMember>(0)
        .ThrowOnError();
}

TCheckPermissionResponse TClient::DoCheckPermission(
    const std::string& user,
    const TYPath& path,
    EPermission permission,
    const TCheckPermissionOptions& options)
{
    YT_LOG_DEBUG("@@gearonixx_driver DoCheckPermission entered (User: %v, Path: %v, Permission: %v, "
        "HasColumns: %v, HasVital: %v, SuppressTxCoordSync: %v)",
        user,
        path,
        permission,
        options.Columns.has_value(),
        options.Vital.has_value(),
        options.SuppressTransactionCoordinatorSync);

    // Да, это синхронный RPC к мастеру. proxy.ExecuteBatch() собирает batch, batchReq->Invoke() уходит по сети в ObjectService мастера (
    // Создаётся объект-обёртка над RPC-каналом до мастера.
    auto proxy = CreateObjectServiceReadProxy(options);
    YT_LOG_DEBUG("@@gearonixx_driver ObjectServiceReadProxy created");

    // создай мне пустой batch к этому ObjectService»
    // Несколько запросов, упакованных в один сетевой вызов.
    auto batchReq = proxy.ExecuteBatch();
    // Первое — флаг «не синхронизироваться с координатором транзакции перед выполнением
    batchReq->SetSuppressTransactionCoordinatorSync(options.SuppressTransactionCoordinatorSync);
    SetBalancingHeader(batchReq, options);
    YT_LOG_DEBUG("@@gearonixx_driver batch request prepared (balancing header set, suppress-tx-coord-sync applied)");
    // просто формат, в который ObjectService умеет принимать запросы; даже если запрос один, его всё равно нужно завернуть в batch, потому что другого API у сервиса не

    auto req = TObjectYPathProxy::CheckPermission(path);
    req->set_user(ToProto(user));
    req->set_permission(ToProto(permission));
    YT_LOG_DEBUG("@@gearonixx_driver CheckPermission proto built (User: %v, Path: %v, Permission: %v)",
        user, path, permission);

    if (options.Columns) {
        ToProto(req->mutable_columns()->mutable_items(), *options.Columns);
        YT_LOG_DEBUG("@@gearonixx_driver columns attached to request (Count: %v)", options.Columns->size());
    }
    if (options.Vital) {
        req->set_vital(*options.Vital);
        YT_LOG_DEBUG("@@gearonixx_driver vital flag attached (Vital: %v)", *options.Vital);
    }
    SetTransactionId(req, options, true);
    SetCachingHeader(req, options);
    NCypressClient::SetSuppressAccessTracking(req, true);
    NCypressClient::SetSuppressExpirationTimeoutRenewal(req, true);
    batchReq->AddRequest(req);
    YT_LOG_DEBUG("@@gearonixx_driver request added to batch -> invoking RPC to master");

    // вот здесь происходит запрос к мастеру
    auto batchRsp = WaitFor(batchReq->Invoke())
        .ValueOrThrow();
    YT_LOG_DEBUG("@@gearonixx_driver batch RPC returned from master");

    auto rsp = batchRsp->GetResponse<TObjectYPathProxy::TRspCheckPermission>(0)
        .ValueOrThrow();
    YT_LOG_DEBUG("@@gearonixx_driver extracted CheckPermission response from batch "
        "(Action: %v, HasObjectName: %v, HasSubjectName: %v, HasColumns: %v, HasRowLevelAcl: %v)",
        FromProto<ESecurityAction>(rsp->action()),
        rsp->has_object_name(),
        rsp->has_subject_name(),
        rsp->has_columns(),
        rsp->has_row_level_acl());

    auto fillResult = [&] (auto* result, const auto& protoResult) {
        result->Action = FromProto<ESecurityAction>(protoResult.action());
        result->ObjectId = FromProto<TObjectId>(protoResult.object_id());
        result->ObjectName = protoResult.has_object_name() ? std::make_optional(protoResult.object_name()) : std::nullopt;
        result->SubjectId = FromProto<TSubjectId>(protoResult.subject_id());
        result->SubjectName = protoResult.has_subject_name() ? std::make_optional(protoResult.subject_name()) : std::nullopt;
        YT_LOG_DEBUG("@@gearonixx_driver fillResult populated (Action: %v, ObjectId: %v, ObjectName: %v, "
            "SubjectId: %v, SubjectName: %v)",
            result->Action,
            result->ObjectId,
            result->ObjectName,
            result->SubjectId,
            result->SubjectName);
    };

    TCheckPermissionResponse response;
    YT_LOG_DEBUG("@@gearonixx_driver building top-level response");
    fillResult(&response, *rsp);

    if (rsp->has_columns()) {
        response.Columns.emplace();
        response.Columns->reserve(static_cast<size_t>(rsp->columns().items_size()));
        YT_LOG_DEBUG("@@gearonixx_driver filling per-column results (Count: %v)",
            rsp->columns().items_size());
        for (const auto& protoResult : rsp->columns().items()) {
            fillResult(&response.Columns->emplace_back(), protoResult);
        }
    }

    if (rsp->has_row_level_acl()) {
        response.RowLevelAcl = FromProto<std::vector<TRowLevelAccessControlEntry>>(rsp->row_level_acl().items());
        YT_LOG_DEBUG("@@gearonixx_driver row-level ACL parsed (EntryCount: %v)",
            response.RowLevelAcl->size());
    }

    YT_LOG_DEBUG("@@gearonixx_driver DoCheckPermission returning (Action: %v, ObjectId: %v)",
        response.Action,
        response.ObjectId);
    return response;
}

TCheckPermissionResult TClient::CheckPermissionImpl(
    const TYPath& path,
    EPermission permission,
    const TCheckPermissionOptions& options)
{
    // TODO(babenko): consider passing proper timeout
    const auto& user = Options_.GetAuthenticatedUser();
    YT_LOG_DEBUG("@@gearonixx_driver CheckPermissionImpl entered (User: %v, Path: %v, Permission: %v)",
        user, path, permission);
    auto result = DoCheckPermission(user, path, permission, options);
    YT_LOG_DEBUG("@@gearonixx_driver CheckPermissionImpl returning (Action: %v)", result.Action);
    return result;
}

//         ValidatePermissionImpl(path, EPermission::Mount);
void TClient::ValidatePermissionImpl(
    const TYPath& path,
    EPermission permission,
    const TCheckPermissionOptions& options)
{
    // TODO(babenko): consider passing proper timeout
    const auto& user = Options_.GetAuthenticatedUser();
    YT_LOG_DEBUG("@@gearonixx_driver ValidatePermissionImpl entered (User: %v, Path: %v, Permission: %v)",
        user, path, permission);
    auto result = DoCheckPermission(user, path, permission, options);
    YT_LOG_DEBUG("@@gearonixx_driver ValidatePermissionImpl got result (Action: %v) -> throwing on error if denied",
        result.Action);
    // Allow → пустой OK-TError, Deny → TError с кодом AuthorizationError. То есть имя метода вводит в заблуждение — он не «возвращает ошибку», а «представляет результат в виде TError».
    result
        .ToError(user, permission)
        .ThrowOnError();
    YT_LOG_DEBUG("@@gearonixx_driver ValidatePermissionImpl passed (User: %v, Path: %v, Permission: %v)",
        user, path, permission);
}

void TClient::MaybeValidateExternalObjectPermission(
    const TYPath& path,
    EPermission permission,
    const TCheckPermissionOptions& options)
{
    TObjectId objectId;
    if (!TryParseObjectId(path, &objectId)) {
        return;
    }

    switch (TypeFromId(objectId)) {
        case EObjectType::TableReplica:
            ValidateTableReplicaPermission(objectId, permission, options);
            break;

        default:
            break;
    }
}

TYPath TClient::GetReplicaTablePath(TTableReplicaId replicaId)
{
    auto cellTag = CellTagFromId(replicaId);
    auto proxy = CreateObjectServiceReadProxy({}, cellTag);
    auto batchReq = proxy.ExecuteBatch();

    auto req = TYPathProxy::Get(FromObjectId(replicaId) + "/@table_path");
    NCypressClient::SetSuppressAccessTracking(req, true);
    NCypressClient::SetSuppressExpirationTimeoutRenewal(req, true);
    batchReq->AddRequest(req);

    auto batchRsp = WaitFor(batchReq->Invoke())
        .ValueOrThrow();
    auto rsp = batchRsp->GetResponse<TYPathProxy::TRspGet>(0)
        .ValueOrThrow();

    return ConvertTo<TYPath>(TYsonString(rsp->value()));
}

void TClient::ValidateTableReplicaPermission(
    TTableReplicaId replicaId,
    EPermission permission,
    const TCheckPermissionOptions& options)
{
    // TODO(babenko): consider passing proper timeout
    auto tablePath = GetReplicaTablePath(replicaId);
    ValidatePermissionImpl(tablePath, permission, options);
}

void TClient::DoTransferAccountResources(
    const std::string& srcAccount,
    const std::string& dstAccount,
    NYTree::INodePtr resourceDelta,
    const TTransferAccountResourcesOptions& options)
{
    auto proxy = CreateObjectServiceWriteProxy();
    auto batchReq = proxy.ExecuteBatch();

    auto req = TAccountYPathProxy::TransferAccountResources(GetAccountPath(dstAccount));
    req->set_src_account(srcAccount);
    req->set_resource_delta(ToProto(ConvertToYsonString(resourceDelta)));
    SetMutationId(req, options);

    batchReq->AddRequest(req);

    auto batchRsp = WaitFor(batchReq->Invoke())
        .ValueOrThrow();
    batchRsp->GetResponse<TAccountYPathProxy::TRspTransferAccountResources>(0)
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
