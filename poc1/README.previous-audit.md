# poc1 — proof-of-concept exploits for the HIGH findings

Two HIGH findings from the audit:

- **F1**: integer overflow in `TryDeserializeProtoWithEnvelope`
  (`yt/yt/core/misc/protobuf_helpers.cpp:189,205`)
- **F2**: `YT_VERIFY` abort in `DeserializeProtoWithEnvelope`
  (`yt/yt/core/misc/protobuf_helpers.cpp:225`)

Both are reachable from authenticated cluster-peer traffic (Hydra mutations,
forwarded tablet writes, exec-node spec decode) — see callers list in the audit.

## Honest impact ceiling

Neither bug, on its own, is a control-flow-hijack RCE. There is no write
primitive — no overwrite of a function pointer, vtable, or return address.
What an attacker actually gets:

| Finding | Primitive | Practical worst case |
|---|---|---|
| F1 | OOB-read of up to ~4 GiB into a `TSharedRef` handed to a compression codec | (a) SIGSEGV on the unmapped page → process crash; (b) if the codec succeeds on the OOB bytes, ~MB-scale heap disclosure inside the deserialized protobuf message that subsequently propagates over RPC/log lines |
| F2 | `YT_VERIFY(false)` → `abort()` | One TCP frame kills a Hydra peer. Replayed deterministically via the consensus log, it kills every peer in the cell on every restart → permanent quorum wedge until operators surgically truncate the changelog. |

So the realistic "RCE-equivalent" is **F2 used to wedge a master cell + leak
heap via F1 in the resulting crash dump**. That is enough to take a YTsaurus
deployment offline indefinitely from a single compromised peer.

If you need a true RCE you would have to chain F1's leak with a separate write
gadget (e.g. a deserialization bug in one of the protobuf message types
decoded after the OOB-read pollutes its bytes). That chain is plausible but
out of scope for this PoC pack.

## Files

| File | What it does |
|---|---|
| `f1_envelope_overflow.cpp` | Self-contained ASAN-friendly reproducer of the F1 arithmetic. Build with `make f1` and run — ASAN flags the OOB read. |
| `f1_craft_payload.py` | Emits the exact bytes an attacker sends on the wire. |
| `f2_craft_payload.py` | Emits bytes that pass the size check but fail `ParseFromArray` → `YT_VERIFY(false)` → `abort()` on the receiver. |
| `fuzz_harness.cpp` | libFuzzer harness that re-discovers F1 + F2 in seconds when linked against the real `TryDeserializeProtoWithEnvelope`. |
| `Makefile` | Builds the standalone reproducer and the fuzz harness. |
| `verify.sh` | Drives the standalone reproducer and prints a pass/fail. |

## Reproducing F1 standalone (no YT build required)

```
cd poc1 && make f1 && ./f1_envelope_overflow
```

The program replicates the exact check at `protobuf_helpers.cpp:205`,
populates `MessageSize`/`EnvelopeSize` with the overflow values, allocates
a small `data` buffer with ASAN red-zones around it, and performs the same
read pattern the codec would. ASAN reports a `heap-buffer-overflow READ`.

## Reproducing F1 / F2 against the real binary

These need a YTsaurus build environment. The fuzz harness (see
`fuzz_harness.cpp`) calls `TryDeserializeProtoWithEnvelope` directly. Drop it
into `yt/yt/core/misc/unittests/`, add a `ya.make` target with
`-fsanitize=address,fuzzer`, and run for ~60 s — both inputs surface.

The on-the-wire crafters (`f1_craft_payload.py`, `f2_craft_payload.py`)
produce the actual byte streams an attacker sends. Wrap them in a TCP frame
that hits any of the audit's listed callers — for example, a peer-to-peer
Hydra mutation, or a forwarded tablet write request body.
