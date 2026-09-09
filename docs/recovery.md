# Recovery of applications hidden by the old limit

## Scope and status

The recovery extension is enabled in source builds independently of the optional
icon-cache correction. It extends the existing `SceDbRecovery` algorithm; it does
not rebuild `app.db`, reinstall applications, suppress the warning, or introduce
a second database implementation. Runtime logging remains disabled by default.

The implementation has passed host sanitizer tests, checks using both retail
recovery text images, and execution of the relevant original ARM functions with
the assembled replacements. Retail 3.65 hardware testing restored 73 applications
from the hidden page into a 500-visible-application library, producing 573 visible
applications across 15 pages. The same test completed with the icon-cache
correction enabled; scrolling, edit mode, idle, and repeated sleep/wake all worked.

Version 1.7.0 removes the recovery allocator-entry hook. Hardware tracing showed
that even a pure-passthrough hook at that pre-start address causes a shutdown,
whereas the same seven direct recovery patches complete successfully without it.
The preceding v1.6.0 guard-validation error and the unsafe hook timing are detailed
below.

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
matching recovery start, the plugin validates the loaded module and all seven
patch sites, installs those direct patches, then invokes the native start. It does
not hook the recovery allocator. Unrelated modules and unsupported identities
pass through unchanged. Reentrant target lifecycle operations are rejected rather
than allowing concurrent patch installation or removal.

Successful native stop removes the patches while recovery memory is still mapped.
A failed stop leaves them installed. Partial installation rolls back; if rollback
itself fails, the partially modified module is not started or unloaded. The plugin
refuses its own unload while patched recovery is running or cleanup cannot complete.
Failed native starts are cleaned up only after confirming that the module remains
mapped; an unexpected disappearance pins the extension rather than restoring bytes
into unmapped memory.

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
notes. Its relocator tests cover the remaining shell and PAF function hooks; the
recovery extension no longer contains a function-entry hook.

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
failed native start/stop/unload, re-entry, cleanup, repeated recovery starts, and
the fact that allocator-entry changes no longer affect validation. These models
are not an execution of the entire Vita database, scheduler, and UI.

See [the 500-icon capacity notes](top-level-capacity.md) for the combined native
shell and recovery verification against the final assembled plugin.

## Hardware coverage and rollback

Retail 3.65 recovery is validated on an affected device at 500 visible plus 73
hidden applications. The database update completed, all 73 applications appeared
across 15 pages, and the recovery module stopped and rolled back its temporary
patches cleanly. Equivalent affected-library recovery remains untested on retail
3.60 and PTEL 3.60 hardware.

Keep backups of the active plugin, configuration, and a stock-compatible layout
database. Disabling the plugin does not shrink an already-expanded `app.db`; a
stock shell can fail to boot when that database contains pages or counts beyond
its native limits. Holding L disables plugins but does not make such a database
stock-compatible. Restore a compatible backup at the same time as disabling the
plugin, or use Safe Mode database rebuild as the destructive fallback.

At the real configured counted-icon or top-level capacity, applications can still
remain hidden. A persistent warning at those boundaries is not the old 500-limit
bug, and this extension intentionally does not hide it.
