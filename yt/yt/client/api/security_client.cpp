#include "security_client.h"

#include <yt/yt/core/logging/log.h>

namespace NYT::NApi {

using namespace NYTree;

static const NLogging::TLogger Logger("SecurityClient");

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TGetCurrentUserResult& result, NYson::IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("user").Value(result.User)
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

TError TCheckPermissionResult::ToError(
    const std::string& user,
    EPermission permission,
    const std::optional<std::string>& column) const
{
    YT_LOG_DEBUG("@@gearonixx_driver TCheckPermissionResult::ToError entered "
        "(User: %v, Permission: %v, Column: %v, Action: %v, ObjectId: %v, ObjectName: %v, "
        "SubjectId: %v, SubjectName: %v)",
        user,
        permission,
        column,
        Action,
        ObjectId,
        ObjectName,
        SubjectId,
        SubjectName);

    switch (Action) {
        case NSecurityClient::ESecurityAction::Allow:
            YT_LOG_DEBUG("@@gearonixx_driver Action == Allow -> returning empty TError (OK)");
            return TError();

        case NSecurityClient::ESecurityAction::Deny: {
            YT_LOG_DEBUG("@@gearonixx_driver Action == Deny -> building AuthorizationError");
            TError error;
            if (ObjectName && SubjectName) {
                YT_LOG_DEBUG("@@gearonixx_driver Deny: both ObjectName and SubjectName present "
                    "-> detailed message (Subject: %v, Object: %v)",
                    *SubjectName,
                    *ObjectName);
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied for user %Qv: %Qlv permission is denied for %Qv by ACE at %v",
                    user,
                    permission,
                    *SubjectName,
                    *ObjectName);
            } else {
                YT_LOG_DEBUG("@@gearonixx_driver Deny: ObjectName/SubjectName missing "
                    "-> generic 'no matching ACE' message");
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied for user %Qv: %Qlv permission is not allowed by any matching ACE",
                    user,
                    permission);
            }
            error <<= TErrorAttribute("user", user);
            error <<= TErrorAttribute("permission", permission);
            YT_LOG_DEBUG("@@gearonixx_driver attached base attributes (user, permission)");
            if (ObjectId) {
                error <<= TErrorAttribute("denied_by", ObjectId);
                YT_LOG_DEBUG("@@gearonixx_driver attached denied_by (ObjectId: %v)", ObjectId);
            }
            if (SubjectId) {
                error <<= TErrorAttribute("denied_for", SubjectId);
                YT_LOG_DEBUG("@@gearonixx_driver attached denied_for (SubjectId: %v)", SubjectId);
            }
            if (column) {
                error <<= TErrorAttribute("column", *column);
                YT_LOG_DEBUG("@@gearonixx_driver attached column (Column: %v)", *column);
            }
            YT_LOG_DEBUG("@@gearonixx_driver ToError returning Deny error");
            return error;
        }

        default:
            YT_LOG_DEBUG("@@gearonixx_driver unexpected Action value -> YT_ABORT");
            YT_ABORT();
    }
}

TError TCheckPermissionByAclResult::ToError(const std::string& user, EPermission permission) const
{
    switch (Action) {
        case NSecurityClient::ESecurityAction::Allow:
            return TError();

        case NSecurityClient::ESecurityAction::Deny: {
            TError error;
            if (SubjectName) {
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied for user %Qv: %Qlv permission is denied for %Qv by ACL",
                    user,
                    permission,
                    *SubjectName);
            } else {
                error = TError(
                    NSecurityClient::EErrorCode::AuthorizationError,
                    "Access denied for user %Qv: %Qlv permission is not allowed by any matching ACE",
                    user,
                    permission);
            }
            error <<= TErrorAttribute("user", user);
            error <<= TErrorAttribute("permission", permission);
            if (SubjectId) {
                error <<= TErrorAttribute("denied_for", SubjectId);
            }
            return error;
        }

        default:
            YT_ABORT();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi
