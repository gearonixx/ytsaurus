To: security@ytsaurus.tech
Subject: [Security] Seven vulnerabilities in YTsaurus (CRITICAL setuid LPE + 6 HIGH)

Hello YTsaurus security team,

Per your SECURITY.md I am reporting seven vulnerabilities I discovered while
auditing the YTsaurus tree at commit b5a39533962. One is critical (a
setuid-root command runner with no caller authentication or argv allowlist),
six are high severity (argument-injection in privileged tools, TLS hostname
verification bypass, and an S3 import path that hard-disables TLS
verification).

Summary table:

  #   Severity    Location                                                  Class
  --  ----------  --------------------------------------------------------  ----------------------------------------
  01  CRITICAL    yt/yt/tools/yt_sudo_fixup/main.cpp:6-22                   Unauthenticated setuid root cmd runner
  02  HIGH        yt/yt/server/tools/proc.cpp:236                           rsync argv injection (root)
  03  HIGH        yt/yt/server/tools/proc.cpp:441                           mke2fs argv injection (root)
  04  HIGH        yt/yt/server/tools/proc.cpp:52                            rm -rf argv injection (root)
  05  HIGH        yt/yt/server/tools/proc.cpp:74                            bash -c attacker-string (design review)
  06  HIGH        yt/yt/core/crypto/tls.cpp:640-660                         TLS hostname check skipped without Host
  07  HIGH        yt/yt/tools/import_table/lib/import_table.cpp:151         TLS verification hard-disabled for S3

For each finding I have prepared:
  - a technical write-up,
  - a non-weaponized reproducer using benign payloads (/usr/bin/id,
    --version, --help) so verification produces evidence of execution but no
    destructive effect, and
  - a suggested patch.

I am happy to share the reproducer package over a channel of your choosing
(encrypted attachment, secure file transfer, etc.). I have not published
anything publicly and will follow whatever embargo schedule you propose. My
default expectation, per your SECURITY.md, is acknowledgement within 5
working days, and a coordinated public disclosure once a mitigation is
available — 7 days for F01/F07 (the fixes are mechanical), up to 90 days for
the rest.

Suggested mitigations at a glance:
  - F01: drop the setuid bit; replace with file capabilities and a
    well-defined caller authentication path. If the tool must remain, hard
    allowlist permitted argv[2] paths and forbid uid=0 except for an
    explicit, named operation.
  - F02-F04: insert "--" before positional arguments to rsync/mke2fs/rm,
    and validate Source/Destination/Type/Path do not begin with "-".
  - F05: review the seccomp profile in
    yt/yt/server/tools/seccomp.cpp confirms blocking of setuid/setgid/clone
    (CLONE_NEWUSER), mount, ptrace, keyctl, bpf.
  - F06: make the host required for TLS dialing, or document a separate
    explicit "skip hostname check" flag.
  - F07: remove the hard-coded sslConfig->InsecureSkipVerify = true and use
    the system trust store; if a custom CA is needed, plumb it through
    TSslContextConfig.

CVSS estimates (3.1):
  F01  9.8  AV:L/AC:L/PR:N/UI:N/S:C/C:H/I:H/A:H  (assumes setuid install)
  F02  8.8  AV:N/AC:L/PR:L/UI:N/S:U/C:H/I:H/A:H
  F03  8.1  AV:N/AC:H/PR:L/UI:N/S:U/C:H/I:H/A:H
  F04  8.1  AV:L/AC:L/PR:H/UI:N/S:C/C:H/I:H/A:H
  F05  --   intentional design; advisory
  F06  7.4  AV:N/AC:H/PR:N/UI:N/S:U/C:H/I:H/A:N
  F07  7.4  AV:N/AC:H/PR:N/UI:N/S:U/C:H/I:H/A:N  (credential exposure)

Please confirm receipt and let me know your preferred channel for the full
disclosure package.

Best regards,
[name]
gearonixx@proton.me
