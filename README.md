# livearea_nolimits

livearea_nolimits is a taiHEN user plugin that expands the PlayStation Vita and
PSTV home-screen capacity. It supports large application libraries and manages
icon artwork caching as you scroll through LiveArea.

**[Download the latest release](https://github.com/devnoname120/livearea_nolimits/releases/latest)**

## Limits

| Item | Official limit | **With livearea_nolimits** |
| --- | ---: | ---: |
| LiveArea pages | 10 | **50** |
| Top-level icons in total | 100 | **500** |
| Application/content icons, including those inside folders | 500 | **4,000** |

Each folder occupies one top-level slot. Applications inside folders count toward
the 4,000 application/content limit without taking additional top-level slots.

## Compatibility

A system with taiHEN plugin support is required. Supported firmware:

- Retail PS Vita and PSTV: **3.60 or 3.65**.
- PTEL/testkit: **3.60**.

Compatibility depends on the actual firmware, not the spoofed version shown by
HENkaku. The plugin validates the loaded modules and expected instructions before
applying patches.

## Installation

Before installing, back up your active taiHEN configuration and the LiveArea
layout database at `ur0:shell/db/app.db`. Keep these backups somewhere accessible
from a computer.

1. Download `livearea_nolimits.suprx` from the
   [latest release](https://github.com/devnoname120/livearea_nolimits/releases/latest).
2. Copy it to `ur0:tai/livearea_nolimits.suprx`.
3. Add the following entry under `*main` in your active taiHEN `config.txt`:

   ```text
   *main
   ur0:tai/livearea_nolimits.suprx
   ```

   The configuration is usually `ur0:tai/config.txt`. If `ux0:tai/config.txt`
   exists, it takes precedence; edit the configuration your system uses.
4. Fully reboot the system. Standby does not reload the plugin.

Installation does not require deleting applications or rebuilding the database.

To update, replace the SUPRX at the configured path and fully reboot. Keep a
single configuration entry for the plugin.

## Behavior and limitations

- Icon artwork stays cached until memory pressure requires eviction. Evicted
  artwork reloads when needed; brief white placeholders can appear while cold or
  evicted artwork loads.
- Icon-cache management is limited to LiveArea; other applications keep their
  normal caching behavior.
- Pages after the first ten use the firmware's default page appearance.
- Boot recovery can restore applications hidden by the firmware's application
  limit when application and page capacity are available. See the
  [recovery notes](docs/recovery.md) for details and limitations.
- Regular release builds have runtime logging disabled.

### Disabling or removing the plugin

An expanded layout can exceed the firmware's normal page or application limits.
Before disabling or removing the plugin, restore a compatible layout/database.
Otherwise, the system may be unable to finish booting into LiveArea.

Holding `L` during boot skips taiHEN plugin loading, but does not shrink the
layout. It is not sufficient recovery for a database that depends on the expanded
limits. Keep a working plugin and a compatible `app.db` backup available.

## Reporting a problem

Check the [issues](https://github.com/devnoname120/livearea_nolimits/issues) for
an existing report. Include your actual firmware version, device model, plugin
version, other enabled plugins, and the steps that trigger the problem. Include
any error code or crash dump produced by the failure.

Diagnostic builds can provide logs for investigation. Follow the
[diagnostic guidance](docs/diagnostics.md) and any instructions in your issue for
choosing a build and collecting its logs.

## Building from source

With Docker installed, run these commands from the repository root. The included
Dockerfile provides VitaSDK and the host build tools.

```sh
docker build --platform linux/amd64 -t livearea-nolimits-vitasdk .

docker run --rm --platform linux/amd64 \
  --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" -w /work \
  livearea-nolimits-vitasdk \
  cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIVEAREA_ICON_CACHE_TRIAL=ON \
    -DLIVEAREA_ICON_CACHE_LOGGING=OFF \
    -DLIVEAREA_DEBUG_LOGGING=OFF

docker run --rm --platform linux/amd64 \
  --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" -w /work \
  livearea-nolimits-vitasdk \
  cmake --build build
```

The output is `build/livearea_nolimits.suprx`, built with icon-cache management
enabled and runtime logging disabled.

Set `LIVEAREA_ICON_CACHE_TRIAL=OFF` to disable icon-cache management. For diagnostic
builds, set `LIVEAREA_DEBUG_LOGGING=ON` and provide a recognizable
`LIVEAREA_DEBUG_BUILD_ID`. `LIVEAREA_ICON_CACHE_LOGGING=ON` also enables the logger.

Run the host tests with `python3 tests/run.py`.

The limits are defined in [src/limits.h](src/limits.h). The top-level limit must
equal the page count times ten and cannot exceed the application/content limit.
Replacement instructions must be able to encode the selected values.

For implementation details, see the [capacity notes](docs/top-level-capacity.md),
[icon-cache design](docs/instance-cache.md), and
[firmware validation](docs/firmware-validation.md).
