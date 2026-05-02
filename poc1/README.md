# YTsaurus — security disclosure package

Tree audited: `b5a39533962` (branch `main`).
Researcher contact: gearonixx@proton.me
Date: 2026-04-27.

This directory contains a vulnerability disclosure package intended for
**security@ytsaurus.tech** (per the project's `SECURITY.md`). Each finding
folder is self-contained and includes:

- `README.md` — vulnerability description, affected code path, impact, attacker
  prerequisites.
- `repro.*` — a **non-weaponized** reproducer that demonstrates the bug is
  reachable. Where the bug runs as root, the reproducer uses benign payloads
  (`/usr/bin/id`, `--version`, `--help`) so that running it produces evidence
  of execution but no destructive effect.
- `fix.patch` — a suggested code change.

> These reproducers are intended for vendor verification only. They are not
> drop-in exploits and do not include payloads that destroy data, persist
> access, or move laterally. Do not use them against systems you do not own
> or have written authorization to test.

## Findings index

| # | Severity | File:line | Class |
|---|---|---|---|
| 01 | CRITICAL | `yt/yt/tools/yt_sudo_fixup/main.cpp:6-22` | Unauthenticated setuid root command runner |
| 02 | HIGH | `yt/yt/server/tools/proc.cpp:236` | rsync argument-injection in root-context tool |
| 03 | HIGH | `yt/yt/server/tools/proc.cpp:441` | mke2fs argument-injection in root-context tool |
| 04 | HIGH | `yt/yt/server/tools/proc.cpp:52` | `rm -rf` argument-injection in root-context tool |
| 05 | HIGH | `yt/yt/server/tools/proc.cpp:74` | `bash -c` of attacker-string in root tool (design review) |
| 06 | HIGH | `yt/yt/core/crypto/tls.cpp:640-660` | TLS hostname binding skipped if caller omits `Host` |
| 07 | HIGH | `yt/yt/tools/import_table/lib/import_table.cpp:151` | TLS verification hard-disabled for S3 imports |

## Recommended disclosure timeline

1. Send `disclosure-email.md` to **security@ytsaurus.tech**.
2. The project commits to acknowledgement within 5 working days
   (`SECURITY.md`).
3. Coordinate disclosure window — 90 days is the standard ceiling; F01/F07 are
   straightforward to mitigate and a 7-day window is appropriate per
   `SECURITY.md`.
4. Public CVE assignment after a fix is available.

## Trust boundaries assumed

- F01 assumes `yt-sudo-fixup` is installed setuid-root in production
  (the binary is non-functional otherwise — `setreuid(realUid, 0)` requires
  euid=0). This is the documented deployment model for privileged YT node
  operations.
- F02–F05 assume an actor that can drive the tools subsystem on a YT node:
  this includes the YT node process itself (so any RCE/SSRF that lands code
  inside the node), and any operator/cluster path that constructs a YSON
  config consumed by the privileged child process. Crossing this boundary
  with another bug then cleanly escalates to root.
- F06–F07 assume a network attacker between the YT client and a TLS
  endpoint (the documented threat model TLS exists to defend against).

## Files in this package

- `disclosure-email.md` — ready-to-send disclosure to security@ytsaurus.tech.
- `finding-0*/` — per-finding folders (see above).
- `README.previous-audit.md` — unrelated prior audit's notes, retained for
  reference.
