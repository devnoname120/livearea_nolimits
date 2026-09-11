# Retail 3.60 icon-cache implementation history

> Historical v1.3-v1.8 implementation. Current releases use the
> [per-instance cache implementation](instance-cache.md); the shared PAF hook
> installation described below is no longer part of the production build.


This file preserves the original retail 3.60 investigation and device trials.
As of v1.5.0, the same correction is enabled by default for retail 3.60, retail
3.65, and PTEL 3.60 using validated profiles. Current compatibility, tests, and
hardware-coverage limits are documented in [cache profiles](cache-profiles.md).

**The LRU device comparison improved scrolling but did not fix revisited white icons.**
The user no longer noticed the short freezes and saw all artwork while scrolling
to the bottom. Scrolling back up still produced white pages; the two-page detour
still restored them. These are user observations, not measured frame timings.
A separate consumer reload correction is now installed and active. The prepared
9,107-byte candidate uploaded correctly but failed runtime validation because
its stack-guard check assumed a PAF-relative address for an imported variable.
After isolating and correcting that check, the new 9,736-byte build activated
on 7 September at 11:38:12 CEST. Post-reboot byte comparisons and rollback/config
checks completed at 11:38:14 CEST. Navigation with the consumer correction still
needs the user's comparison; successful activation is not a visual correctness
result. The installed SHA-256 is
`9cdbeca2ad349b83a7f9e9dbb96c721fe9af51ef8cb24568a7a4f0dd90199297`.
At that trial stage the optional build was disabled by default; the 2 MiB pool
was not enlarged.

## First-trial failure and recovery

The first trial (SUPRX SHA-256
`4e8c98d21915f5d3ee6eb7a6cbccab68f0b247107c5b8679f8ea67037cb7fdf2`)
crashed at the loading spinner before LiveArea, as reported by the user.
Do not redeploy this artifact. Host tests and byte/layout checks did not cover
the complete firmware execution path. The original installed plugin is backed
up at `ur0:/tai/livearea_nolimits.before-icon-cache-20260906.suprx` (SHA-256
`4e23c0ffb2af067a2cc2300f62b292d12131de8caa18d736cb5dfdc75b7bf9f8`).
Rollback and retrieval of the trial log/crash evidence take priority over
another cache-policy experiment.

Offline failure reproduction found that taiHEN's pinned libsubstitute
(`4713452731dd489c13c9415c3c31637845f108f3`) needs a 12-byte patch at the
halfword-aligned eviction entry. Its branch at entry+4 targets entry+10,
inside that patch region. The actual transform routine rejects the bytes with
`SUBSTITUTE_ERR_FUNC_BAD_INSN_AT_START`. The first trial then rolled back the
working page/count patches on hook failure. That error handling has been
corrected locally: an optional cache-hook failure now leaves the limit patches
active. The recovered device log contains only
`icon-cache trial validation or hook failed`: activation failed, but that old
message cannot identify the exact failed check or returned error. The eviction
policy itself has not been validated by the failed startup.

Recovery: holding L was reported to crash at the same spinner. A saved layout
above the stock page limit may also fail without the original limit patches;
there are page-population paths that do not check a NULL page result before
insertion. A temporary `ux0:tai/config.txt` was created on the removable microSD
(there was no previous file), using the pre-trial configuration with only the
NoLimits path changed to the verified backup. The card was read back and
ejected. The user then confirmed that boot no longer crashed. The prior run
restored the original plugin at its canonical `ur0:/tai/livearea_nolimits.suprx`
path and downloaded a matching readback. On 7 September, that saved readback
was checked again against the local original; both have the backup hash above.
The prior run issued deletion of the temporary `ux0:tai/config.txt`. FTP was
initially unavailable during continuation, then returned. A fresh directory
listing confirmed that the temporary `config.txt` was absent (only the
`._config.txt` sidecar remained). Fresh downloads of the canonical original
plugin, remote backup and `ur0:tai/config.txt` matched the saved originals.
The repaired scan-hook trial was subsequently uploaded at the canonical path
and read back byte-for-byte before an acknowledged reboot. The original
rollback backup was left unchanged.

A recent encrypted SceShell `.spsp2dmp` was copied off the card and preserved;
it has not yielded a decoded fault address. Recovery records are under
`build/icon-cache-trial/recovery/`.

## Replacement hook

