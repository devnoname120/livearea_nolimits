# 500 top-level icons and 50 pages

## Scope

Version 1.4.0 raises the top-level limit from 255 to 500 and the page limit from
26 to 50. Ten icons per page and the independent 1,000 counted-application limit
remain unchanged. Recovery consumes the same constants. No database schema,
folder format, texture-pool size, theme table or icon-cache implementation changes
are part of this extension.

The change is implemented and verified offline for the retail 3.60, retail 3.65
and PTEL 3.60 shell profiles. It has not been installed as a 500-icon candidate or
tested with a 50-page library on hardware. The previous recovery candidate remains
the device-tested build; its successful normal boot is not evidence for 50 pages.
The release enables the existing retail 3.60 icon-cache correction and uses
`CMAKE_BUILD_TYPE=Release` with `LIVEAREA_ICON_CACHE_LOGGING=OFF`.

## Why the count no longer stops at 255

The original shell uses a 16-bit Thumb comparison with an eight-bit immediate.
The register and count are already 32-bit. In the audited admission function,
LSDB returns the ordinary-page count in R0, it is copied into R6, and only the
Boolean `can_add` result is written as a byte. The page collection's count and
individual page index are also 32-bit fields. The inspected database schema uses
integer page IDs, page numbers and icon positions.

A four-byte `CMP.W` cannot be inserted over a two-byte instruction without
accounting for the surrounding code. The implementation therefore replaces two
complete, same-length instruction windows rather than overwriting the following
instruction or adding a new runtime hook.

| Shell profile | Admission block, 22 bytes | Message block, 12 bytes |
| --- | --- | --- |
| Retail 3.60 | `0x0552C6` | `0x063A8E` |
| Retail 3.65 | `0x05531E` | `0x063AE6` |
| PTEL 3.60 | `0x054E86` | `0x06364E` |

Offsets are relative to segment 0. Original bytes for both windows match all
three inspected firmware images. The admission block retains the Boolean result
and live registers while making room for `CMP.W R6,#LIVEAREA_TOP_LEVEL_LIMIT`.
The message block reuses comparison flags across MOVT, which does not overwrite
them, instead of repeating a narrow comparison. Its original external branch
destinations remain unchanged. No statically visible external entry into the
replaced block interiors was found in the inspected IDA references.

`src/main.c` checks every original byte in both windows before any injection.
The three old narrow comparison patches become two block patches, reducing the
shell table from 23 to 22 regions. Expected-byte storage and host rollback buffers
accommodate the larger windows. Assembly uses explicit fixed footprints; attempts
to grow beyond the reserved windows are rejected, and native tests check exact
ELF symbol sizes and behaviour. The assembled patch-byte object has no relocation
records, so its internal branches remain valid when copied into the shell.

The existing eleven page-limit sites receive 50 from `src/limits.h`. The
independent ten-entry appearance-table bound and ten-icons-per-page loops are
not patched. The recovery allocator's existing guard now enforces 500 entries
and its native page creation/search patches use 50 pages.

## Offline verification

`tests/run.py` builds and runs the actual C startup/recovery logic under ASan and
UBSan. It checks all shell profiles, every corrupt expected byte, every partial
installation and rollback, cache-enabled/disabled integration and optional
recovery failures. Host recovery tests cover the existing hidden-app scenario
and filling the layout from 255 through 500 entries, including the final slot,
full rejection, simulated restart and freed-slot reuse. The native recovery
routines are tested separately; the host materialization model is not a complete
execution of the Vita database.

With `LIVEAREA_TEST_PLUGIN_ELF` set, the runner emits a manifest directly from the
compiled production C patch tables and invokes `tests/test_shell_capacity.py`.
The script extracts the actual replacement bytes from the built plugin ELF,
checks the full original regions and non-overlap, and applies them to emulator
memory containing each private shell image. It executes the complete original
and patched admission/message functions, not just a second implementation of
their Boolean logic. External LSDB calls are stubbed; query arguments, errors,
callee-saved registers, stack restoration and the one-byte result are checked.

The shell suite passes 33,942 checks over all three profiles. Coverage includes
counts 0 through 1,100, wider integer values, 255/256 and 499/500 boundaries,
the unchanged 1,000 counted-icon boundary, negative query errors, and each of the
eleven page-limit instructions. Retail 3.60 and 3.65 recovery instruction tests
also pass using the final ELF. Existing cache and taiHEN relocator regressions
remain enabled. Both cache-enabled and limits/recovery-only ARM builds succeed.
The pre-existing raw ELF `_start` linker warning remains; generated module entry
metadata is checked separately against the configured module_start/module_stop.

Example with locally supplied, undistributed firmware:

```sh
LIVEAREA_TEST_PLUGIN_ELF=build/livearea_nolimits \
LIVEAREA_TEST_RECOVERY_360=/path/to/retail360-recovery.text.bin \
LIVEAREA_TEST_RECOVERY_365=/path/to/retail365-recovery.text.bin \
LIVEAREA_TEST_PAF_TEXT=/path/to/ScePaf.retail-3.60.text.bin \
LIVEAREA_TEST_SUBSTITUTE=/path/to/taihen/substitute \
python3 tests/run.py \
  0x0552F692 /path/to/retail360-shell.text.bin \
  0x5549BF1F /path/to/retail365-shell.text.bin \
  0xEAB89D5C /path/to/ptel360-shell.text.bin
```

The optional native tests require Unicorn 2.x. Set `LIVEAREA_TEST_ARM_PYTHON` to a
separate Python interpreter when that dependency is not installed in the ordinary
host-test environment. The substitute revision is pinned as described in the
[cache notes](icon-cache-trial.md). No firmware or third-party implementation
sources are included in the repository.

## Hardware acceptance

Treat crossing 255 and scaling to 50 pages as separate experiments. A first trial
at 260 entries/26 pages isolates integer-width behaviour; a subsequent 500/50
trial exercises the larger page range. Those trials require deliberately built
configurations and backups, not editing a live database to force a count.

With a backed-up representative library, validate cold boot and a second reboot,
installation at 499/500/501 entries, deletion/reinstallation, page creation and
removal, moves between pages 48 and 49, folder moves and application launches.
Repeatedly scroll from the first to last page and back while checking artwork,
responsiveness and memory use. Preserve the database layout and compare it after
restarting. Also test recovery of applications hidden before plugin activation.

Integer encoding is no longer the known obstacle. UI scheduling, metadata growth
and texture-cache behaviour at this scale remain hardware-validation risks.
Version 1.5.0 supplies the cache correction for every supported shell profile;
see [cache profiles](cache-profiles.md). Firmware-byte and host validation do not
establish equivalent large-library rendering stability on untested hardware.
A separate byte value 255 inspected in the earlier `sub_835713F8` candidate is
used while walking a bounded tag/length byte buffer, not the LSDB icon count;
that code has not been patched. This is not an exhaustive claim about every
reserved layout field or unrelated firmware path.
