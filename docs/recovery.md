# Recovery of applications hidden by the old limit

## Scope and status

The recovery extension is enabled in source builds independently of the optional
icon-cache correction. It extends the existing `SceDbRecovery` algorithm; it does
not rebuild `app.db`, reinstall applications, suppress the warning, or introduce
a second database implementation. Runtime logging remains disabled by default.

The implementation has passed host sanitizer tests, tests using both retail
recovery text images, execution of the relevant original ARM functions with the
assembled replacements, and taiHEN prologue relocation checks. Both ARM build
configurations have been built. These are offline results: restoration on a Vita
with a pre-existing hidden application library has not yet been verified.

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

The allocator at `0x5F50` is wrapped to count existing top-level entries across
pages `0` through `LIVEAREA_PAGE_LIMIT - 1`. At the configured top-level limit it
returns the native hidden-page sentinel without allocating another slot. This
keeps recovery aligned with the current 500-entry/50-page shell configuration,
including when a custom top-level limit is below the total page-slot capacity.
Folder contents do not contribute to this
ordinary-page count. Count-query errors propagate without changing output slots.

The allocator's first 24 bytes are checked, including the register/opcode bits of
its relocated MOVW/MOVT pair and the resulting word-aligned address inside text.
The absolute stack-guard import-slot address is not compared to a fixed dump base.
The real taiHEN transformer requires an eight-byte jump here and preserves ten
bytes of displaced instructions; both relocator passes are tested.

## Module lifecycle

The inspected PAF loader calls load, start, stop and unload separately. The plugin
hooks the raw module-manager imports in `SceLibKernel`, library `0xEAED1616`:

| Import | NID |
| --- | --- |
| `_sceKernelStartModule` | `0x72CD301F` |
| `_sceKernelStopModule` | `0x086867A8` |
| `_sceKernelUnloadModule` | `0x8E4A7716` |

The raw start/stop fourth argument points to `{flags, option, result, reserved}`;
it is not the public six-argument ABI. Arguments and native results are forwarded.
A local result slot is supplied only when the caller did not provide one.

Unload and stop interception are registered before start interception. At a
matching recovery start, the plugin validates the loaded module, resolves the
LSDB top-level-count function, installs the allocator guard and the seven patches,
then invokes the native start. Unrelated modules and unsupported identities pass
through unchanged. Reentrant target lifecycle operations are rejected rather than
allowing concurrent patch installation or removal.

Successful native stop removes the recovery hook and patches while its memory is
still mapped. A failed stop leaves them installed. Partial installation rolls back;
if rollback itself fails, the partially modified module is not started or unloaded.
The plugin refuses its own unload while patched recovery is running or cleanup
cannot complete. Failed native starts are cleaned up only after confirming that
the module remains mapped; an unexpected disappearance pins the extension rather
than restoring bytes into unmapped memory.

An unavailable recovery extension does not remove the already-working SceShell
page/count patches or the independent icon-cache correction. Validation failures
leave native recovery unchanged after successful rollback. They can therefore
leave the original missing-app symptom, rather than risking an unsupported patch.

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
notes. With recovery inputs, the optional relocator test also checks the recovery
allocator prologue at two aligned text bases.

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

The 500-visible/two-hidden regression is exercised with original and patched ARM
instructions. Host tests separately model materialization onto a new eleventh
page, unchanged existing pages, repeated boot without duplicate insertion, total
and top-level boundaries, query errors, every partial installation, failed native
start/stop/unload, re-entry and cleanup failures. These models are not an execution
of the entire Vita database, scheduler and UI.

The expanded-capacity regression additionally restores from 255 through 500
top-level entries, fills the final slot of page 49, rejects a 501st entry, retains
the full layout over a simulated module restart, and reuses a freed final slot.
See [the 500-icon capacity notes](top-level-capacity.md) for combined native shell
and recovery verification against the final assembled plugin.

## Device acceptance still required

Preserve a backup of the active plugin, configuration and layout database before
an on-device trial. Activate the corrected SUPRX with a full reboot, not standby.
For an affected library, confirm that the existing 500 visible and two hidden
applications become 502 visible without reinstalling applications or manually
rebuilding the database. Confirm folders and positions are preserved, launches
work, and a second reboot introduces neither duplicates nor renewed hiding.

At the real configured counted-icon or top-level capacity, applications can still
remain hidden. A persistent warning at those boundaries is not the old 500-limit
bug, and this extension intentionally does not hide it.
