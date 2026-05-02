# F01 — Unauthenticated setuid root command runner

**Severity:** CRITICAL — local privilege escalation to root.
**CVSS 3.1:** 9.8 — `AV:L/AC:L/PR:N/UI:N/S:C/C:H/I:H/A:H` (assuming setuid install).
**File:** `yt/yt/tools/yt_sudo_fixup/main.cpp:6-22`.

## Vulnerable code

```c
int main(int argc, char* argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: yt-sudo-wrapper UID CMD [ARGS]\n");
        return 2;
    }

    int realUid = atoi(argv[1]);
    if (setreuid(realUid, 0) != 0) {
        perror("setreuid");
        return 1;
    }

    execv(argv[2], argv+3);
    perror("exec");
    return 1;
}
```

## Why it is exploitable

The binary's `setreuid(realUid, 0)` only succeeds when the process already has
`euid=0` — i.e. it is intended to be installed setuid-root in production so
that the YTsaurus node can perform privileged housekeeping. Once installed
that way:

1. `argv[1]` (target real-uid) is fully attacker-controlled.
2. `argv[2]` (command path) is fully attacker-controlled.
3. `argv[3..]` (arguments) are fully attacker-controlled.
4. There is **no authentication** of the invoking process (no SCM_CREDENTIALS
   socket, no `getppid` check against the YT node, no handshake).
5. There is **no allowlist** of permitted commands.
6. There is **no minimum-uid** check; an attacker can request `realUid=0`
   to make both real and effective uid become 0.

Net effect: any local user with execute access to the binary becomes root.

## Reproducer

`repro.sh` runs the binary with the benign payload `/usr/bin/id` and prints
the resulting credentials. If output shows `euid=0` (or `uid=0` when invoked
with `0` as the first argument), the vulnerability is confirmed.

```bash
$ ./repro.sh /usr/local/bin/yt-sudo-fixup
[*] Demonstrating that an arbitrary command runs with euid=0:
[*] $ /usr/local/bin/yt-sudo-fixup $(id -u) /usr/bin/id
uid=1000(...) euid=0(root) groups=...
[!] CONFIRMED: an unprivileged user just executed /usr/bin/id with euid=0.
```

The reproducer intentionally invokes `/usr/bin/id` only — it does not spawn a
shell, write files, or persist privilege.

## Fix sketch

See `fix.patch`. Three layers:

1. **Drop setuid**; ship as a regular root-owned binary launched only by the
   YT node via a unix-domain socket with `SO_PEERCRED` validation.
2. If keeping setuid, **hard-allowlist** the permitted `argv[2]` paths
   (e.g. `/bin/mount`, `/bin/umount`, `/usr/sbin/mke2fs` only), and refuse
   `realUid <= 0` and any `argv[2]` not in the allowlist.
3. Drop ambient capabilities to the minimum needed
   (`cap_sys_admin`/`cap_chown`/`cap_dac_override+ep` via `setcap`) instead
   of full setuid root.
