# Firmware compatibility

The source profiles cover 31 firmware/hardware combinations. Profile selection
uses the loaded SceShell, ScePaf and SceDbRecovery module identities. Each component
has its own validated layout; the spoofed system version is not used.

## Firmware matrix

“Validated” means the exact decrypted modules were checked offline and the
compiled plugin passed native ARM capacity, cache and recovery tests for them.
It does not mean that firmware was booted on physical hardware during this work.
“Unverified” means no matching external firmware modules were available; it is
not a claim that the build does not exist.

| Firmware | Retail Vita/PSTV | Testkit | Devkit |
| --- | --- | --- | --- |
| 3.60 | Validated | Validated | Validated |
| 3.61 | Validated | Validated | Validated |
| 3.63 | Validated | Validated | Validated |
| 3.65 | Validated | Validated | Validated |
| 3.67 | Validated | Validated | Validated |
| 3.68 | Validated | Validated | Validated |
| 3.69 | Validated | Unverified | Validated |
| 3.70 | Validated | Validated | Validated |
| 3.71 | Validated | Unverified | Validated |
| 3.72 | Validated | Validated | Unverified |
| 3.73 | Validated | Validated | Validated |
| 3.74 | Validated | Unverified | Unverified |

Kit entries refer to the corresponding external `3.xxx.011` build. Internal/QAF
images with missing module fingerprints are not substituted for external builds.
Unknown module identities fail validation before their component is patched.

## What is validated

- All 22 SceShell capacity patch regions, their instruction sizes and boundaries.
- Shell text/data dimensions, icon-pool globals, pool/request entry prefixes,
  original recovery-load imports and native completion callbacks.
- Both PAF layouts: native pool/image virtual tables, allocation and surface
  release, widget status/getter behavior, module-cache reuse and initialization.
- All seven recovery patches, module interfaces, stop entries, segment sizes
  and the component-specific recovery action flag. Devkit capacity sites shift
  by four bytes; testkit recovery flags are stored at data `+0x1108`.
- Required system-function exports in each firmware's libkernel/module manager.
- Startup, partial injection failure, corrupted code rejection and rollback under
  host ASan/UBSan. Native ARM checks retain pending artwork subscriptions and
  texture references across allocation pressure without modifying shared PAF code.

OS services, asynchronous decoding, scheduling and UI rendering are modeled in
ARM tests. Physical-device testing and maximum-library stress coverage are
separate from these checks.

The exact module identities, segment sizes, hashes and profile offsets are in
[firmware-profiles.json](firmware-profiles.json). Firmware binaries and decryption
keys are not included in the repository or plugin.

## Local firmware archive

The inputs are under `/Volumes/data/Downloads/vita/decrypted/external/`, with
`official/`, `testkit/` and `devkit/` hardware directories. Additional testkit
3.700.011/3.730.011 and devkit 3.690.011/3.700.011 builds are stored there with
original PUPs, extracted packages/filesystems, converted ELF modules and download
provenance. PUP payloads remain intact; sparse temporary padding is used only
when extracting truncated free space from FAT volumes.

The added PUPs came from the [firmware archive](https://darthsternie.net/ps-vita-dev-firmwares/).
Published MD5 checksums were checked where listed, and SHA-256 hashes were
recorded for all four downloads and extracted module inputs.

## Repeating offline checks

Build the plugin with the VitaSDK image described in the README, then run:

```sh
python3 tests/run.py
python3 tests/firmware_matrix.py \
  --firmware-root /Volumes/data/Downloads/vita/decrypted/external \
  --plugin build/livearea_nolimits \
  --output build/firmware-matrix.json
```

The matrix runner requires Unicorn 2.x. It opens firmware inputs read-only and
checks their text hashes before executing the compiled plugin against them.
