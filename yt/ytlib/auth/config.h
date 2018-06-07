#pragma once

#include "public.h"

#include <yt/core/ypath/public.h>

#include <yt/core/misc/config.h>

namespace NYT {
namespace NAuth {

////////////////////////////////////////////////////////////////////////////////

class TDefaultBlackboxServiceConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    TDefaultBlackboxServiceConfig()
    {
        RegisterParameter("host", Host)
            .Default("blackbox.yandex-team.ru");
        RegisterParameter("port", Port)
            .Default(443);
        RegisterParameter("secure", Secure)
            .Default(true);
        RegisterParameter("request_timeout", RequestTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("attempt_timeout", AttemptTimeout)
            .Default(TDuration::Seconds(10));
        RegisterParameter("backoff_timeout", BackoffTimeout)
            .Default(TDuration::Seconds(1));
    }

    TString Host;
    ui16 Port;
    bool Secure;

    TDuration RequestTimeout;
    TDuration AttemptTimeout;
    TDuration BackoffTimeout;
};

DEFINE_REFCOUNTED_TYPE(TDefaultBlackboxServiceConfig)

////////////////////////////////////////////////////////////////////////////////

class TBlackboxTokenAuthenticatorConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    TBlackboxTokenAuthenticatorConfig()
    {
        RegisterParameter("scope", Scope);
        RegisterParameter("enable_scope_check", EnableScopeCheck)
            .Default(true);
    }

    TString Scope;
    bool EnableScopeCheck;
};

DEFINE_REFCOUNTED_TYPE(TBlackboxTokenAuthenticatorConfig)

////////////////////////////////////////////////////////////////////////////////

class TCachingBlackboxTokenAuthenticatorConfig
    : public TBlackboxTokenAuthenticatorConfig
    , public TAsyncExpiringCacheConfig
{ };

DEFINE_REFCOUNTED_TYPE(TCachingBlackboxTokenAuthenticatorConfig)

////////////////////////////////////////////////////////////////////////////////

class TCypressTokenAuthenticatorConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    TCypressTokenAuthenticatorConfig()
    {
        RegisterParameter("root_path", RootPath);
        RegisterParameter("realm", Realm)
            .Default("cypress");
    }

    NYPath::TYPath RootPath;
    TString Realm;
};

DEFINE_REFCOUNTED_TYPE(TCypressTokenAuthenticatorConfig)

////////////////////////////////////////////////////////////////////////////////

class TCachingCypressTokenAuthenticatorConfig
    : public TCypressTokenAuthenticatorConfig
    , public TAsyncExpiringCacheConfig
{ };

DEFINE_REFCOUNTED_TYPE(TCachingCypressTokenAuthenticatorConfig)

////////////////////////////////////////////////////////////////////////////////

class TBlackboxCookieAuthenticatorConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    TBlackboxCookieAuthenticatorConfig()
    { }
};

DEFINE_REFCOUNTED_TYPE(TBlackboxCookieAuthenticatorConfig)

////////////////////////////////////////////////////////////////////////////////

class TCachingBlackboxCookieAuthenticatorConfig
    : public TBlackboxCookieAuthenticatorConfig
    , public TAsyncExpiringCacheConfig
{
public:
    TCachingBlackboxCookieAuthenticatorConfig()
    { }
};

DEFINE_REFCOUNTED_TYPE(TCachingBlackboxCookieAuthenticatorConfig)

////////////////////////////////////////////////////////////////////////////////

class TAuthenticationManagerConfig
    : public virtual NYT::NYTree::TYsonSerializable
{
public:
    bool RequireAuthentication;
    NAuth::TCachingBlackboxTokenAuthenticatorConfigPtr BlackboxTokenAuthenticator;
    NAuth::TCachingBlackboxCookieAuthenticatorConfigPtr BlackboxCookieAuthenticator;
    NAuth::TDefaultBlackboxServiceConfigPtr BlackboxService;
    NAuth::TCachingCypressTokenAuthenticatorConfigPtr CypressTokenAuthenticator;

    TAuthenticationManagerConfig()
    {
        // COMPAT(prime@)
        RegisterParameter("require_authentication", RequireAuthentication)
            .Alias("enable_authentication")
            .Default(true);
        RegisterParameter("blackbox_token_authenticator", BlackboxTokenAuthenticator)
            .Alias("token_authenticator")
            .Optional();
        RegisterParameter("blackbox_cookie_authenticator", BlackboxCookieAuthenticator)
            .Alias("cookie_authenticator")
            .Optional();
        RegisterParameter("blackbox_service", BlackboxService)
            .Alias("blackbox")
            .Optional();
        RegisterParameter("cypress_token_authenticator", CypressTokenAuthenticator)
            .Optional();
    }
};

DEFINE_REFCOUNTED_TYPE(TAuthenticationManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NAuth
} // namespace NYT
