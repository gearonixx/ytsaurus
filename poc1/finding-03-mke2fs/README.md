# F03 — mke2fs argv-injection in `TMkFsAsRootTool` (root context)

**Severity:** HIGH.
**CVSS 3.1:** 8.1 — `AV:N/AC:H/PR:L/UI:N/S:U/C:H/I:H/A:H`.
**File:** `yt/yt/server/tools/proc.cpp:437-445`.
**Config struct:** `yt/yt/server/tools/proc.h` (`TMkFsConfig`).
**Caller:** `yt/yt/server/node/data_node/nbd_session.cpp:82`.

## Vulnerable code

```cpp
void TMkFsAsRootTool::operator()(const TMkFsConfigPtr& config) const
{
    TrySetUid(0);
    execl("/usr/sbin/mke2fs", "/usr/sbin/mke2fs", "-F", "-q", "-t",
          config->Type.c_str(), config->Path.c_str(), (void*)nullptr);
    ...
}
```

```cpp
void TMkFsConfig::Register(TRegistrar registrar) {
    registrar.Parameter("path", &TThis::Path).Default();
    registrar.Parameter("type", &TThis::Type).Default("ext4");
}
```

No character-class validation on `Type`; no constraint that `Path` is
absolute or doesn't begin with `-`.

## Why it is exploitable

mke2fs (e2fsprogs ≥ 1.43) accepts `-d <directory>` to **populate the
new filesystem image from an arbitrary directory**, walking it as the
calling user (root). It also accepts `-O <feature>` and `-E <ext-options>`
families that influence the resulting FS layout.

A `Path` that begins with `-` is parsed as an option:

```
config = { type = "ext4"; path = "-d/etc"; }
```

Becomes:

```
mke2fs -F -q -t ext4 -d/etc <implicit_device?>
```

This either populates the new FS with the contents of `/etc` (a confidentiality
leak — credentials, keys, machine-id end up readable to the attacker who can
later read the device image), or fails in a way that still consumed CPU/IO. A
crafted device path (e.g. an attacker-prepared loop device) lets the attacker
later mount the image and read it.

Combined with the `-F` (force) flag already in argv, mke2fs will not refuse
to operate on devices with existing filesystems.

## Reproducer (benign)

`repro.cpp` mirrors the execl pattern with a stub `mke2fs` binary that just
prints argv. The attacker payload is `path = "--help"`:

```
[stub-mke2fs] argv[0] = /usr/sbin/mke2fs
[stub-mke2fs] argv[1] = -F
[stub-mke2fs] argv[2] = -q
[stub-mke2fs] argv[3] = -t
[stub-mke2fs] argv[4] = ext4
[stub-mke2fs] argv[5] = --help        <-- attacker-controlled, parsed as option
```

`--help` is harmless; in production `--help` would be replaced with `-d/etc`
or `-O ^metadata_csum` (downgrade integrity protection).

## Fix sketch

`fix.patch` validates `Type` against a strict regex, rejects `Path` not
starting with `/`, and inserts `--` before positional arguments.
