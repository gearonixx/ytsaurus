#pragma once

#include "public.h"

#include <yt/yt/library/auth_server/credentials.h>

#include <yt/yt/ytlib/api/public.h>

#include <yt/yt/core/http/http.h>

#include <yt/yt/core/rpc/authenticator.h>

namespace NYT::NHttpProxy {

////////////////////////////////////////////////////////////////////////////////
/// Result — то, что вернула аутентификация наружу: логин, realm, scope. Это используется дальше в обработке запроса (логи, DriverRequest_.AuthenticatedUser, ACL).
// TokenHash — хеш самого токена/куки, по которому аутентифицировались. Нужен отдельно для двух вещей: кеширование результата аутентификации (ключ кеша — хеш, чтобы не светить сырой токен в памяти/логах) и аудит/троттлинг по конкр

struct TAuthenticationResultAndToken
{
    NAuth::TAuthenticationResult Result;
    TString TokenHash;
};

void SetStatusFromAuthError(const NHttp::IResponseWriterPtr& req, const TError& error);

////////////////////////////////////////////////////////////////////////////////

class THttpAuthenticator
    : public NHttp::IHttpHandler
{
public:
    THttpAuthenticator(
        TBootstrap* bootstrap,
        const NAuth::TAuthenticationManagerConfigPtr& authManagerConfig,
        const NAuth::IAuthenticationManagerPtr& authManager);

    void HandleRequest(
        const NHttp::IRequestPtr& req,
        const NHttp::IResponseWriterPtr& rsp) override;

    TErrorOr<TAuthenticationResultAndToken> Authenticate(
        const NHttp::IRequestPtr& request,
        bool disableCsrfTokenCheck = false);

    const NAuth::ITokenAuthenticatorPtr& GetTokenAuthenticator() const;

private:
    TBootstrap* Bootstrap_;

    const NAuth::TAuthenticationManagerConfigPtr Config_;
    const NAuth::IAuthenticationManagerPtr AuthenticationManager_;
    const NAuth::ITokenAuthenticatorPtr TokenAuthenticator_;
    const NAuth::ICookieAuthenticatorPtr CookieAuthenticator_;
};

////////////////////////////////////////////////////////////////////////////////
///
    /// Composite — паттерн, когда один объект объединяет несколько других и делегирует им работу.
    // Здесь: один аутентификатор, который внутри держит несколько аутентификаторов, по одному на каждый порт.

class TCompositeHttpAuthenticator
    : public NHttp::IHttpHandler
{
public:
    explicit TCompositeHttpAuthenticator(const THashMap<int, THttpAuthenticatorPtr>& portAuthenticators);

    void HandleRequest(
        const NHttp::IRequestPtr& req,
        const NHttp::IResponseWriterPtr& rsp) override;

    TErrorOr<TAuthenticationResultAndToken> Authenticate(
        const NHttp::IRequestPtr& request,
        bool disableCsrfTokenCheck = false);

    const NAuth::ITokenAuthenticatorPtr& GetTokenAuthenticatorOrThrow(int port) const;

private:
    THashMap<int, THttpAuthenticatorPtr> PortAuthenticators_;

    TErrorOr<THttpAuthenticatorPtr> GetPortAuthenticator(int port) const;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
