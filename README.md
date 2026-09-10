# LiveArea NoLimits

**[Download v1.8.0](https://github.com/devnoname120/livearea_nolimits/releases/tag/v1.8.0).**
This regular release disables runtime logging and replaces the shared-library
recovery hooks implicated in VitaShell/application crashes. It retains hidden-app
recovery and the icon-cache correction, and includes the 4,000 counted-icon limit.
Upgrade from v1.7.0 or earlier by replacing the existing SUPRX and fully rebooting.

Reporters using rc1 confirmed improvements to VitaShell rename/edit, delayed
crashes on one setup, and PSTV page creation after correcting the configuration.
Remaining PSP/PSX launch and system-hang reports are still being investigated in
[#6](https://github.com/devnoname120/livearea_nolimits/issues/6). Actual hidden-app
restoration with the replacement recovery callback and the full capacity remain
unconfirmed on affected hardware. See [diagnostics](docs/diagnostics.md) if you are
already following a test request in an issue.

`livearea_nolimits.suprx` is a taiHEN user plugin for the PlayStation Vita retail
FW 3.60 and FW 3.65 `SceShell`, with a separate PTEL/testkit 3.60 profile.
It changes the home-screen limits to:

- 50 pages;
- 10 top-level icons per page, unchanged from the firmware;
- 500 top-level icons in total;
- 4,000 counted application/content icons instead of 500.

The 4,000 counted-icon limit is included in v1.8.0. The already-published
v1.8.0-rc1 diagnostic remains at 1,000; its assets are unchanged.

The plugin validates every original instruction before applying any injection.
It selects a patch profile by the loaded `SceShell` module NID and verifies its
text-segment size and the expected code at every patch site. HENkaku version
spoofing can remain enabled: the plugin does not use the system-version API. This
protects unsupported firmware versions and already-modified shells from blind
writes. Version 1.8.0 is built in Release mode with runtime logging disabled,
including when the icon-cache correction is enabled.

Version 1.7.0 changed hidden-application recovery by retaining the seven validated
`SceDbRecovery` instruction patches while removing the unsafe pre-start allocator
hook. On retail 3.65, the release candidate restored 73 hidden applications into
a 500-visible-application library, producing 573 visible applications across 15
pages. This historical result does not validate v1.8.0's replacement callback or
resolve the separate shared-hook defect in v1.7.0. See
[the recovery notes](docs/recovery.md).

The feature parity introduced in v1.5.0 is retained on every supported profile:

| Feature | Retail 3.60 | Retail 3.65 | PTEL 3.60 |
| --- | --- | --- | --- |
| 500 top-level icons / 50 pages | Yes | Yes | Yes |
| 4,000 counted application/content icons | Yes | Yes | Yes |
| Hidden-application recovery extension | Yes | Yes | Yes |
| LRU icon-cache eviction and artwork reload | Yes | Yes | Yes |

All runtime patches remain conditional on successful module and code validation.
The cache profiles were verified against the corresponding firmware binaries and
regression tests. Retail 3.65 now also has the affected-library hardware run above;
PTEL does not. See [cache profile validation](docs/cache-profiles.md) for identities,
offsets, and the distinction between implemented feature parity and coverage at
the absolute 500-top-level-icon/50-page boundary.

Pages after the original first ten use the firmware's default page appearance.
The plugin intentionally leaves the ten-entry custom theme/layout table bounds
unchanged so that extra pages cannot read beyond that table.

Version 1.4.0 replaced the previous 255-icon/26-page limits with wider, same-size
instruction blocks. The count was already 32-bit; no database-format change is
required. Retail 3.65 has now run 573 visible applications across 15 pages, but
500 top-level icons on all 50 pages remain untested. See
[the capacity implementation and validation notes](docs/top-level-capacity.md).

Version 1.2 corrects the firmware profiles in 1.0/1.1: the original reference
was PTEL 3.60, and the profile previously labeled 3.65 was actually retail 3.60.
The retail 3.65 profile is now mapped against an identified 3.65 update image.
See [firmware validation](docs/firmware-validation.md) for the binary identities
and verification details.

Version 1.3 adds the retail 3.60 icon-cache correction used by the GitHub release
binary. It keeps texture residency bounded with single-victim LRU eviction and
preserves pending widget requests while evicted artwork reloads. The correction
validates the relevant SceShell/ScePaf code and data references before installing
its hooks; if that optional path is unavailable, the baseline page/count patches
remain active. That release uses only the baseline patches on retail 3.65 and
PTEL 3.60.
The implementation and device-validation record are in
[the icon-cache notes](docs/icon-cache-trial.md).

Version 1.4.0 also extended boot recovery for applications already hidden by the
old 500-application limit before this plugin was installed. Recovery uses the same
configured application and page limits as the shell. Version 1.7.0 removes the
pre-start allocator hook; the top-level limit is instead required to equal the
physical page capacity, currently 50 pages times 10 icons. The implementation
retains the native recovery algorithm rather than reinstalling applications,
rebuilding the database, or hiding the warning. Hardware and offline validation
are documented in [the recovery notes](docs/recovery.md).

## Build with the VitaSDK image

The current `gnuton/vitasdk-docker:latest` SDK image is based on Ubuntu 22.04
and has glibc 2.35, while its bundled VitaSDK host compiler requires glibc 2.36
and 2.38. The included multi-stage Dockerfile copies that exact VitaSDK into an
Ubuntu 24.04 userspace.

```sh
docker build --platform linux/amd64 \
  -t livearea-nolimits-vitasdk:ubuntu24.04 .

docker run --rm --platform linux/amd64 \
  --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" -w /work \
  livearea-nolimits-vitasdk:ubuntu24.04 \
  cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIVEAREA_ICON_CACHE_TRIAL=ON \
    -DLIVEAREA_ICON_CACHE_LOGGING=OFF \
    -DLIVEAREA_DEBUG_LOGGING=OFF

docker run --rm --platform linux/amd64 \
  --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" -w /work \
  livearea-nolimits-vitasdk:ubuntu24.04 \
  cmake --build build
```

The result is `build/livearea_nolimits.suprx`. These options reproduce the release
configuration: optimized code, the multi-firmware icon-cache correction, and no
runtime logging. The project retains its explicit `-O2` optimization level.

The icon-cache correction defaults to `ON` in fresh configurations. The historical
option name `LIVEAREA_ICON_CACHE_TRIAL` is retained for build compatibility; pass
`-DLIVEAREA_ICON_CACHE_TRIAL=OFF` for an explicit limits/recovery-only build.
Logging is controlled separately. `LIVEAREA_ICON_CACHE_LOGGING` and the broader
`LIVEAREA_DEBUG_LOGGING` both default to `OFF`. Logless builds do not open,
truncate, write, delete, or rotate a diagnostic file, even on validation failure;
an existing log from an older build is left untouched.

For recovery or cross-component diagnostics, enable `LIVEAREA_DEBUG_LOGGING` and
set a recognizable `LIVEAREA_DEBUG_BUILD_ID`. The older v1.3.0 asset has startup
diagnostics enabled and remains unchanged; the v1.4.0 through v1.8.0 regular
release assets disable all runtime logging. See [the changelog](CHANGELOG.md).

Host startup and rollback tests can be run with `python3 tests/run.py`.

## Install

Copy `livearea_nolimits.suprx` to `ur0:tai/`, then add it to the special SceShell
section in `ur0:tai/config.txt`:

```text
*main
ur0:tai/livearea_nolimits.suprx
```

Reboot so the plugin runs before the shell constructs its page container.
Do not place this user plugin under `*KERNEL`.
Do not place it under `*NPXS10015`; that title ID belongs to SceSettings.

To upgrade, replace the existing SUPRX at the configured path and reboot.
For the already-hidden-applications case, use a full reboot rather than standby.
Do not delete applications or manually rebuild the database as a prerequisite
for testing the recovery correction. Applications can still remain hidden when
the configured total or top-level capacity is genuinely exhausted.

An expanded database can exceed the stock shell's page or application limits.
Disabling the plugin, including by holding L during boot, does not contract that
database and can leave the stock shell unable to finish booting. Restore a
stock-compatible `app.db` backup at the same time as disabling the plugin; Safe
Mode database rebuild is the destructive fallback.

This is an FW 3.60/FW 3.65 system-shell patch. Keep a working plugin-recovery
method before installing it. Holding `L` during boot normally suppresses taiHEN
plugin loading and allows a bad configuration entry to be removed.

## Changing the limits

The limits are in `src/limits.h`. Page limits still use 8-bit Thumb immediate
fields, but the top-level count uses wide Thumb-2 comparisons. The top-level and
total limits must be encodable by the corresponding Thumb-2 instructions. Because
the recovery allocator cannot safely be hooked before module relocation, the
top-level limit must equal `page limit * 10`; it also cannot exceed the counted-
icon limit. Ten icons per page remains fixed. The assembler rejects oversized
instruction blocks; native tests also verify their exact footprints.
