# Recovery of applications hidden by the old limit

## Scope and status

The recovery extension is enabled in source builds independently of the optional
icon-cache correction. It extends the existing `SceDbRecovery` algorithm; it does
not rebuild `app.db`, reinstall applications, suppress the warning, or introduce
a second database implementation. Runtime logging remains disabled by default.

The v1.9.1-rc1 candidate fixes the initialization-order regression in v1.8.0
and v1.9.0. It preloads `SceDbRecovery` at SceShell's specific recovery-load call,
installs the seven capacity patches after module start/relocation but before PAF
constructs and initializes the Plugin, and then resumes normal asynchronous loading.
The later SceShell ready callback is called directly, with its correct void ABI.
Its entry is no longer hooked and no substitute continuation is involved.

This candidate is for reporter validation. Offline tests reproduce both supplied
500-visible/49-hidden and 500-visible/73-hidden recovery decisions and validate
module-reference/patch cleanup. They do not execute the full database/UI/scheduler
or establish that the reported pre-LiveArea freeze in #8 is fixed. No physical
Vita testing was performed for this candidate. The existing v1.9.0 stable release
and its per-instance icon-cache implementation are otherwise unchanged.

The earlier shared-library hooks in v1.7.0 applied limits early enough but exposed
other processes to SceShell-only callbacks. v1.8.0 removed those hooks, but its
replacement ran after the initializer had stored its recovery decision. Late
patching cannot update that decision. The historical allocator-hook problem is
documented below; neither form of shared hook is restored here.

## Why installation order matters

The normal `SceShell` insertion path and boot recovery have separate limits.
Earlier plugin builds patched the shell but left the recovery module unchanged.
An application added after plugin activation can therefore be placed normally,
whereas an application already parked on the hidden page can remain invisible.

The hidden page has page number `-100000000`. Three recovery planning paths bound
the hidden count by `500 - displayed_count`, and two materialization paths still
stop at 500. With 500 displayed and two hidden applications, the planner returns
zero. The patched shell's warning changes, but restarting the unchanged recovery
module cannot restore those two applications.

The extension uses `LIVEAREA_ICON_LIMIT` in all five places. The existing arithmetic
is retained: for a valid displayed count between zero and the configured limit,
recovery schedules `min(hidden_count, LIVEAREA_ICON_LIMIT - displayed_count)`.
Negative LSDB query results continue through the native error paths. This is not
a repair tool for externally corrupted layouts or counts already beyond the
configured capacity.

## Binary profiles

`src/recovery.c` selects a recovery identity paired with the actual shell identity.
No firmware-version API or spoofed version string is used.

| Shell | Shell NID | Recovery NID |
| --- | --- | --- |
| Retail 3.60 | `0x0552F692` | `0xC1F30F67` |
| Retail 3.65 | `0x5549BF1F` | `0x3F76E38F` |
| PTEL 3.60 | `0xEAB89D5C` | `0xC1F30F67` |

The inspected PTEL dump uses the same recovery module identity and instruction
locations as retail 3.60. Its text load address is `0x811605E0`, not the IDB image
base. Both retail profiles have segment 0 size `0x3E9E8` and segment 1 size
`0x3094`. The following locations are relative to recovery segment 0, not SceShell.

| Offset | Original instruction | Replacement |
| --- | --- | --- |
| `0x06280` | `CMP.W R0, #500` | Compare with configured counted-icon limit |
| `0x07054` | `CMP.W R0, #500` | Compare with configured counted-icon limit |
| `0x0C700` | `RSBS.W R1, R0, #500` | Subtract from configured counted-icon limit |
| `0x0C776` | `RSBS.W R1, R0, #500` | Subtract from configured counted-icon limit |
| `0x0C834` | `RSBS.W R1, R0, #500` | Subtract from configured counted-icon limit |
| `0x06084` | `CMP R4, #10` | Compare with configured page limit |
| `0x060FE` | `MOVS.W R9, #9` | Start backward search at configured last page |

All original bytes are verified before the first recovery modification. The
replacement bytes are assembled from `src/limits.h`, not hard-coded encodings.
The native ten-icons-per-page loops and ten-entry appearance table are unchanged.

No function-entry hook is installed in the recovery allocator. The configured
top-level limit is exactly the physical page capacity:

```text
50 pages * 10 icons per page = 500 top-level icons
```

`src/limits.h` enforces that equality. The native allocator therefore cannot
place more top-level entries than the configured shell limit once the direct page
limit patches are active. A source configuration with a lower top-level limit than
its page capacity is intentionally rejected rather than relying on the unsafe
pre-start hook.

### Why the allocator hook was removed

The allocator begins at segment-0 offset `0x5F50`. In both inspected retail
recovery images, its imported `__stack_chk_guard` operand is finalized by loader
relocations targeting the MOVW at `0x5F56` and MOVT at `0x5F5A`. taiHEN installs
an eight-byte Thumb entry jump over `0x5F50` through `0x5F57`, so the MOVW
relocation overlaps the last two bytes of that jump.

