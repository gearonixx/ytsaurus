#!/usr/bin/env bash
# F07 — S3 import accepts any TLS cert.
# This is a procedural description; running it requires a built ytserver and
# the import_table tool. No exploit code is provided. The static source
# evidence below is sufficient for vendor verification.
set -u

REPO_ROOT="${REPO_ROOT:-/home/x/try3/ytsaurus}"

echo "[*] Static evidence:"
echo
grep -n 'InsecureSkipVerify' \
    "$REPO_ROOT/yt/yt/tools/import_table/lib/import_table.cpp" \
    "$REPO_ROOT/yt/yt/core/crypto/tls.cpp" \
    "$REPO_ROOT/yt/yt/core/crypto/config.cpp" \
    2>/dev/null || true

cat <<'EOF'

[*] Dynamic verification (description only):

  1. Generate a self-signed certificate for hostname "wrong-host.example":

     openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
       -keyout /tmp/wrong.key -out /tmp/wrong.crt \
       -subj "/CN=wrong-host.example" \
       -addext "subjectAltName=DNS:wrong-host.example"

  2. Run a TLS-terminating proxy (e.g. mitmproxy, stunnel) on 127.0.0.1:9443
     using /tmp/wrong.crt + /tmp/wrong.key. Have it forward to a fake S3
     responder that returns a tiny valid bucket listing.

  3. Configure the importer with --s3-url https://127.0.0.1:9443 and any
     accessKeyId/secretAccessKey.

  4. Observe: the import succeeds. With TLS verification disabled, the
     mismatched hostname in the cert is ignored and the AWS SigV4
     headers — including accessKeyId — are sent to the proxy in cleartext
     (relative to the attacker, who terminates the TLS).

  Mitigation: drop the hardcoded sslConfig->InsecureSkipVerify = true.
EOF
