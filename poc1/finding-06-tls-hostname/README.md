# F06 — TLS hostname binding skipped when caller omits `Host`

**Severity:** HIGH.
**CVSS 3.1:** 7.4 — `AV:N/AC:H/PR:N/UI:N/S:U/C:H/I:H/A:N`.
**File:** `yt/yt/core/crypto/tls.cpp:640-660`, `225-256`.

## Vulnerable code

```cpp
TFuture<IConnectionPtr> Dial(const TNetworkAddress& remoteAddress, TDialerContextPtr context) override
{
    return Underlying_->Dial(remoteAddress).Apply(BIND(
        [
            ctx = Context_,
            poller = Poller_,
            context = std::move(context),
            allowBypassTls = AllowBypassTls_,
            insecureSkipVerify = Context_->IsInsecureSkipVerify()
        ] (const IConnectionPtr& underlying) -> IConnectionPtr {
            if (allowBypassTls && context->BypassTls) {
                return underlying;
            }
            auto connection = New<TTlsConnection>(ctx, poller, underlying);
            if (context && context->Host) {
                connection->SetHost(*context->Host);    // <-- only here
            }
            connection->StartClient(insecureSkipVerify);
            return connection;
        }));
}
```

`SetHost` is the only place that calls `SSL_set1_host`:

```cpp
void SetHost(const std::string& host) {
    SSL_set_hostflags(Ssl_.get(), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    SSL_set1_host(Ssl_.get(), host.c_str());
    SSL_set_tlsext_host_name(Ssl_.get(), host.c_str());
}
```

`StartClient(false)` does call `SSL_set_verify(SSL_VERIFY_PEER | …)`, but
that *only* checks that the peer presents a certificate that chains to the
configured trust store. Without `SSL_set1_host`, OpenSSL does **no
hostname-to-certificate binding**.

## Why it is exploitable

A network attacker who holds **any** valid certificate from any CA in the
client's trust store can MITM the connection — they redirect the TCP
connection to themselves (BGP hijack, DNS poisoning, malicious local
resolver, etc.), present their own valid certificate (e.g. a Let's Encrypt
cert for a domain they own), and the YTsaurus client accepts the handshake
because:

1. Verification is on, so OpenSSL checks the cert chain — passes (any valid
   web cert chains to Let's Encrypt → ISRG Root X1, which is in the
   default system store).
2. Hostname binding is off, so the cert's CN/SANs are not compared to the
   intended host.

This is the canonical "missing hostname check" vulnerability class
(CVE-2014-3568, CVE-2018-7160 family).

## Reproducer

`repro_test.cpp` is a unit-test-style reproducer. It:

1. Spawns a local TLS server with a self-issued certificate for hostname
   `attacker.example.com`.
2. Constructs a `TTlsDialer` with the system trust store **plus** the
   self-issued CA temporarily added (modelling "any valid cert").
3. Dials the server **without** setting `context->Host`.
4. Asserts the handshake succeeds — demonstrating that no hostname check
   was performed.
5. Then re-runs with `context->Host = "victim.example.com"` and asserts the
   handshake **fails** — demonstrating that the check works when the host
   is plumbed through.

The reproducer does not execute any payload over the connection. It is a
test of the dial path only.

> Because building the YT crypto layer requires the YT build system, this
> reproducer is delivered as a description + `gtest`-shaped pseudo-code in
> `repro_test.cpp`. The vendor's existing TLS unit tests
> (`yt/yt/core/crypto/unittests/`) are the appropriate place to land it.

## Fix sketch

`fix.patch` makes `Host` mandatory — refuse to dial without a host name —
and adds an explicit, separately-named `SkipHostnameCheck` flag for the
narrow case where a caller really does need to bypass the check (e.g. IP
literals).
