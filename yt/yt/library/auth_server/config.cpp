#include "config.h"

#include <yt/yt/core/concurrency/config.h>

#include <yt/yt/core/https/config.h>

namespace NYT::NAuth {

////////////////////////////////////////////////////////////////////////////////

void TAuthCacheConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cache_ttl", &TThis::CacheTtl)
        .Default(TDuration::Minutes(5));
    registrar.Parameter("optimistic_cache_ttl", &TThis::OptimisticCacheTtl)
        .Default(TDuration::Hours(1));
    registrar.Parameter("error_ttl", &TThis::ErrorTtl)
        .Default(TDuration::Seconds(15));
}

////////////////////////////////////////////////////////////////////////////////

void TBlackboxServiceConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("http_client", &TThis::HttpClient)
        .DefaultNew();
    registrar.Parameter("host", &TThis::Host)
        .Default("blackbox.yandex-team.ru");
    registrar.Parameter("port", &TThis::Port)
        .Default(443);
    registrar.Parameter("secure", &TThis::Secure)
        .Default(true);
    registrar.Parameter("blackbox_service_id", &TThis::BlackboxServiceId)
        .Default("blackbox");
    registrar.Parameter("request_timeout", &TThis::RequestTimeout)
        .Default(TDuration::Seconds(15));
    registrar.Parameter("attempt_timeout", &TThis::AttemptTimeout)
        .Default(TDuration::Seconds(10));
    registrar.Parameter("backoff_timeout", &TThis::BackoffTimeout)
        .Default(TDuration::Seconds(1));
    registrar.Parameter("use_lowercase_login", &TThis::UseLowercaseLogin)
        .Default(true);
}

////////////////////////////////////////////////////////////////////////////////

void TTvmServiceConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("use_tvm_tool", &TThis::UseTvmTool)
        .Default(false);
    registrar.Parameter("client_self_id", &TThis::ClientSelfId)
        .Default(0);
    registrar.Parameter("client_disk_cache_dir", &TThis::ClientDiskCacheDir)
        .Optional();
    registrar.Parameter("tvm_host", &TThis::TvmHost)
        .Optional();
    registrar.Parameter("tvm_port", &TThis::TvmPort)
        .Optional();
    registrar.Parameter("client_enable_user_ticket_checking", &TThis::ClientEnableUserTicketChecking)
        .Default(false);
    registrar.Parameter("client_blackbox_env", &TThis::ClientBlackboxEnv)
        .Default("ProdYateam");
    registrar.Parameter("client_enable_service_ticket_fetching", &TThis::ClientEnableServiceTicketFetching)
        .Default(false);
    registrar.Parameter("client_self_secret", &TThis::ClientSelfSecret)
        .Optional();
    registrar.Parameter("client_dst_map", &TThis::ClientDstMap)
        .Optional();
    registrar.Parameter("client_enable_service_ticket_checking", &TThis::ClientEnableServiceTicketChecking)
        .Default(false);

    registrar.Parameter("tvm_tool_self_alias", &TThis::TvmToolSelfAlias)
        .Optional();
    registrar.Parameter("tvm_tool_port", &TThis::TvmToolPort)
        .Optional();
    registrar.Parameter("tvm_tool_auth_token", &TThis::TvmToolAuthToken)
        .Optional();

    registrar.Parameter("enable_mock", &TThis::EnableMock)
        .Default(false);
}

////////////////////////////////////////////////////////////////////////////////

void TBlackboxTokenAuthenticatorConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("scope", &TThis::Scope);
    registrar.Parameter("enable_scope_check", &TThis::EnableScopeCheck)
        .Default(true);
    registrar.Parameter("get_user_ticket", &TThis::GetUserTicket)
        .Default(true);
}

////////////////////////////////////////////////////////////////////////////////

void TBlackboxTicketAuthenticatorConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("scopes", &TThis::Scopes)
        .Optional();
    registrar.Parameter("enable_scope_check", &TThis::EnableScopeCheck)
        .Default(false);
}

////////////////////////////////////////////////////////////////////////////////

void TCachingTokenAuthenticatorConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cache", &TThis::Cache)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

void TCypressTokenAuthenticatorConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("root_path", &TThis::RootPath)
        .Default("//sys/tokens");
    registrar.Parameter("realm", &TThis::Realm)
        .Default("cypress");

    registrar.Parameter("secure", &TThis::Secure)
        .Default(false);
}

////////////////////////////////////////////////////////////////////////////////

void TBlackboxCookieAuthenticatorConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("domain", &TThis::Domain)
        .Default("yt.yandex-team.ru");

    registrar.Parameter("csrf_secret", &TThis::CsrfSecret)
        .Default();
    registrar.Parameter("csrf_token_ttl", &TThis::CsrfTokenTtl)
        .Default(DefaultCsrfTokenTtl);

    registrar.Parameter("get_user_ticket", &TThis::GetUserTicket)
        .Default(true);
}

////////////////////////////////////////////////////////////////////////////////

void TCachingCookieAuthenticatorConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cache", &TThis::Cache)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

void TDefaultSecretVaultServiceConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("host", &TThis::Host)
        .Default("vault-api.passport.yandex.net");
    registrar.Parameter("port", &TThis::Port)
        .Default(443);
    registrar.Parameter("secure", &TThis::Secure)
        .Default(true);
    registrar.Parameter("http_client", &TThis::HttpClient)
        .DefaultNew();
    registrar.Parameter("request_timeout", &TThis::RequestTimeout)
        .Default(TDuration::Seconds(3));
    registrar.Parameter("vault_service_id", &TThis::VaultServiceId)
        .Default("yav");
    registrar.Parameter("consumer", &TThis::Consumer)
        .Optional();
}

////////////////////////////////////////////////////////////////////////////////

void TBatchingSecretVaultServiceConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("batch_delay", &TThis::BatchDelay)
        .Default(TDuration::MilliSeconds(100));
    registrar.Parameter("max_subrequests_per_request", &TThis::MaxSubrequestsPerRequest)
        .Default(100)
        .GreaterThan(0);
    registrar.Parameter("requests_throttler", &TThis::RequestsThrottler)
        .DefaultNew();

    registrar.Preprocessor([] (TThis* config) {
        config->RequestsThrottler->Limit = 100;
    });
}

////////////////////////////////////////////////////////////////////////////////

void TCachingSecretVaultServiceConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cache", &TThis::Cache)
        .DefaultNew();

    registrar.Preprocessor([] (TThis* config) {
        config->Cache->RefreshTime = std::nullopt;
        config->Cache->ExpireAfterAccessTime = TDuration::Seconds(60);
        config->Cache->ExpireAfterSuccessfulUpdateTime = TDuration::Seconds(60);
        config->Cache->ExpireAfterFailedUpdateTime = TDuration::Seconds(60);
    });
}

////////////////////////////////////////////////////////////////////////////////

void TCypressCookieStoreConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("full_fetch_period", &TThis::FullFetchPeriod)
        .Default(TDuration::Minutes(5));

    registrar.Parameter("error_eviction_time", &TThis::ErrorEvictionTime)
        .Default(TDuration::Minutes(1));
}

////////////////////////////////////////////////////////////////////////////////

void TCypressCookieGeneratorConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cookie_expiration_timeout", &TThis::CookieExpirationTimeout)
        .Default(TDuration::Days(90));
    registrar.Parameter("cookie_renewal_period", &TThis::CookieRenewalPeriod)
        .Default(TDuration::Days(30));

    registrar.Parameter("secure", &TThis::Secure)
        .Default(true);

    registrar.Parameter("http_only", &TThis::HttpOnly)
        .Default(true);

    registrar.Parameter("domain", &TThis::Domain)
        .Default();
    registrar.Parameter("path", &TThis::Path)
        .Default("/");

    registrar.Parameter("redirect_url", &TThis::RedirectUrl)
        .Default();

    registrar.Postprocessor([] (TThis* config) {
        if (config->CookieRenewalPeriod > config->CookieExpirationTimeout) {
            THROW_ERROR_EXCEPTION(
                "\"cookie_renewal_period\" cannot be greater than \"cookie_expiration_timeout\"");
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TCypressCookieManagerConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cookie_store", &TThis::CookieStore)
        .DefaultNew();
    registrar.Parameter("cookie_generator", &TThis::CookieGenerator)
        .DefaultNew();
    registrar.Parameter("cookie_authenticator", &TThis::CookieAuthenticator)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

TString TAuthenticationManagerConfig::GetCsrfSecret() const
{
    if (BlackboxCookieAuthenticator &&
        BlackboxCookieAuthenticator->CsrfSecret)
    {
        return *BlackboxCookieAuthenticator->CsrfSecret;
    }

    return TString();
}

TInstant TAuthenticationManagerConfig::GetCsrfTokenExpirationTime() const
{
    if (BlackboxCookieAuthenticator) {
        return TInstant::Now() - BlackboxCookieAuthenticator->CsrfTokenTtl;
    }

    return TInstant::Now() - DefaultCsrfTokenTtl;
}

void TAuthenticationManagerConfig::Register(TRegistrar registrar)
{
    // COMPAT(prime@)
    registrar.Parameter("require_authentication", &TThis::RequireAuthentication)
        .Alias("enable_authentication")
        .Default(true);
    registrar.Parameter("blackbox_token_authenticator", &TThis::BlackboxTokenAuthenticator)
        .Alias("token_authenticator")
        .Optional();
    registrar.Parameter("blackbox_cookie_authenticator", &TThis::BlackboxCookieAuthenticator)
        .Alias("cookie_authenticator")
        .DefaultNew();
    registrar.Parameter("blackbox_service", &TThis::BlackboxService)
        .Alias("blackbox")
        .DefaultNew();
    registrar.Parameter("cypress_token_authenticator", &TThis::CypressTokenAuthenticator)
        .Optional();
    registrar.Parameter("tvm_service", &TThis::TvmService)
        .Optional();
    registrar.Parameter("blackbox_ticket_authenticator", &TThis::BlackboxTicketAuthenticator)
        .Optional();
    registrar.Parameter("cypress_cookie_manager", &TThis::CypressCookieManager)
        .Default();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NAuth
