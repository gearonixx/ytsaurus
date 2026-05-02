# F04 — `rm -rf` argv-injection in `TRemoveDirAsRootTool` (root context)

**Severity:** HIGH.
**CVSS 3.1:** 8.1 — `AV:L/AC:L/PR:H/UI:N/S:C/C:H/I:H/A:H`.
**File:** `yt/yt/server/tools/proc.cpp:48-56`.
**Callers:** `yt/yt/server/node/exec_node/layer_location.cpp:670-671`,
`yt/yt/server/node/exec_node/slot_location.cpp:907`.

## Vulnerable code

```cpp
void TRemoveDirAsRootTool::operator()(const std::string& path) const
{
    // Child process
    TrySetUid(0);
    execl("/bin/rm", "/bin/rm", "-rf", path.c_str(), (void*)nullptr);
    ...
}
```

## Why it is exploitable

`/bin/rm` interprets a positional argument that starts with `-` as an
option. An attacker-controlled `path` of the form `--no-preserve-root`
followed (in a subsequent invocation) by `/` removes the host. A `path` of
`-rf` is also accepted, although `-rf` is already implicit. More usefully,
GNU `rm` accepts `--interactive=never` and `--one-file-system` which
materially change the semantics of subsequent operations.

In the current callers, `path` flows from `slotPath`/`VolumesPath_` etc. —
which are themselves derived from node configuration. An attacker who
influences node config (for example via a misconfigured shared config
directory, or via chaining a config-injection bug) gets a host-takeout
primitive: `path = "/"` is accepted as the positional argument and
`rm -rf /` runs as root.

The `/` case is partially mitigated by GNU rm's default `--preserve-root`,
but `path = "/etc"` is not, nor is `path = "/var"` etc. — all wipe critical
host state.

## Reproducer (benign)

`repro.cpp` uses a stub `rm` to print argv. Payload: `path = "--help"`. The
stub demonstrates that the attacker-controlled value lands in argv as an
option. To verify the destructive case under controlled conditions, run the
reproducer against a throw-away container with the stub returning exit 0.

## Fix sketch

`fix.patch` rejects `path` not absolute, containing `..`, or starting with
`-`, and inserts `--`.