### Startup ordering

The first scan-hook build (`973b81f2c4b3f11cb4cfe92ae5b93403bd826200f34ad483d987742a81993268`)
booted far enough to return FTP and a precise diagnostic:
`icon-cache trial PAF lookup failed: 0x90010002`.
taiHEN defines that code as `TAI_ERROR_NOT_FOUND`. The optional failure no
longer removed the baseline limit patches. This device result established a
startup-order problem before scan-hook installation; the old generic failure
log could not distinguish it from the independently reproduced bad hook site.

Both the deployed build and its replacement install a bootstrap hook on the shell's icon-pool initializer
at retail segment 0 + `0x2C74`. It calls the original initializer first, then
attempts PAF lookup and hook installation once. No worker thread, delay,
or polling is used. If the pool already exists when the plugin starts, it
attempts installation immediately instead. A failed deferred attempt does not
release its own executing bootstrap trampoline. The consumer hook is installed
before the LRU scan hook; a scan-installation failure removes the consumer hook.
Plugin stop removes the scan, consumer and bootstrap hooks in that order,
before the baseline patches are released.

The initializer needs an 8-byte jump and displaces 10 bytes because a MOVW
crosses the patch boundary. The first six bytes are checked exactly; the MOVW
opcode and R8 destination are checked while its relocated address immediate
is excluded. Both relocator passes succeed on the real retail shell bytes.

### PAF scan

The original predicate remains unmodified. The trial now hooks
`ScePafMisc_FC4FD91F`, the cache scan taking `(cache, predicate, outItem)`.
Only calls whose predicate is the verified original `ScePafMisc_5A80D3F5`
and whose cache uses the shell icon pool receive the replacement policy.
The scan is still called through taiHEN's hook chain. Other predicates and
non-icon caches receive their original arguments and behavior.

The replacement entry is at segment 0 + `0x15E6A`. Its first 12 bytes contain
no branch or relocated absolute address. The first conditional branch is at
entry + 14, outside the patch. Both the prologue transformation and subsequent
backward-branch check pass with the pinned libsubstitute code and the real
retail 3.60 PAF text. The transformed prologue is byte-for-byte identical at
both tested load bases. This does not test the live kernel hook installation
or prove the texture policy on hardware.

The inherited predicate also read the image-specific surface field before
checking the entry kind. A short non-image entry reproduced an out-of-bounds
read under AddressSanitizer. The kind/load-state checks now precede that read.

## Broad-scan findings and replacement policy

On 7 September the user reported that the last page initially showed artwork,
but returning to earlier pages left them white, and subsequently the last page
also became white. Further scrolling sometimes restored artwork. These are
user observations, not a captured frame trace or timing measurement.

`build/icon-cache-trial/deferred-scan/scroll-log-20260907.txt` contains the two
activation markers plus all 16 permitted `released an unreferenced icon texture`
records. This proves that the relaxed eviction path ran, but the capped log
does not identify total evictions, selected entries, reload results, or timing.

Two policy defects are confirmed by source and firmware analysis:

- The callback releases textures during traversal, so a single allocation
  failure can discard every eligible texture. A regression with three eligible
  textures failed the one-victim assertion before the policy correction.
- `FC4FD91F` starts its best age at zero and uses a signed, strictly-greater
  comparison (`0x83216D96`/`0x83216D98` in the original PAF IDB). If all eligible
  textures have age zero, the callback can free them but the scan returns zero
  because it selected no entry. The allocation caller then gives up instead
  of retrying against the memory just reclaimed.

These defects explain plausible eviction/reload churn and false allocation
failure. They do not establish that every reported stall or white icon has
the same cause. `170E50BE` queues missing surfaces only with remaining retries;
`16F7F4B3` can exhaust those retries after failed loads. The replacement does
not blindly reset failure states or retry budgets.

SceShell already recycles widgets across a three-page window. Distant pages
release their widgets' texture references while persistent icon models retain
their PAF cache handles. The stock PAF eviction predicate also checks the
cache-item reference count, which can prevent reclaiming these unused textures.

The replacement uses the firmware traversal with a non-destructive collector.
It selects the maximum unsigned `clock - last_used` age, including age zero;
equal-age candidates retain deterministic traversal order. Selection and the
one selected surface's normal release are protected by the same recursive
PAF cache mutex used by the getter and allocator. The extra lock remains held
after the firmware traversal drops its own lock. Nested invocations restore
the prior selection context.

