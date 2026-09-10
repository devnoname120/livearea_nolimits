# Testing v1.8.0-rc1 and collecting diagnostics

This prerelease keeps 50 pages, 500 top-level icons, 1,000 counted application/content
icons, hidden-application recovery, and the icon-cache correction. It changes the
recovery interception and enables logging so failures can be investigated.
It has not yet been confirmed on affected hardware. Please test the operation
requested in your issue and report results there, including successful results.

## Install and test

1. Back up the current plugin, active taiHEN configuration, and
   `ur0:shell/db/app.db` before replacing anything.
2. Extract `livearea_nolimits.suprx` from the diagnostic ZIP and replace the file
   referenced under `*main`. Keep other plugins and the theme unchanged for the
   first test. Fully reboot; standby does not load the replacement.
3. Perform the focused test requested in your issue. For a freeze or crash,
   record which operation failed, the error code if shown, and roughly how long
   after boot it occurred. Avoid repeatedly provoking the same failure.
4. Copy the logs below before another normal boot. Each boot rotates the current
   log to the previous log. Send both if present, even when the test succeeds.

An expanded database may require the expanded-limit plugin to boot. Holding L
only skips plugins; it does not make that database compatible with stock limits.
For rollback, use a compatible plugin/database backup pair and your existing
recovery access. A database rebuild is not a prerequisite for this test.

## Files to attach

ZIP these files from `ur0:/data/`:

- `livearea_nolimits-debug.log`
- `livearea_nolimits-debug.previous.log`, if present

If they are absent, check the same filenames under `ux0:/data/`. Say explicitly
if neither location contains logs; that can mean the plugin did not load or
neither log destination could be opened.

Include the actual firmware version, Vita/PSTV model, Enso version, previous
plugin version, and the operation/results. If a crash produced a new
`psp2core-*` dump, attach it as well if you are comfortable sharing it publicly;
core dumps can contain application data. A photo of an error is useful when
screenshot capture itself fails. Configuration and database files may be
requested separately when the logs point to installation or hidden-app state.

## What the log establishes

The first line identifies `build=v1.8.0-rc1`, the selected log path, and the sync
interval. Lines include a sequence number, monotonic process time, and thread ID.
Startup records shell/PAF/recovery identities, memory ranges, limits, validation
results, and hook addresses. Recovery records each injection, the ready callback,
its native entry/return, and patch removal during module stop. Mismatches include
the expected and actual bytes. Cache activity is sampled and includes counters.

`ready-hook` means installation succeeded. `ready-enter` means the callback ran.
`module-patched` means all recovery patches were installed. Native entry/return
and module-stop events show how far execution progressed; the restored app count
must still be confirmed by the reporter. A negative installation result with
`run_native=1 patched=0` means native recovery continued without the extension.

The logger synchronizes every eight lines and explicitly flushes at recovery
boundaries. It avoids waiting on a contended logging lock; a subsequent line
reports `dropped=` if contention lost events. Short writes are completed where
possible, and later lines report `write_failures=` after storage errors. A hard
failure can still lose the last unsynchronized lines. Logging adds overhead, so
this candidate is for diagnosis, not a performance benchmark.

## Reproduce the diagnostic build

Use the SDK image described in the README. Configure with:

```sh
cmake -S . -B build/diagnostic -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLIVEAREA_ICON_CACHE_TRIAL=ON \
  -DLIVEAREA_ICON_CACHE_LOGGING=OFF \
  -DLIVEAREA_DEBUG_LOGGING=ON \
  -DLIVEAREA_DEBUG_BUILD_ID=v1.8.0-rc1 \
  -DLIVEAREA_DEBUG_SYNC_INTERVAL=8
cmake --build build/diagnostic
```

The target retains `-O2`; `RelWithDebInfo` also preserves symbols in the local ELF.
The release's symbols archive contains only this project's ELF and symbol map,
not firmware images or user data. Keep the ELF matching the SUPRX hash when
analyzing core dumps.
