# LiveArea NoLimits

`livearea_nolimits.suprx` is a taiHEN user plugin for the PlayStation Vita retail
FW 3.60 and FW 3.65 `SceShell`, with a separate PTEL/testkit 3.60 profile.
It changes the home-screen limits to:

- 26 pages;
- 10 top-level icons per page, unchanged from the firmware;
- 255 top-level icons in total, the maximum supported by the original
  instruction forms;
- 1,000 counted application/content icons instead of 500.

The plugin validates every original instruction before applying any injection.
It selects a patch profile by the loaded `SceShell` module NID and verifies its
text-segment size and the expected code at every patch site. HENkaku version
spoofing can remain enabled: the plugin does not use the system-version API. This
protects unsupported firmware versions and already-modified shells from blind
writes. Release builds do not create or write runtime log files.

Pages after the original first ten use the firmware's default page appearance.
The plugin intentionally leaves the ten-entry custom theme/layout table bounds
unchanged so that extra pages cannot read beyond that table.

Version 1.2 corrects the firmware profiles in 1.0/1.1: the original reference
was PTEL 3.60, and the profile previously labeled 3.65 was actually retail 3.60.
The retail 3.65 profile is now mapped against an identified 3.65 update image.
See [firmware validation](docs/firmware-validation.md) for the binary identities
and verification details.

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
  cmake -S . -B build -G Ninja

docker run --rm --platform linux/amd64 \
  --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" -w /work \
  livearea-nolimits-vitasdk:ubuntu24.04 \
  cmake --build build
```

The result is `build/livearea_nolimits.suprx`.

Host startup and rollback tests can be run with `python3 tests/run.py`.

## Install

Copy `livearea_nolimits.suprx` to `ur0:tai/`, then add it to the special SceShell
section in `ur0:tai/config.txt`:

```text
*main
ur0:tai/livearea_nolimits.suprx
```

Reboot so the plugin runs before the shell constructs its page container.
Do not place it under `*NPXS10015`; that title ID belongs to SceSettings.

To upgrade, replace the existing SUPRX at the configured path and reboot.

This is an FW 3.60/FW 3.65 system-shell patch. Keep a working plugin-recovery
method before installing it. Holding `L` during boot normally suppresses taiHEN
plugin loading and allows a bad configuration entry to be removed.

## Changing the limits

The limits are in `src/limits.h`. Page and top-level limits must fit the
firmware's existing 8-bit Thumb immediate fields. The total icon limit must be
encodable by each existing Thumb-2 immediate instruction; the assembler will
fail rather than silently emitting a different instruction sequence.
