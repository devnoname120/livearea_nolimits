# Per-instance icon cache

Version 1.9.0 applies the cache correction to SceShell's own icon-pool and image
objects. It does not modify shared PAF executable code or original virtual tables.
The plugin remains one SUPRX, installed under `*main` with a full reboot.

## Scope and behavior

Two four-byte detours in SceShell observe icon-pool initialization and the native
image-request wrapper. Each explicitly replays the complete original PUSH.W
instruction before continuing at entry+4. The request continuation preserves all
five arguments, including the stack argument. This avoids the misaligned
CBZ/CBNZ trampoline problem reproduced during earlier development.

Once the native pool exists, the implementation validates the firmware identity,
pool size and original virtual tables, selected PAF instruction sequences, and
recursive mutex behavior. It clones the tables into plugin memory and changes
only the private pool object's vptr and compatible images returned through the
SceShell request wrapper. It checks the image's cache/pool ownership before
registration. Other processes and other PAF pools retain their existing behavior.
There is no PID check placed after an unsafe jump into SceShell-only code.

Native allocation runs first. On failure, the wrapper tries the cache mutex
without blocking because its caller may already hold the surface-global mutex.
It chooses the oldest eligible surface with one remaining reference, releases
that surface and retries native allocation. Another victim is selected only if
the retry still fails. Persistent image handles, source information and offscreen
model references stay intact. The texture pool remains 2 MiB, so cold or evicted
artwork can still briefly appear white while loading.

The native widget consumer checks an image's virtual status immediately before
fetching its surface. The replacement status method recognizes that exact caller.
If the texture is missing, it queues the native reload and reports pending so the
widget's existing subscription remains alive. If a texture is resident, it holds
a temporary per-thread reference that the following getter transfers into the
native binding path. Native code releases it after binding/callbacks. Other status
callers and real decoder errors keep their original handling.

A weak registry tracks images without retaining them. Native destructor callbacks
remove records, clear temporary pins and restore both original tables before
delegating destruction. The native pool lifetime contract still applies: its
users must finish before the pool is destroyed. Runtime unloading is refused
while detours or instances remain live; replacing the SUPRX requires a reboot.

## Firmware profiles

Offsets are relative to the named SceShell segment.

| Firmware | Shell NID | PAF NID | Text bytes | Data bytes | Pool slot, data | Pool init, text | Image request, text |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Retail 3.60 | `0552F692` | `CD679177` | `541B74` | `93FAC` | `6DEC` | `2C74` | `BD34E` |
| Retail 3.65 | `5549BF1F` | `73F90499` | `5420F4` | `93FBC` | `6DFC` | `2CCC` | `BD3A6` |
| PTEL 3.60 | `EAB89D5C` | `CD679177` | `535CF4` | `92D1C` | `6BBC` | `2C74` | `BA976` |

All three PAF text segments are `0x300D00` bytes and use the verified pool/image
method layouts. PTEL's image request was independently matched to the retail
five-argument wrapper; the different pool slot and request offset are explicit.
Unsupported/mismatched code is rejected without removing working capacity patches.
The historical option `LIVEAREA_ICON_CACHE_TRIAL` now selects this implementation;
there is no build option that re-enables the old shared PAF hooks.

## Verification

`python3 tests/run.py` includes ASan/UBSan tests of instance ownership, LRU ordering
and ties, allocation retries, protected/foreign images, lock contention, pending
reload, reference transfer, nested/cross-thread use, cancellation, destruction,
address reuse and startup/unload behavior. The instance tests run with logging
both enabled and disabled. Historical cache-policy tests remain as comparisons;
the old implementation is not a production CMake source.

With Unicorn installed, `tests/test_instance_cache_arm.py` executes the compiled
backend together with native PAF allocation, Surface release and widget-consumer
instructions. It checks pending subscription preservation and allocation pressure
between status and getter. Supply the three PAF text images as positional inputs;
`LIVEAREA_INSTANCE_SHELL_TEXTS` accepts a path-separated list of the matching shell
text images for startup, actual detour dispatch, five-argument continuation,
validation rejection and partial-install rollback checks. OS services, decoding
and rendering/event callbacks are mocked. Firmware images are private test inputs
and are not distributed.

A diagnostic version of this policy completed physical boot and scripted
forward/reverse scrolling on retail 3.60. Its logged allocation retries freed one
surface per successful retry, and missing-texture reloads recovered. The fastest
pass still showed temporary white placeholders. These results do not establish
zero loading flashes, an A/B improvement over v1.8.0, operation at every capacity
boundary, or NetStream compatibility. Retail 3.65 and PTEL validation is offline.
The exact no-logs release artifact has compiled ARM/build validation; the physical
test used the diagnostic development build.

## Release and diagnostics

The release uses Release mode, `-O2`, and both logging switches `OFF`. It has no
diagnostic file-I/O imports and leaves logs from previous builds untouched.
Development-only controller recovery and embedded rollback images are not part
of the public source or release artifact.

For matching diagnostics, build the release tag with `LIVEAREA_DEBUG_LOGGING=ON`
and a distinct `LIVEAREA_DEBUG_BUILD_ID`. Startup should show `cache=1`,
`instance-cache ... shell-detours-installed`, and then `instance-cache ... active`
with `paf_hooks=0`. Detour installation alone does not prove instance activation.
Sampled events record image registrations, pending reloads, allocation retries,
victims, reference transfers, contention and stale pins. The default sync interval
is one line; a larger interval reduces storage overhead but can lose recent events
on a hard crash. These are diagnostics, not a rendering-performance benchmark.