Eligible entries must be complete images with exactly one surface reference.
Externally retained entries must also remain registered with positive original
and remaining retry budgets. Active surfaces, non-images, unfinished loads,
absent surfaces, and non-reloadable retained images are excluded. Cache metadata,
source information, handles and retry counters are never changed by eviction.

Success means exactly one texture was released. Empty scans and scans without
an eligible texture return zero, not false progress. The existing allocation
caller retries after each successful eviction and may request another victim
if still necessary. There are no allocations, file writes or formatting calls
in the new collector/release path. Startup diagnostics remain available through
the separate, default-off `LIVEAREA_ICON_CACHE_LOGGING` build option.

## Consumer reload correction

The 7 September LRU comparison separates the remaining failure from the broad
eviction problem: initial navigation works, but revisits still need the
two-page widget-rebinding gesture. The original PAF IDB confirms a specific
false-completion path:

1. Texture-only eviction leaves a valid image handle with no surface, completed
   load state (`item+10 == 2`) and the previous success result (`item+12 == 0`).
2. `sub_83360872` sees completed state and asks for the surface. The native
   `9F7CBA3B` getter calls `170E50BE`, which queues a reload and returns no surface
   while the asynchronous job is pending.
3. The consumer reads the old zero result at `0x83360976`, clears its pending
   binding bookkeeping and returns zero. `sub_8333CC26` removes the pending
   request. Finishing the reload does not restore that abandoned subscription.

The new hook at PAF segment 0 + `0x15F9E2` gates the original consumer. It first
checks the canonical cache-image interface vtable before deriving the containing
item at interface minus 100; unrelated interfaces may have no such container.
Only registered images in the shell icon pool with a zero last result enter
the added readiness check. Genuine nonzero load results retain the original
consumer behavior; retry counters and error results are not rewritten.

The native getter supplies a surface reference or queues the missing-image
reload. Without a surface, the wrapper returns the consumer's existing pending
result (`-1`) before the original consumer can clear any bookkeeping. When a
surface is available, the wrapper retains its reference through the original
consumer call. That pin prevents a concurrent LRU eviction between the readiness
check and the native consumer's own getter. The cache mutex is released before
the original consumer runs its widget callbacks. The extra reference is released
normally afterward. No page rebuild, synthetic failure result, timer, new worker,
per-icon allocation, or hot-path file write is introduced by the wrapper.

The host regression first failed with the actual LRU code followed by a model of
the verified firmware consumer: the pending subscription disappeared while a
reload was queued. With the hook it passes, including 32 eviction/reload cycles,
completion at the unlock boundary, an eviction attempt during the pinned handoff,
cancellation, queue failure, terminal and transient load errors, and unrelated
interface/cache delegation. This is a deterministic host model, not execution
of the complete firmware widget implementation on the host.

## Binary validation

The optional trial supports retail 3.60 only:

- SceShell NID `0x0552F692`; existing 23 patch-site and segment-size checks apply.
- Deferred initializer: SceShell segment 0 + `0x2C74` (six exact bytes and the
  following MOVW opcode/register checked when the bootstrap hook is needed).
- Icon-pool holder pointer: SceShell segment 1 + `0x6DEC`.
- ScePaf NID `0xCD679177`; segment 0 size `0x300D00`.
- Cache scan hook: ScePaf segment 0 + `0x15E6A` (20 entry bytes checked).
- Unhooked eviction predicate: ScePaf segment 0 + `0x13EF6` (74 bytes checked).
- Surface release: ScePaf segment 0 + `0x8997A` (48 bytes checked).
- Cache mutex lock/unlock providers: original IDB `0x83254D20`/`0x83254D34`
  (both full routines, 38 bytes total, checked).
- Timestamp getter: original IDB `0x83213F70` (four bytes checked; field +76).
- Mutex and clock: original IDB `0x8116BA18` and `0x8116BA04`, relative to
  segment 1 at `0x811607E0`. Segment bounds and the actual relocated MOVW/MOVT
  references in the scan are checked before using either address.
- Widget consumer: PAF segment 0 + `0x15F9E2`; all 30 entry bytes are validated,
  including the relocated stack-guard address. The 12-byte taiHEN patch displaces
  14 bytes because a MOVT crosses its boundary. Both relocator passes succeed.
