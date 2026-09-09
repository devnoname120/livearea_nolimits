# Icon-cache feature parity

## Supported features

Version 1.5.0 enables the same icon-cache correction for retail 3.60, retail 3.65,
and PTEL/testkit 3.60. The existing single-victim LRU eviction, pending artwork
reload handling, and pinned native consumer handoff are shared. No firmware gets
an eviction-only subset. The 2 MiB texture pool, per-page slot count, and cache
object layouts are not enlarged or rewritten.

The release and fresh source configurations enable `LIVEAREA_ICON_CACHE_TRIAL`.
That historical option name remains compatible; explicitly setting it to `OFF`
still builds only the capacity and recovery features. Runtime logging remains
controlled independently by `LIVEAREA_ICON_CACHE_LOGGING`, default `OFF`.

Feature parity is conditional on the same fail-closed validation used on 3.60.
A validation or installation failure leaves the existing shell capacity patches
active rather than installing cache hooks at unverified locations.

## Binary profiles

Offsets below are relative to the indicated shell segment, not an IDB image base.

| Shell | Shell NID | PAF NID | Text size | Data size | Pool slot, segment 1 | Initializer, segment 0 |
| --- | --- | --- | --- | --- | --- | --- |
| Retail 3.60 | `0x0552F692` | `0xCD679177` | `0x541B74` | `0x93FAC` | `0x6DEC` | `0x2C74` |
| Retail 3.65 | `0x5549BF1F` | `0x73F90499` | `0x5420F4` | `0x93FBC` | `0x6DFC` | `0x2CCC` |
| PTEL 3.60 | `0xEAB89D5C` | `0xCD679177` | `0x535CF4` | `0x92D1C` | `0x6BBC` | `0x2C74` |

The retail 3.65 PAF input was decrypted from the previously identified 3.65 update
package. The retail 3.60 input came from the existing device dump. The PTEL input
is the original loaded-image dump. Module identities were read from the binaries,
not inferred from filenames or spoofable system-version APIs.

IDA analysis independently identifies the 3.65 initializer at `0x81002CCC` writing
the pool pointer at `0x81549DFC` (data base `0x81543000`). The PTEL initializer at
`0x83203A34` writes `0x83787B6C` (data base `0x83780FB0`). Both allocate an eight-byte
holder containing backing memory and a surface-pool pointer, using the existing
`SceShellIconCache` / `SceShellIconSruface` providers and 2 MiB backing allocation.
Removing only the old shell-NID guard would use the wrong pool slot on both ports
and the wrong initializer on 3.65.

All three inspected PAF images have segment 0 size `0x300D00`. IDA analysis and
actual-byte tests confirm the shared cache entry points and object layout used
by the correction. In particular, the getter uses the cache mutex, checks the
registration byte at image +81, obtains the surface at +156, and retains it for
the caller. The scan uses the same mutex and cache clock. Existing complete-byte
checks on the release/eviction/getter routines, consumer and scan prologues,
handle thunk/vtable, and relocated data references remain enabled.

| PAF location | Offset |
| --- | --- |
| Cache scan | `0x15E6A` in text |
| Stock eviction predicate | `0x13EF6` in text |
| Surface release | `0x8997A` in text |
| Widget consumer | `0x15F9E2` in text |
| Surface getter | `0x13344` in text |
| Image-handle getter thunk | `0x14672` in text |
| Image-handle vtable | `0x2E106C` in text |
| Recursive mutex | `0xB238` in data |
| Cache clock | `0xB224` in data |

Private input text-image SHA-256 values:

| PAF input | SHA-256 |
| --- | --- |
| Retail 3.60, before relocation | `c402c8b96c0c21883f360f826cb66d1e3988737cf379bcb7ad2502bdee7eab07` |
| Retail 3.65, before relocation | `ee3f8d7456666462b4d427bbfc43db3eae81c83cd9660ee429c62df57c6486c0` |
| PTEL 3.60, loaded dump | `16bd6024c3fbf907bbd40f49a1722d97112ff0fc0de673ec0bf40b021d6d6bbe` |

