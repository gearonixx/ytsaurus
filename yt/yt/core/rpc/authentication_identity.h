#pragma once

#include "public.h"

#include <yt/yt/core/yson/public.h>

#include <library/cpp/yt/misc/hash.h>

namespace NYT::NRpc {

////////////////////////////////////////////////////////////////////////////////

// @gearonixx
//     GetAuthenticationIdentity() возвращает структуру с парой (User, UserTag) — то есть полную identity, включая тег для квотирования
struct TAuthenticationIdentity
{
    TAuthenticationIdentity() = default;
    explicit TAuthenticationIdentity(const std::string& user, const std::string& userTag = {});

    bool operator==(const TAuthenticationIdentity& other) const = default;

    // @gearonixx @@UPSTREAM

    // Почему не сделали сразу: исторически написали со строкой, оно везде используется,
    // рефакторинг — это апдейт всех мест чтения (if (tag.empty()) → if (!tag.has_value())),
    // сериализаций, сравнений. TODO висит как напоминание, что хорошо бы починить, но не критично.

    // TODO(babenko): consider wrapping with std::optional
    std::string User;
    std::string UserTag;
};

//! Returns the current identity.
//! If none is explicitly set then returns the root identity.
const TAuthenticationIdentity& GetCurrentAuthenticationIdentity();

//! Sets the current identity.
//! The passed identity is not copied, just a pointer is updated.
//! The caller must ensure a proper lifetime of the installed identity.
void SetCurrentAuthenticationIdentity(const TAuthenticationIdentity* identity);

//! Returns the root identity, which is the default one.
const TAuthenticationIdentity& GetRootAuthenticationIdentity();

void FormatValue(TStringBuilderBase* builder, const TAuthenticationIdentity& value, TStringBuf spec);

void Serialize(const TAuthenticationIdentity& identity, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

class TCurrentAuthenticationIdentityGuard
    : private TNonCopyable
{
public:
    TCurrentAuthenticationIdentityGuard();
    explicit TCurrentAuthenticationIdentityGuard(const TAuthenticationIdentity* newIdentity);
    TCurrentAuthenticationIdentityGuard(TCurrentAuthenticationIdentityGuard&& other) noexcept;
    ~TCurrentAuthenticationIdentityGuard();

    TCurrentAuthenticationIdentityGuard& operator=(TCurrentAuthenticationIdentityGuard&& other) noexcept;

private:
    const TAuthenticationIdentity* OldIdentity_;

    void Release() noexcept;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc

template <>
struct THash<NYT::NRpc::TAuthenticationIdentity>
{
    size_t operator()(const NYT::NRpc::TAuthenticationIdentity& value) const;
};