- Surface getter: PAF segment 0 + `0x13344`; all 104 bytes are validated, including
  its relocated reference to the same cache mutex.
- Canonical image-interface vtable: segment 0 + `0x2E106C`; its surface-getter
  slot must point to the thunk at +`0x14672`. All 14 thunk bytes are validated,
  including its interface-minus-100 adjustment and relocated getter target.

Both complete PAF routines were compared with the module downloaded from
`vs0:sys/external/libpaf.suprx` on the test Vita. They match the supplied PAF
analysis database. ARM compilation asserts the field offsets used by the hook.
Unsupported/mismatched PAF modules or a hook-installation failure disable the
optional cache trial while retaining the working shell injections. PAF hooking
uses taiHEN's normal hook chain at the scan entry. The original eviction-entry
hook site remains unsuitable and must not be used.

The current VitaSDK's taiHEN continuation macro needs the pre-C23 meaning of
an unprototyped function pointer, so the target explicitly uses GNU C11.
Diagnostics use `sceClibSnprintf` from the system library: newlib `snprintf`
would pull application-runtime initialization into this `-nostdlib` plugin.

## Build and verify

Current source builds retain the cache correction with
`LIVEAREA_ICON_CACHE_TRIAL=ON` but omit logging by default. The separate
`LIVEAREA_ICON_CACHE_LOGGING=ON` option is only needed for diagnostic builds.
Leaving it off removes diagnostic formatting and file I/O, including file
creation/truncation, without removing the consumer or LRU hooks. Existing log
files are not deleted. Historical deployment logs below came from logging-enabled
builds; the published v1.3.0 asset also still has logging enabled.

```sh
docker run --rm --platform linux/amd64 \
  --user "$(id -u):$(id -g)" -v "$PWD:/work" -w /work \
  livearea-nolimits-vitasdk:ubuntu24.04 \
  cmake -S . -B build/icon-cache-trial/consumer-reload -G Ninja \
  -DLIVEAREA_ICON_CACHE_TRIAL=ON

docker run --rm --platform linux/amd64 \
  --user "$(id -u):$(id -g)" -v "$PWD:/work" -w /work \
  livearea-nolimits-vitasdk:ubuntu24.04 \
  cmake --build build/icon-cache-trial/consumer-reload

python3 tests/run.py
```

`LIVEAREA_TEST_PAF_TEXT=/path/to/ScePaf.text.bin python3 tests/run.py` also
checks actual PAF bytes. To include the actual taiHEN relocator regression:

```sh
LIVEAREA_TEST_PAF_TEXT='/Volumes/data/Downloads/vitadump 3.60 ELF/ScePaf.retail-3.60.text.bin' \
LIVEAREA_TEST_SUBSTITUTE=build/icon-cache-trial/taihen-source/substitute \
python3 tests/run.py \
  0x0552F692 build/firmware-evidence/3.60/SceShell.text.bin \
  0x5549BF1F build/firmware-evidence/3.65/SceShell.text.bin
```

The optional relocator test requires an existing checkout at the pinned
libsubstitute revision above and rejects modifications to its tracked `lib`
sources. Firmware and third-party implementation sources are not bundled.
`tests/test_hook_transform.c` reproduces rejection of the old predicate entry
and acceptance of the replacement scan entry, including the second branch
check used by the hook engine. It also checks the new consumer entry and the
deferred shell initializer using the retail 3.60 inputs above.

Host tests cover firmware and byte rejection, hook failure, retention of the
limit patches when the optional trial fails, argument/result forwarding for
unrelated calls, untouched cache metadata, active-texture protection,
short non-image entries, non-icon-cache delegation, no-victim and empty scans,
no hot-path logging, unavailable startup logging, startup before PAF availability,
one-time deferred installation, relocated address validation, and cleanup.
They also cover oldest-first ordering, age-zero ties, unsigned clock wrap,
exactly one victim per successful scan, protected/non-reloadable entries,
nested selection contexts, lock/scan failures, and repeated eviction cycles.
The policy and consumer suites run both with logging disabled and enabled;
the disabled variant fails on any attempted diagnostic formatting or log-file
open, write or close, including startup and guard-failure paths.
They do not prove device behavior.