Firmware images are not included in the repository or release.

## Runtime validation and startup

The shell NID selects one `CacheProfile`. Text/data segment sizes, the initializer
prologue, and its relocated MOVW/MOVT reference to the profile's pool slot are
checked before dereferencing that slot. The following store instruction is also
checked. This reference check applies to both already-initialized and deferred
startup, so a wrong offset cannot be accepted merely because some unrelated
pointer happens to be non-null.

PAF is checked against the NID paired with that shell. Only after validating its
code, mutex/clock references, handle vtable/thunk, and imported stack-guard address
are the shared consumer and scan hooks installed. Consumer installation precedes
scan installation, so additional evictions are never enabled without the matching
reload correction. A deferred startup retains the selected profile until the
native pool initializer has returned. Cleanup clears the profile as well as the
existing hooks and cached addresses.

## Verification

The 3.65 regression was observed failing at cache startup before the port. The
runner also compares the compiled shell and cache profile lists: adding support
for a shell without a matching cache profile fails the suite. The
same host suite now exercises each of the three shell/PAF profiles under ASan and
UBSan, with logging both enabled and disabled. Coverage includes immediate and
deferred activation of both cache hooks, mismatched shell/PAF pairs, unknown
identities, segment sizes, every checked code byte, relocated pool references,
partial hook failures, cleanup, LRU ordering, retries, and bounded texture release.
The shared consumer tests continue to cover pending requests, fast completion,
pinned handoff, cancellation, queue failures, and repeated reload cycles.

Actual shell and PAF text images can be supplied to those same tests. Their
address operands are relocated to the host fixture only after the corresponding
original checks; the shell's original pool-slot address and the PAF module NID
are checked directly. The pinned taiHEN/libsubstitute tests exercise scan,
consumer, and each shell initializer prologue at both test load bases. Existing
native ARM capacity/recovery tests remain available through the same runner.

```sh
LIVEAREA_TEST_PAF_TEXT=/path/to/retail360-paf.text.bin \
LIVEAREA_TEST_PAF_365_TEXT=/path/to/retail365-paf.text.bin \
LIVEAREA_TEST_PAF_PTEL_TEXT=/path/to/ptel360-paf.text.bin \
LIVEAREA_TEST_SUBSTITUTE=/path/to/taihen/substitute \
LIVEAREA_TEST_RECOVERY_360=/path/to/retail360-recovery.text.bin \
LIVEAREA_TEST_RECOVERY_365=/path/to/retail365-recovery.text.bin \
LIVEAREA_TEST_PLUGIN_ELF=build/livearea_nolimits \
python3 tests/run.py \
  0x0552F692 /path/to/retail360-shell.text.bin \
  0x5549BF1F /path/to/retail365-shell.text.bin \
  0xEAB89D5C /path/to/ptel360-shell.text.bin
```

The optional native ARM suites require Unicorn; the pinned substitute revision
and original retail device trials are recorded in [the historical cache notes](icon-cache-trial.md).

## Hardware coverage

The original correction has a retail 3.60 device-validation record. Retail 3.65
now also has an affected-library hardware run: recovery produced 573 visible
applications across 15 pages, and scrolling, edit mode, idle, and repeated
sleep/wake worked with the cache correction enabled. Its trace recorded 32 LRU
evictions without a cache-scan error. PTEL retains firmware-analysis, real-byte,
sanitizer, and relocation coverage but no corresponding hardware run.

This does not establish a populated 500-top-level-icon/50-page result: the retail
3.65 database used 15 ordinary pages and many folders. Neither host mocks nor one
device result prove the full scheduler/renderer at the absolute boundary.

On each additional device, verify boot, repeated first-to-last-page scrolling,
artwork reloads after revisiting pages, folder moves, launches, and persistence
across a second reboot. Keep plugin, configuration, and database rollback backups;
an expanded database can exceed stock-shell limits when the plugin is disabled.
This is a remaining validation task, not an intentionally disabled feature.
