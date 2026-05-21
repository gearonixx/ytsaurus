#pragma once

#include "client_common.h"

#include <yt/yt/client/security_client/acl.h>
#include <yt/yt/client/security_client/public.h>

namespace NYT::NApi {

////////////////////////////////////////////////////////////////////////////////

// @gearonixx: опции для добавления пользователя/группы в группу.
// Наследует таймаут, флаги мутирующей операции (для дедупликации ретраев
// мастером) и список prerequisite-транзакций, которые должны быть живы,
// иначе мутация будет отклонена.
struct TAddMemberOptions
    : public TTimeoutOptions
    , public TMutatingOptions
    , public TPrerequisiteOptions
{ };

// @gearonixx: опции для удаления члена из группы — симметрично TAddMemberOptions.
struct TRemoveMemberOptions
    : public TTimeoutOptions
    , public TMutatingOptions
    , public TPrerequisiteOptions
{ };

// @gearonixx: опции запроса проверки прав на объект Cypress.
// TMasterReadOptions задаёт режим чтения мастера (leader/follower/cache),
// TTransactionalOptions — транзакцию, в которой проверяем (важно для
// прав на объекты, видимых только из транзакции).
struct TCheckPermissionOptions
    : public TTimeoutOptions
    , public TMasterReadOptions
    , public TTransactionalOptions
    , public TPrerequisiteOptions
{
    // @gearonixx: ограничить проверку конкретными колонками таблицы
    // (column-level ACL). nullopt — проверять только табличный ACL.
    std::optional<std::vector<std::string>> Columns;
    // @gearonixx: если задано, фильтрует чанки по флагу vital
    // (используется для проверки прав на специфические данные).
    std::optional<bool> Vital;
};

// @gearonixx: результат проверки прав на один объект.
struct TCheckPermissionResult
{
    // @gearonixx: преобразует Deny-результат в TError с понятным текстом
    // (вида "Access denied for user X: permission Y is not allowed"),
    // либо возвращает OK, если Action == Allow.
    TError ToError(
        const std::string& user,
        NYTree::EPermission permission,
        const std::optional<std::string>& column = {}) const;

    // @gearonixx: вердикт — Allow или Deny.
    NSecurityClient::ESecurityAction Action;
    // @gearonixx: id ACE-объекта (узел Cypress), который определил вердикт.
    NObjectClient::TObjectId ObjectId;
    // @gearonixx: человекочитаемое имя этого объекта, если доступно.
    std::optional<TString> ObjectName;
    // @gearonixx: id субъекта (пользователь/группа), которому ACE разрешил/запретил доступ.
    NSecurityClient::TSubjectId SubjectId;
    // @gearonixx: имя этого субъекта.
    std::optional<std::string> SubjectName;
};

// @gearonixx: полный ответ CheckPermission: вердикт на сам объект плюс
// детализация по колонкам и row-level ACL, если они применимы.
struct TCheckPermissionResponse
    : public TCheckPermissionResult
{
    // @gearonixx: пер-колоночные вердикты, в порядке Options.Columns.
    std::optional<std::vector<TCheckPermissionResult>> Columns;
    // @gearonixx: row-level ACL (фильтрация прав по предикатам строк).
    std::optional<std::vector<NSecurityClient::TRowLevelAccessControlEntry>> RowLevelAcl;
};

// @gearonixx: опции проверки прав не по объекту, а по явно переданному ACL
// (полезно для UI/dry-run "что было бы, если бы я применил вот этот ACL").
struct TCheckPermissionByAclOptions
    : public TTimeoutOptions
    , public TMasterReadOptions
    , public TPrerequisiteOptions
{
    // @gearonixx: не падать, если в ACL упомянут несуществующий субъект —
    // просто вернуть его в MissingSubjects.
    bool IgnoreMissingSubjects = false;
    // @gearonixx: то же самое для субъектов, помеченных на удаление.
    bool IgnorePendingRemovalSubjects = false;
};

// @gearonixx: результат CheckPermissionByAcl — без ObjectId (объекта нет),
// зато со списками "битых" субъектов, обнаруженных в ACL.
struct TCheckPermissionByAclResult
{
    TError ToError(const std::string& user, NYTree::EPermission permission) const;