On 7 September 2026, the deferred broad-scan build's ASan/UBSan run passed with both retail shell
text dumps, the real retail 3.60 PAF text, and the pinned relocator checks.
Both trial-enabled and baseline ARM builds packaged successfully. The ELF
link retained the existing missing-`_start` warning in both configurations;
the SUPRX conversion completed. The baseline has no trial/hook symbols.
Artifact hashes and the deployment boundary are recorded separately in
`build/icon-cache-trial/deferred-scan-verification.json` so the earlier failed
activation records are not overwritten.

Logging-enabled builds write `ur0:/data/livearea_nolimits-icon-cache-trial.log`,
containing an `active (LRU + consumer reload)` marker or a stage-specific failure
with a hexadecimal detail/error code. It no longer writes per-eviction lines.
Logging code and the file-I/O stub library are included only when both
`LIVEAREA_ICON_CACHE_TRIAL` and `LIVEAREA_ICON_CACHE_LOGGING` are enabled.

The single-victim replacement also passed the ASan/UBSan suite with actual
firmware inputs and the relocator checks. Both build configurations compiled
and packaged; the pre-existing missing-`_start` warning remains. The local
replacement and its proof boundary are recorded in
`build/icon-cache-trial/single-victim-lru-verification.json`. The earlier run
stopped after reporting blocked artifact-verification commands. During the
deployment continuation, SHA-256 succeeded and both upload and post-reboot
readbacks matched the frozen candidate byte-for-byte.

## Previous broad-scan device result

The previously uploaded SUPRX was the superseded broad-scan trial:
`build/icon-cache-trial/deferred-scan/livearea_nolimits.suprx` (7,705 bytes),
SHA-256 `e295980e35b7e9780ea2c0de6733179c014fbcd1864e335198d14453a30ad606`.
Its remote readback matched byte-for-byte before reboot. FTP returned and
`build/icon-cache-trial/deferred-scan/device-boot.log` contains:

```text
icon-cache trial waiting for icon pool
icon-cache trial active (scan hook)
```

The later scroll log confirms eviction, and the user's report above establishes
intermittent recovery but unacceptable scrolling/revisit behavior. The original
remote rollback backup and `ur0:tai/config.txt` were not modified in this
follow-up. That investigation initially ended without deploying its replacement.

## Single-victim deployment: 7 September 2026

The replacement was uploaded to `ur0:/tai/livearea_nolimits.suprx` and read
back before an acknowledged reboot at 02:23:35 CEST. Its 8,304-byte artifact
has SHA-256 `70c444cf761dea5bc5ba753b53612d62eaf7e34eeb04369f14c599ffe7d4c826`.
At 02:26:52 CEST, fresh FTP downloads verified the installed bytes, unchanged
`ur0:tai/config.txt`, and unchanged original rollback backup. The log contains:

```text
icon-cache trial waiting for icon pool
icon-cache trial active (single-victim LRU)
```

The superseded broad-scan trial was also preserved remotely at
`ur0:/tai/livearea_nolimits.before-lru-zQpe75vz.suprx`, with matching readback.
The original known-good limit-patch backup remains
`ur0:/tai/livearea_nolimits.before-icon-cache-20260906.suprx`; it was not replaced.
The temporary `ux0:tai/config.txt` was absent before deployment.
Deployment evidence is in `build/icon-cache-trial/lru-deploy-zQpe75vz/`.

The subsequent user comparison found no noticeable short freezes and complete
artwork while scrolling down, but white pages on revisits still required the
two-page detour. The LRU-only build does not correct consumer false completion.

Before replacing a deployed trial, verify its canonical SUPRX against its
recorded hash and verify the original remote rollback backup. Check whether
the temporary `ux0:tai/config.txt` has reappeared; do not remove a configuration
without first confirming that it is the temporary recovery copy.

For device comparison, preserve the installed SUPRX locally and remotely,
verify upload readback, and reboot to activate the trial. Check the same page
with Limbo, Bermuda, River Raid, Freedom Wars, Persona 4 and Space Cadet, then
navigate away and back. SuperTuxKart's separately missing icon asset is not
expected to be repaired by this change. Remaining intermittent failures after
the LRU replacement would require correlating allocation failures, load completion
errors, retry budgets and widget reattachment instead of assuming eviction alone
is responsible. Increasing the pool is a separate diagnostic alternative, not
part of this bounded-cache change.