The hook was installed while intercepting module start, before those import
relocations reached their final state. Static substitute tests could relocate a
stable snapshot, but they did not model the loader subsequently rewriting bytes
inside the installed jump and its copied continuation prologue.

The hardware sequence isolated this boundary:

- r3 installed only the seven direct recovery patches and restored all 73 hidden
  applications without an OS crash;
- r4 added a pure-passthrough allocator hook with no logging, LSDB calls, or
  placement policy in its body, and the Vita powered off immediately;
- r5 removed the allocator hook, retained the seven patches, re-enabled the
  icon-cache correction, and again completed recovery and normal LiveArea use.

Version 1.6.0 also compared the pre-start operand with this plugin's own imported
guard address. The hardware trace showed the operand still pointed to the recovery
module's relocation reference slot at `text + 0x2DA40`, so that comparison rejected
the module before any recovery patch was installed. Version 1.7.0 no longer treats
the unrelated allocator prologue as a prerequisite for the seven direct patches.

The [issue #2 reproduction](https://github.com/devnoname120/livearea_nolimits/issues/2#issuecomment-5574338737)
provides a database with 510 visible application rows, one hidden row, 52 folder
icons and 96 ordinary-page entries. SQLite integrity and foreign-key checks pass.
The video shows a database update followed by shutdown; it does not identify a
faulting instruction. With counts obtained using the inspected LSDB SQL, the
original ARM recovery planner returns `min(1, 500 - 510) = -10`; the patched
planner returns `1`.

A second affected-device database contained 500 visible and 73 hidden
applications. Its original planner budget was zero and its patched budget was 73.
The [retail 3.65 r5 test](https://github.com/devnoname120/livearea_nolimits/issues/2#issuecomment-5609052347)
materialized all 73 applications and completed normal shell use. The uploaded
databases, videos, and diagnostic logs remain local test inputs, not repository
assets.

## Module lifecycle

The only permanent recovery interception is a four-byte `BL` replacement at
SceShell text offset `0xC1E`. Startup validates the complete surrounding 16-byte
call sequence, including the relocated local finish-callback address and original
call instruction. Other PAF plugin-load calls are not intercepted. The wrapper
also checks the exact plugin name, module filename, interface version, option and
finish callback before taking ownership.

| Shell | Original LoadAsync import offset | Original BLX bytes |
| --- | --- | --- |
| Retail 3.60 | `0x45CA50` | `5B F0 18 E7` |
| Retail 3.65 | `0x45CE98` | `5C F0 3C E1` |
| PTEL 3.60 | `0x452C08` | `51 F0 F4 E7` |

The original SceShell import stub is left intact and called normally. PAF Module
acquire/release/interface helpers are selected by the matched PAF NID and checked
native instruction sequences. The wrapper mirrors PAF's effective module option,
including its common-dialog mode bit. It declines an untracked preexisting
recovery module because initialization timing cannot be established for that state.

The preload starts the module and registers its interface; it does not run the
Plugin initializer. PAF's filename cache lets the subsequent native Plugin load
reuse that ModuleImpl, taking its own reference. The preload stays alive through
the original finish callback, then its extra reference is released. Failed
preloads are released before native LoadAsync retries, so an unsuccessful cached
ModuleImpl cannot be kept alive by the wrapper. Duplicate pending loads share the
retained reference until their callbacks complete. Preparing/releasing/blocked
states reject unsafe reentry. An unexpected owner mismatch retains the preload
rather than stopping an unverified owner.

Before modifying the recovery module, validate its UID/NID, both segments, all
seven original patch sites, module-stop body and relocated five-entry module
interface. The interface returned by PAF must match that module's data segment.
This is the Module interface; the Plugin has a separate interface table at object
`+0x58`, which the native initializer populates for the later SceShell callback.
The high shared-address window is rejected for recovery code/data. A private
module-stop redirect restores the temporary patches while the module is
still mapped, then calls its original stop. No lifecycle mutex is held across
native module construction/destruction, load requests or client callbacks.

Diagnostic builds also redirect the initializer pointer in the private module's
interface table at data offset `+4`. The observer logs entry/return and the stored
action flags, then delegates to the original initializer at text `+0x9A`. This
uses the native callback table, with no executable-entry trampoline. The pointer
is restored with the other temporary modifications before module stop. It is
compiled out when logging is disabled.

Partial installation rolls back. If rollback fails, the preload and callback code
remain retained and Plugin initialization is blocked; a partly patched recovery
module must not execute. A live call site or retained module prevents hot unload.
Switch builds by full reboot. These optional recovery errors never remove the
baseline shell capacity patches or the independent icon-cache correction.

Logs distinguish `preload-enter/return`, successful preparation before the
initializer, `init-enter/return`, `load-finished`, `finish-native-enter/return`,
preload release and module-stop cleanup. The initializer's restoration flag is
`0x80000` for the validated normal-boot test cases with hidden apps and remaining
capacity. Flags and successful callbacks are not a restored-app count; reporter
confirmation of the actual resulting layout remains necessary.

## Repeating offline verification

Run the ordinary host suite:

```sh
python3 tests/run.py
```

Add local firmware inputs without distributing them:

```sh
LIVEAREA_TEST_RECOVERY_360=/path/to/retail360-recovery.text.bin \
LIVEAREA_TEST_RECOVERY_365=/path/to/retail365-recovery.text.bin \
LIVEAREA_TEST_PAF_TEXT=/path/to/ScePaf.retail-3.60.text.bin \
LIVEAREA_TEST_SUBSTITUTE=/path/to/taihen/substitute \
python3 tests/run.py \
  0x0552F692 /path/to/retail360-shell.text.bin \
  0x5549BF1F /path/to/retail365-shell.text.bin \
  0xEAB89D5C /path/to/ptel360-shell.text.bin
```

The substitute checkout must be at the revision documented in the icon-cache
notes. Its relocator tests are historical cache-policy comparisons. Current cache and
recovery code do not install shared PAF or SceLibKernel entry hooks. Current recovery
call dispatch and its stop redirect are checked with compiled ARM code.

Host tests use placeholder replacement bytes, as the existing shell tests do.
The separate optional instruction test requires Unicorn 2.x and an unstripped
built plugin ELF. It reads the actual assembled replacements from ELF symbols,
applies them to private firmware copies in emulator memory, and executes the
three native recovery-planning functions plus both admission branches and the
page-limit instructions. Only the external LSDB calls are stubbed.

```sh
python3 tests/test_recovery_firmware.py \
  build/livearea_nolimits \
  /path/to/retail360-recovery.text.bin \
  /path/to/retail365-recovery.text.bin
```

Optionally append `--database /path/to/copied-app.db` to derive planning inputs
from a saved database using the inspected LSDB count queries. This opens the
copy read-only, reports only counts, and verifies that its hash is unchanged.
The instruction test also verifies that the allocator guard-import relocation
overlaps the old eight-byte pre-start hook range, documenting why that hook must
not return.

The 500-visible/two-hidden regression is exercised with original and patched ARM
instructions. Host tests cover every direct-patch byte, partial installation,
failed stop and cleanup, re-entry, repeated recovery cycles, and plugin unload
during an unpatched native callback. Diagnostic assertions cover both success
and failure paths, including retained patches after failed rollback. These models
are not an execution of the entire Vita database, scheduler, and UI.

See [the 500-icon capacity notes](top-level-capacity.md) for the combined native
shell and recovery verification against the final assembled plugin.

## Hardware coverage and rollback

**Historical v1.7.0 evidence:** retail 3.65 recovery was tested at 500 visible plus 73
hidden applications. The database update completed, all 73 applications appeared
across 15 pages, and the recovery module stopped and rolled back its temporary
patches cleanly. Equivalent affected-library recovery remains untested on retail
3.60 and PTEL 3.60 hardware. The v1.9.1-rc1 preload implementation requires new
reporter confirmation on affected devices; these older results do not carry over.

Keep backups of the active plugin, configuration, and a stock-compatible layout
database. Disabling the plugin does not shrink an already-expanded `app.db`; a
stock shell can fail to boot when that database contains pages or counts beyond
its native limits. Holding L disables plugins but does not make such a database
stock-compatible. Restore a compatible backup at the same time as disabling the
plugin, or use Safe Mode database rebuild as the destructive fallback.

At the real configured counted-icon or top-level capacity, applications can still
remain hidden. A persistent warning at those boundaries is not the old 500-limit
bug, and this extension intentionally does not hide it.

## Recovery candidate instruction test

`tests/test_recovery_preload_arm.py PLUGIN_ELF EVIDENCE_ROOT` executes the compiled
call-site wrapper, native PAF Module acquire/reuse/release code and interface-copy
sequence, initializer observer and original SceShell ready callback. The native
recovery initializer and completion-registration routine run in a separate Unicorn
context using the exact capacity bytes installed by the compiled wrapper. This
avoids overlapping link-address spaces while preserving the tested patch timing.
OS, strings/maps, asynchronous scheduling, environment lookup and UI services are
modeled. Passing this test is not a full-system boot test.

The evidence root is the main checkout's local `build/` directory containing the
private firmware inputs used by the existing profile tests. `--database` can be
repeated to derive visible/hidden counts from supplied read-only database copies.
Both reporter count cases are included by default. Failure cases cover start or
interface failure, individual patch/observer failures, incomplete rollback and
failed Plugin load, as well as successful cleanup without retained ModuleImpls.
