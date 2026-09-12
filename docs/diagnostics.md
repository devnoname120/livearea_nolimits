# Collecting diagnostics

Regular releases have runtime logging disabled. Use a diagnostic ZIP from the
same [release](https://github.com/devnoname120/livearea_nolimits/releases) as the
version being investigated. The normal diagnostic enables logging and keeps
icon-cache management enabled. The no-cache diagnostic disables only icon-cache
management; use it when requested for a comparison in your issue.

Both diagnostic variants retain the release's application and page limits and
hidden-application recovery. Logs left by another build do not describe a later
session with logging disabled. Keep the matching build ID, SUPRX and symbols
when investigating a failure.

## Install and test

1. Back up the current plugin, active taiHEN configuration, and
   `ur0:shell/db/app.db` before replacing anything.
2. Extract `livearea_nolimits.suprx` from the diagnostic ZIP and replace the file
   referenced under `*main`. Keep other plugins and the theme unchanged for the
   comparison. Fully reboot; standby does not load the replacement.
3. Perform the focused test requested in your issue. For a freeze or crash,
   record which operation failed, the error code if shown, and roughly how long
   after boot it occurred. One failed attempt is enough.
4. Copy the logs below before another normal boot. Each boot rotates the current
   log to the previous log. Send both if present, even when the test succeeds.

Keep a restored layout. Do not rebuild the database or deliberately hide apps
just to reproduce recovery. An expanded database may require the expanded-limit
plugin to boot. Holding L skips plugins but does not make that database compatible
with the firmware's normal limits. For rollback, use a compatible plugin/database
backup pair and your existing recovery access.

## Files to attach

ZIP these files from `ur0:/data/`:

- `livearea_nolimits-debug.log`
- `livearea_nolimits-debug.previous.log`, if present

If they are absent, check the same filenames under `ux0:/data/`. Say explicitly
if neither location contains logs; that can mean the plugin did not load or
neither log destination could be opened.

Include the actual firmware version, Vita/PSTV model, Enso version, plugin build
ID, and the operation/results. If a crash produced a completed `psp2core-*` dump,
attach it as well if you are comfortable sharing it publicly; core dumps can
contain application data. Configuration and database files may be requested
separately when logs point to installation or hidden-app state.

## What the log establishes

The first line identifies the build, selected log path and synchronization
interval. Lines include a sequence number, monotonic process time and thread ID.
Startup records firmware identities, memory ranges, configured limits and
validation results. Recovery records module preload, patching before
initialization, initializer entry/return, the original completion callback and
patch removal during module stop. Cache activity is sampled and includes counters.

`load-call-installed` means the SceShell interception was installed;
`preload-prepared patched=1 before-initializer=1` records early patching.
`init-return flags=0x00080000` indicates the hidden-app recovery decision for the
validated normal-boot cases. Native callback and module-stop events show how far
execution progressed. These events do not count restored apps: report the
visible layout and whether it persists after reboot as well.

The cache's `shell-detours-installed` event precedes `active`; only the latter
confirms that the private pool/image interception is active. `paf_hooks=0` and
`text_unchanged=1` record that shared PAF executable code was left untouched.

Diagnostic builds synchronize each line by default and explicitly flush at
recovery boundaries. Logging avoids waiting on a contended lock; a subsequent
line reports `dropped=` if contention lost events. Short writes are completed
where possible, and later lines report `write_failures=` after storage errors.
Logging adds overhead, so use the regular SUPRX for ordinary use once the
investigation is complete.

## Build a matching diagnostic

Check out the source tag for the release being investigated. Use the SDK image
described in the README and a distinct build ID. Configure with:

```sh
cmake -S . -B build/diagnostic -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLIVEAREA_ICON_CACHE_TRIAL=ON \
  -DLIVEAREA_ICON_CACHE_LOGGING=OFF \
  -DLIVEAREA_DEBUG_LOGGING=ON \
  -DLIVEAREA_DEBUG_BUILD_ID=local-diagnostic \
  -DLIVEAREA_DEBUG_SYNC_INTERVAL=1
cmake --build build/diagnostic
```

Set `LIVEAREA_ICON_CACHE_TRIAL=OFF` for the no-cache comparison. The target uses
`-O2`; `RelWithDebInfo` also preserves symbols in the local ELF. The symbols
archive contains this project's binaries and symbol maps, with a separate folder
for each build variant. It contains no firmware images or user data. Use the
ELF whose recorded SUPRX hash matches the installed diagnostic.