## Original consumer candidate and blocked deployment: 7 September 2026

The original consumer-corrected artifact, preserved unchanged, is
`build/icon-cache-trial/consumer-reload/livearea_nolimits.suprx` (9,107 bytes),
SHA-256 `d1e9d019d3ed396df3443dbe33db2ac84386bc2923fb94783c37cd9739ccfe3e`.
The full ASan/UBSan suite, actual firmware-byte validation, all three hook-site
relocation checks and both ARM build configurations passed. The existing
missing-`_start` warning remains at trial ELF link; SUPRX packaging succeeded.
The consumer findings were annotated and saved in the original PAF IDB in place.

Preflight downloaded and verified the installed LRU-only plugin, original
rollback backup and unchanged `ur0:tai/config.txt`. The temporary
`ux0:tai/config.txt` was absent. The current LRU-only plugin was additionally
backed up at `ur0:/tai/livearea_nolimits.before-consumer-fpDI2cp3.suprx`, with
matching downloaded readback. The original rollback backup was not changed.

The earlier upload request targeting `ur0:/tai/livearea_nolimits.suprx` was
reported as rejected by the tool layer; its exact response was not preserved.
No reboot was requested in that attempt. A fresh download after the reported
rejection matched the preflight LRU-only bytes and hash
`70c444cf761dea5bc5ba753b53612d62eaf7e34eeb04369f14c599ffe7d4c826`.
The consumer-corrected candidate was therefore not installed or active at that
earlier boundary. This did not establish that the Vita rejected the binary.

The frozen candidate and preflight/readback evidence are under
`build/icon-cache-trial/consumer-deploy-fpDI2cp3/`. The complete verification
boundary is recorded in `build/icon-cache-trial/consumer-reload-verification.json`;
the failing/passing lifecycle evidence is in `consumer-reload-evidence/` beside it.

## Consumer deployment retry and runtime rejection: 7 September 2026

The exact checkout was resumed on `dev/icon-cache-trial` without creating a
worktree or changing the existing implementation. Both prepared candidate
copies matched the 9,107-byte hash recorded above. Fresh sequential FTP
downloads matched the installed LRU-only build, both rollback backups and the
original `ur0:tai/config.txt`; the `ux0:tai` listing contained only `._config.txt`.

At 11:11:57 CEST, upload returned FTP 226 for 9,107 bytes. A separate download
matched the frozen consumer candidate byte-for-byte. The existing VitaCompanion
command service acknowledged `reboot` at 11:12:36 CEST. At 11:13:07 CEST both
ICMP and a fresh FTP log download succeeded. The new boot log contains:

```text
icon-cache trial waiting for icon pool
icon-cache trial PAF consumer bytes failed: 0x0015F9E2
```

This is runtime validation rejection, not a transfer failure. The logged offset
identifies the combined consumer validation stage; it does not distinguish
the consumer entry, relocated stack-guard reference, getter, thunk or vtable
subcheck. Neither new optional hook is installed when this validation fails.
The existing optional-failure path retains the baseline limit patches; no
on-device count of applied baseline patches was captured.

Post-reboot downloads completed at 11:13:59 CEST and confirmed the installed
consumer hash `d1e9d019d3ed396df3443dbe33db2ac84386bc2923fb94783c37cd9739ccfe3e`,
unchanged original and LRU rollback backups, unchanged active configuration,
and continued absence of the temporary `ux0:tai/config.txt`. Evidence is in
`build/icon-cache-trial/consumer-retry-LLAft6NJ/`. That candidate did not activate;
the subsequent diagnosis and replacement are recorded below.

## Imported stack-guard correction and successful activation

Failure-only startup diagnostics were added without changing the existing
validation predicate or hook behavior. A red regression first demonstrated the
missing diagnostic output; the updated sanitizer/firmware/relocator suite then
passed. The diagnostic artifact was 9,688 bytes, SHA-256
`17451943b73708df76e2ade5a307eb20a30c9b6fa35f9172971f6a44f57fb0c6`.
It was uploaded, downloaded and byte-compared before reboot; the acknowledged
11:24:05 CEST reboot produced this runtime address comparison:

