# F02 — rsync argv-injection in `TCopyDirectoryContentTool` (root context)

**Severity:** HIGH.
**CVSS 3.1:** 8.8 — `AV:N/AC:L/PR:L/UI:N/S:U/C:H/I:H/A:H`.
**File:** `yt/yt/server/tools/proc.cpp:232-242`.
**Config struct:** `yt/yt/server/tools/proc.h:214-225` (`TCopyDirectoryContentConfig`).

## Vulnerable code

```cpp
void TCopyDirectoryContentTool::operator()(TCopyDirectoryContentConfigPtr config) const
{
    SafeSetUid(0);

    execl("/usr/bin/rsync", "/usr/bin/rsync", "-q", "--perms", "--recursive",
          "--specials", "--links",
          config->Source.c_str(), config->Destination.c_str(), (void*)nullptr);
    ...
}
```

`Source` and `Destination` are `std::string` deserialized from a YSON config:

```cpp
void TCopyDirectoryContentConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("source", &TThis::Source).NonEmpty();
    registrar.Parameter("destination", &TThis::Destination).Default();
}
```

The only validation is `NonEmpty()` on `Source`. There is no check that the
strings do not begin with `-`.

## Why it is exploitable

`rsync` interprets a positional argument starting with `-` as an option.
Several option families are abusable; the historically-popular ones include
`-e <cmd>` (set the remote shell), `--rsh=<cmd>`, and `--debug=...` — any of
which let an attacker steer rsync into running arbitrary commands as part of
its remote-source resolution. Because `SafeSetUid(0)` precedes the `execl`,
that command runs as **root**.

## Attacker prerequisites

The attacker needs to submit a `TCopyDirectoryContentConfig` to a YT node.
This is reachable through any caller of `RunTool<TCopyDirectoryContentTool>`
that takes its config from network-side input — most importantly the exec
node's volume-management code paths. An authenticated cluster operator who
can drive a copy-directory operation triggers it; an actor who has chained
another bug to get code into the YT node process triggers it trivially.

## Reproducer (benign)

`repro.cpp` constructs a `TCopyDirectoryContentConfig` with a
`Source` that starts with `-` and asserts that the `execl` argv received by
rsync would interpret it as an option. To avoid actually running rsync, the
reproducer compiles a stub `rsync` that just prints argv and exits — placed
on `$PATH` ahead of the real rsync — so the demonstration prints the
attacker-controlled argv slot without performing any network or filesystem
side effect.

The "trigger" payload uses `--version` (benign):

```
source      = "--version"
destination = "/tmp/dest"
```

When the vulnerable `TCopyDirectoryContentTool` runs, the rsync stub prints:

```
[stub-rsync] argv[0] = /usr/bin/rsync
[stub-rsync] argv[1] = -q
[stub-rsync] argv[2] = --perms
...
[stub-rsync] argv[7] = --version          <-- attacker-controlled
[stub-rsync] argv[8] = /tmp/dest
```

The `--version` slot is an option — confirming that rsync sees it as an
option, not a path. Substitute `--rsh=/path/to/cmd` in production rsync and
the cmd executes.

`repro.cpp` is a free-standing test harness — it does not pull in YT headers,
so it can be built and run without the full source tree.

## Fix sketch

`fix.patch` adds `--` before the positional arguments and rejects values
starting with `-`.
