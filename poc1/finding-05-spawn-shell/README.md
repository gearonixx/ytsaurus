# F05 — `bash -c` of attacker-supplied string in `TSpawnShellTool`

**Severity:** HIGH (design-review).
**File:** `yt/yt/server/tools/proc.cpp:69-81`.
**Companion file:** `yt/yt/server/tools/seccomp.cpp` (not audited in this pass —
flagged for vendor review).

## Vulnerable code

```cpp
void TSpawnShellTool::operator()(TSpawnShellConfigPtr config) const
{
    SetupSeccomp();

    if (config->Command) {
        execl("/bin/bash", "/bin/bash", "-c",  config->Command->c_str(), (void*)nullptr);
    } else {
        execl("/bin/bash", "/bin/bash", (void*)nullptr);
    }
    ...
}
```

## What needs vendor review

Unlike F02-F04, this tool is **intentionally** a shell — it is the job-shell
mechanism. The security posture relies entirely on `SetupSeccomp()` to
contain the shell. Vendor verification needed:

1. The seccomp BPF program in `yt/yt/server/tools/seccomp.cpp` must block
   at minimum:
   - `setuid`, `setgid`, `setreuid`, `setregid`, `setresuid`, `setresgid`
   - `setfsuid`, `setfsgid`
   - `clone` with `CLONE_NEWUSER` (user-namespace LPE)
   - `unshare` with `CLONE_NEWUSER`
   - `mount`, `umount2`, `pivot_root`, `chroot`
   - `ptrace`, `process_vm_readv`, `process_vm_writev`
   - `keyctl`, `add_key`, `request_key`
   - `bpf` (and any kernel-LPE primitives reachable from there)
   - `kexec_load`, `kexec_file_load`, `init_module`, `finit_module`,
     `delete_module`
   - `perf_event_open` (kernel-LPE history)
   - `userfaultfd` (kernel-LPE history)
   - `io_uring_setup` (a long history of escapes)
2. If the tool is invoked under uid 0, also confirm capability bounding —
   seccomp alone is not enough; the bounding set should drop CAP_SYS_ADMIN /
   CAP_NET_ADMIN / CAP_DAC_OVERRIDE / CAP_SYS_PTRACE.
3. Audit calling sites: anywhere a `TSpawnShellConfig` flows from
   user-side input must require an authenticated identity and a TTL on the
   spawned shell.

## Reproducer

There is no exploit code. The "reproducer" is a code-walkthrough request:
please confirm that the syscall allowlist in `seccomp.cpp` rejects every
syscall in the list above, and please attach the output of:

```
seccomp-tools dump $(which yt-job-shell)
```

(or equivalent strace/auditd evidence) to the ticket.