| Checked address | Captured runtime value | Previous expected value |
| --- | --- | --- |
| Consumer stack-guard variable | `0xE001E9F4` | `0xE047AFDC` |
| Getter cache mutex | `0xE014B708` | `0xE014B708` |
| Handle thunk target | `0xE021E645` | `0xE021E645` |
| Canonical vtable getter | `0xE021F973` | `0xE021F973` |

The captured PAF text and data bases were `0xE020B300` and `0xE01404D0`.
The rejected expectation was text plus `0x26FCDC`, taken from the enriched IDB's
`off_83470B6C`. Original-IDB import metadata identifies this as the SceLibKernel
variable import reference-table entry, not a PAF-local runtime object:
the table at `0x83463F34` names library NID `0xCAE9ACE6`, has one variable,
and points to variable NID `0x93B8AA67` at `0x834705D4` and its reference-table
entry at `0x83470B6C`. The matching SDK variable is `__stack_chk_guard`.

The validator now compares the complete consumer MOVW/MOVT pair against the
plugin's own loader-resolved import of that same variable. It still checks all
opcode, register and address bits; the fixed consumer bytes, native getter,
handle thunk and canonical vtable checks are unchanged. No address mask was
widened, and no fixed runtime address was hardcoded. Failure-only diagnostics
remain bounded and outside the eviction/consumer hot paths.

The host fixture previously replaced the import immediate with the incorrect
PAF-relative address before testing it. The new regression supplies a separate
imported guard address, rejects the old placeholder, and independently checks
the MOV encoding captured on the Vita. It failed against the old validator at
`valid_consumer_code` before the production change and passed afterward. The
full ASan/UBSan suite, real retail 3.60/3.65 shell inputs, retail 3.60 PAF guards,
all bit-mutation checks and pinned taiHEN relocator checks passed again.

Linking the guard initially selected newlib's application-runtime definition
because `libc` preceded the system stub. The link now selects SceLibKernel first
and repeats that stub after `libc` for selected C-wrapper dependencies. The
resulting trial ELF contains the guard as an import (`nm` type `N`) and none of
the checked newlib startup/reentrancy symbols. Both trial and default ARM builds
package successfully with GCC 15.2.0; the existing missing-`_start` warning
remains. Trial and default build logs, failed link attempts, import-table
evidence and the regression logs are preserved in
`build/icon-cache-trial/consumer-guard-deploy-TXRAIeVR/`.

The corrected artifact is
`build/icon-cache-trial/consumer-import-guard/livearea_nolimits.suprx`:
9,736 bytes, SHA-256
`9cdbeca2ad349b83a7f9e9dbb96c721fe9af51ef8cb24568a7a4f0dd90199297`.
The original 9,107-byte candidate and diagnostic candidate were retained,
including verified device backups before each replacement.

Fresh preflight verified the installed diagnostic binary, original and LRU
backups, unchanged active configuration and absence of `ux0:tai/config.txt`.
At 11:37:21 CEST, corrected upload returned FTP 226 for 9,736 bytes and the
separate readback matched byte-for-byte. Reboot was acknowledged at 11:37:54.
The first two FTP probes timed out during restart; at 11:38:12 both ICMP and
FTP succeeded, and the new boot log contained:

```text
icon-cache trial waiting for icon pool
icon-cache trial active (LRU + consumer reload)
```

Post-reboot downloads completed at 11:38:14 CEST. The installed plugin matched
the corrected candidate byte-for-byte; original rollback hash `4e23c0ff...`,
LRU rollback hash `70c444cf...` and configuration hash `bdcffa0b...` were
unchanged. The `ux0:tai` listing still contained only `._config.txt`.
Evidence, timestamps, source hashes, readbacks and `deployment.json` are in
`build/icon-cache-trial/consumer-import-deploy-VtQOfJdx/`; the consolidated
record is `build/icon-cache-trial/consumer-reload-verification.json`.

The new import findings were appended to the original PAF IDB at `0x83360878`
and `0x83470B6C`, preserving earlier annotations, and saved successfully in
place without patching firmware bytes. No project changes were committed or
pushed at that validation boundary. The 2 MiB pool and baseline page/count patch
behavior remain unchanged.

The user subsequently tested this exact installed hash and reported that the
plugin works perfectly. In particular, the prior white-icon revisit failure and
the associated navigation problem were no longer observed in that comparison.
This is qualitative hardware validation from the user, not instrumented frame
timing. SuperTuxKart's separately missing icon asset remains unrelated.
