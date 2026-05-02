// F06 — TLS hostname binding skipped when caller omits Host.
// Reproducer in the shape of a YT gtest unit test.
//
// Drop into yt/yt/core/crypto/unittests/tls_hostname_ut.cpp and add to the
// CMakeLists. The test demonstrates that without context->Host, an attacker
// cert for an unrelated hostname is accepted.
//
// Non-weaponized: no payload is sent over the connection. The test only
// observes whether the handshake completes.

#include <yt/yt/core/crypto/tls.h>
#include <yt/yt/core/crypto/config.h>
#include <yt/yt/core/net/dialer.h>
#include <yt/yt/core/net/listener.h>
#include <yt/yt/core/concurrency/poller.h>
#include <yt/yt/core/concurrency/thread_pool_poller.h>
#include <yt/yt/core/test_framework/framework.h>

namespace NYT::NCrypto {
namespace {

using namespace NConcurrency;
using namespace NNet;

// PEM-encoded throwaway certificate + key pair for hostname
// "attacker.example.com". Generated with:
//   openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
//       -keyout key.pem -out cert.pem \
//       -subj "/CN=attacker.example.com" \
//       -addext "subjectAltName=DNS:attacker.example.com"
//
// (Test-only material. Replace with vendor-generated certs at landing time.)
static const char* kAttackerCertPem  = "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n";
static const char* kAttackerKeyPem   = "-----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----\n";

TEST(TTlsDialerHostnameTest, AcceptsMismatchedCertWhenHostOmitted)
{
    // Trust the attacker's CA — models "any valid cert chains to a CA we
    // trust", which is the realistic threat for systems using the default
    // system trust store.
    auto serverConfig = New<TSslContextConfig>();
    serverConfig->CertChain = New<TPemBlobConfig>();
    serverConfig->CertChain->Value = kAttackerCertPem;
    serverConfig->PrivateKey = New<TPemBlobConfig>();
    serverConfig->PrivateKey->Value = kAttackerKeyPem;

    auto clientConfig = New<TSslContextConfig>();
    clientConfig->CertAuthority = New<TPemBlobConfig>();
    clientConfig->CertAuthority->Value = kAttackerCertPem;  // trust attacker CA

    auto poller = CreateThreadPoolPoller(1, "TlsTest");
    auto serverCtx = CreateSslContext(serverConfig);
    auto clientCtx = CreateSslContext(clientConfig);

    auto rawListener = CreateListener(/* address = */ {}, poller);
    auto tlsListener = CreateTlsListener(serverCtx, rawListener, poller);
    auto serverFuture = tlsListener->Accept();

    auto rawDialer = CreateDialer(New<TDialerConfig>(), poller, NLogging::TLogger());
    auto tlsDialer = CreateTlsDialer(clientCtx, rawDialer, New<TDialerConfig>(), poller);

    // The vulnerable case: caller omits Host.
    auto context = New<TDialerContext>();
    // context->Host  = std::nullopt;     <-- vulnerable path
    auto dialFuture = tlsDialer->Dial(rawListener->GetAddress(), context);

    auto dialResult = dialFuture.WithTimeout(TDuration::Seconds(5)).Get();
    EXPECT_TRUE(dialResult.IsOK())
        << "VULNERABLE: TLS handshake to a server presenting cert for "
           "attacker.example.com succeeded even though no host was specified. "
           "This means the YT TLS dialer accepts any cert that chains to a "
           "trusted CA, regardless of intended hostname.";
}

TEST(TTlsDialerHostnameTest, RejectsMismatchedCertWhenHostProvided)
{
    // Same setup as above…
    auto serverConfig = New<TSslContextConfig>();
    serverConfig->CertChain = New<TPemBlobConfig>();
    serverConfig->CertChain->Value = kAttackerCertPem;
    serverConfig->PrivateKey = New<TPemBlobConfig>();
    serverConfig->PrivateKey->Value = kAttackerKeyPem;

    auto clientConfig = New<TSslContextConfig>();
    clientConfig->CertAuthority = New<TPemBlobConfig>();
    clientConfig->CertAuthority->Value = kAttackerCertPem;

    auto poller = CreateThreadPoolPoller(1, "TlsTest2");
    auto serverCtx = CreateSslContext(serverConfig);
    auto clientCtx = CreateSslContext(clientConfig);

    auto rawListener = CreateListener({}, poller);
    auto tlsListener = CreateTlsListener(serverCtx, rawListener, poller);
    (void)tlsListener->Accept();

    auto rawDialer = CreateDialer(New<TDialerConfig>(), poller, NLogging::TLogger());
    auto tlsDialer = CreateTlsDialer(clientCtx, rawDialer, New<TDialerConfig>(), poller);

    // Fixed case: caller provides Host.
    auto context = New<TDialerContext>();
    context->Host = "victim.example.com";
    auto dialFuture = tlsDialer->Dial(rawListener->GetAddress(), context);

    auto dialResult = dialFuture.WithTimeout(TDuration::Seconds(5)).Get();
    EXPECT_FALSE(dialResult.IsOK())
        << "When Host is set, the cert mismatch should cause the handshake "
           "to fail.";
}

} // namespace
} // namespace NYT::NCrypto