    NSecurityClient::ESecurityAction Action;
    NSecurityClient::TSubjectId SubjectId;
    std::optional<std::string> SubjectName;
    // @gearonixx: имена субъектов из ACL, которых не существует на кластере.
    std::vector<std::string> MissingSubjects;
    // @gearonixx: субъекты, помеченные на удаление (pending removal).
    std::vector<std::string> PendingRemovalSubjects;
};

// @gearonixx: опции установки пароля пользователя (нативная схема аутентификации YT,
// без внешнего IDP).
struct TSetUserPasswordOptions
    : public TTimeoutOptions
{
    // @gearonixx: если true, пароль одноразовый — при первом логине пользователю
    // будет предложено сменить его.
    bool PasswordIsTemporary = false;
};

// @gearonixx: опции выпуска постоянного токена доступа для пользователя.
struct TIssueTokenOptions
    : public TTimeoutOptions
{
    // @gearonixx: произвольное описание токена (для чего выпущен), хранится
    // в метаданных и видно в ListUserTokens.
    TString Description;
};

// @gearonixx: опции выпуска временного токена — добавляет TTL.
struct TIssueTemporaryTokenOptions
    : public TIssueTokenOptions
{
    // @gearonixx: через какое время токен автоматически протухнет.
    TDuration ExpirationTimeout;
};

// @gearonixx: то, что вернёт IssueToken — сам токен (только этот один раз!)
// и id ноды Cypress, в которой он живёт.
struct TIssueTokenResult
{
    std::string Token;
    //! Cypress node corresponding to issued token.
    //! Deleting this node will revoke the token.
    // @gearonixx: токены хранятся как ноды Cypress, поэтому "удалить ноду = отозвать токен".
    NCypressClient::TNodeId NodeId;
};

// @gearonixx: опции продления жизни временного токена (сдвиг expiration).
struct TRefreshTemporaryTokenOptions
    : public TTimeoutOptions
{ };

// @gearonixx: опции отзыва токена.
struct TRevokeTokenOptions
    : public TTimeoutOptions
{ };

// @gearonixx: опции запроса списка токенов пользователя.
struct TListUserTokensOptions
    : public TTimeoutOptions
{
    // @gearonixx: возвращать ли мета-инфу (описание, дата выпуска и т.п.) или
    // только сами хэши токенов.
    bool WithMetadata;
};

// @gearonixx
// Это публичный header клиентского API безопасности — определяет интерфейс ISecurityClient (что клиент умеет делать в области security)
// и все типы опций/результа…Это публичный header клиентского API безопасности — определяет интерфейс ISecurityClient (что клиент умеет делать в области security) и все типы опций/результатов для этих методов. Сам по себе он не реализует ничего — это контракт, который дальше имплементят native-клиент (прямо к мастерам) и RPC-клиент (через прокси).

// @gearonixx: результат ListUserTokens. Сами токены НЕ возвращаются — только
// их SHA256 (см. комментарий ниже), потому что plain-текст токена существует
// только в момент выпуска.
struct TListUserTokensResult
{
    // Tokens are SHA256-encoded.
    std::vector<TString> Tokens;
    // @gearonixx: метаданные по токенам, ключ — SHA256 токена.
    THashMap<TString, NYson::TYsonString> Metadata;
};

// @gearonixx: опции whoami-запроса.
struct TGetCurrentUserOptions
    : public TTimeoutOptions
{ };

// @gearonixx: результат whoami — имя пользователя, под которым клиент
// аутентифицирован на кластере.
struct TGetCurrentUserResult
{
    std::string User;
};

// @gearonixx: сериализатор результата в YSON (нужен, чтобы драйвер мог
// отдать ответ клиенту в текстовом виде).
void Serialize(const TGetCurrentUserResult& result, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

// @gearonixx: "секьюрити-срез" большого интерфейса IClient. Это часть API,
// относящаяся к управлению пользователями, группами, ACL и токенами.
// IClient наследуется от этого интерфейса (mixin-стиль), чтобы не пухнуть
// в одном файле.
struct ISecurityClient
{
    virtual ~ISecurityClient() = default;

    //! Return information about current user.
    // @gearonixx: whoami — кто я сейчас по мнению кластера.
    virtual TFuture<TGetCurrentUserResult> GetCurrentUser(
        const TGetCurrentUserOptions& options = {}) = 0;

    // @gearonixx: добавить пользователя или подгруппу `member` в группу `group`.
    virtual TFuture<void> AddMember(
        const std::string& group,
        const std::string& member,
        const TAddMemberOptions& options = {}) = 0;

    // @gearonixx: исключить `member` из группы `group`.
    virtual TFuture<void> RemoveMember(
        const std::string& group,
        const std::string& member,
        const TRemoveMemberOptions& options = {}) = 0;

    // @gearonixx: проверить, есть ли у `user` право `permission` на узел `path`.
    // Это и есть основной хелпер ACL-проверки в YT: мастер сам обходит
    // дерево наследования ACL и возвращает вердикт + причину.
    virtual TFuture<TCheckPermissionResponse> CheckPermission(
        const std::string& user,
        const NYPath::TYPath& path,
        NYTree::EPermission permission,
        const TCheckPermissionOptions& options = {}) = 0;

    // @gearonixx: dry-run проверки прав по произвольному ACL, без привязки к
    // объекту. Передаём ACL как YSON-дерево; user опционален (nullopt =
    // текущий пользователь).
    virtual TFuture<TCheckPermissionByAclResult> CheckPermissionByAcl(
        const std::optional<std::string>& user,
        NYTree::EPermission permission,
        NYTree::INodePtr acl,
        const TCheckPermissionByAclOptions& options = {}) = 0;

    // Methods below correspond to simple authentication scheme
    // and are intended to be used on clusters without third-party tokens (e.g. Yandex blackbox).
    // @gearonixx: нативная аутентификация YT — пароли и токены хранит сам кластер,
    // без внешнего сервиса. На внутренних Яндексовых инсталляциях вместо этого
    // используется blackbox, и эти методы там не нужны.
    // @gearonixx: смена пароля. Передаётся именно SHA256, чтобы plain-пароль
    // никогда не уходил по сети.
    virtual TFuture<void> SetUserPassword(
        const std::string& user,
        const TString& currentPasswordSha256,
        const TString& newPasswordSha256,
        const TSetUserPasswordOptions& options) = 0;

    // @gearonixx: выпуск нового долгоживущего токена. Аутентификация
    // выполняется по SHA256 текущего пароля.
    virtual TFuture<TIssueTokenResult> IssueToken(
        const std::string& user,
        const TString& passwordSha256,
        const TIssueTokenOptions& options) = 0;

    // @gearonixx: отзыв конкретного токена. Идентифицируем токен его SHA256
    // (plain-токен мы не храним и принимать здесь его нет смысла).
    virtual TFuture<void> RevokeToken(
        const std::string& user,
        const TString& passwordSha256,
        const TString& tokenSha256,
        const TRevokeTokenOptions& options) = 0;

    // @gearonixx: перечислить токены пользователя — возвращает их SHA256,
    // опционально с метаданными (Description, дата и т.д.).
    virtual TFuture<TListUserTokensResult> ListUserTokens(
        const std::string& user,
        const TString& passwordSha256,
        const TListUserTokensOptions& options) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi
