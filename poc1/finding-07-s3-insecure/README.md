# F07 — TLS verification hard-disabled for S3 imports

**Severity:** HIGH.
**CVSS 3.1:** 7.4 — `AV:N/AC:H/PR:N/UI:N/S:U/C:H/I:H/A:N` (credential exposure
on every S3 import).
**File:** `yt/yt/tools/import_table/lib/import_table.cpp:150-152`.

## Vulnerable code

```cpp
auto sslConfig = NYT::New<NYT::NCrypto::TSslContextConfig>();
sslConfig->InsecureSkipVerify = true;

auto poller = CreateThreadPoolPoller(1, "S3Poller");
auto client = NS3::CreateClient(
    std::move(clientConfig),
    std::move(credentialProvider),     // accessKeyId + secretAccessKey
    sslConfig,
    poller,
    poller->GetInvoker());
```

The S3 client is constructed with `accessKeyId` + `secretAccessKey` and
`InsecureSkipVerify = true`. There is no flag, no opt-in, no environment
variable — the line is hardcoded.

## Why it is exploitable

Any actor on the network path between the YTsaurus importer and the S3
endpoint can:

1. Intercept the TLS handshake (the importer accepts any certificate or
   none).
2. Read the AWS Signature V4 headers in the request — these expose the
   `accessKeyId` directly and allow offline brute-force or replay against
   the secret if the implementation re-uses nonces.
3. Modify the imported data in flight, leading to data-integrity failures
   downstream.

In practice the most common attacker is a corporate or coffee-shop network
operator, but in cloud deployments it is also any compromised hop —
including a node in the same VPC that wins a DNS race or runs an ARP
spoof.

## Reproducer

The "reproducer" is a one-liner audit:

```sh
$ grep -n 'InsecureSkipVerify *= *true' yt/yt/tools/import_table/lib/import_table.cpp
151:    sslConfig->InsecureSkipVerify = true;
```

That is sufficient to demonstrate the bug — the value is a literal in the
source.

A dynamic reproducer (`repro_mitm.sh`) describes how to verify with a local
HTTPS endpoint presenting a self-signed cert for `wrong-host.example` and a
test S3-protocol responder. We provide the description, not a working
attacker server, because the literal-source-grep already constitutes proof.

## Fix sketch

Drop the line. The `TSslContextConfig` already supports a custom
`CertAuthority` if a private CA is needed.
