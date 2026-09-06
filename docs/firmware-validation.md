# Firmware profile validation

Version 1.2 selects the loaded `SceShell` by module NID, then checks the exact
text-segment size and all 23 original instructions before the first injection.
Unknown identities, mismatched sizes, or altered patch sites fail without
installing any patch. An injection failure rolls back earlier injections.

| Profile | Module NID | Segment 0 size | Reference |
| --- | --- | --- | --- |
| Retail 3.60 | `0x0552F692` | `0x541B74` | Decrypted `vs0:vsh/shell/shell.self` read from a retail 3.60 Vita |
| Retail 3.65 | `0x5549BF1F` | `0x5420F4` | Decrypted shell from the 3.65 update package |
| PTEL/testkit 3.60 | `0xEAB89D5C` | `0x535CF4` | Original decrypted PTEL reference ELF |

The reference identities were read from each binary's module information.
They agree with the retail/testkit mappings in
[HENkaku's shell patch selection](https://github.com/TheOfficialFloW/update365/blob/master/henkaku/user.c).

The 3.65 input is the `PSP2UPDAT.PUP` asset from
[the original 3.65 updater release](https://github.com/TheOfficialFloW/update365/releases/tag/v1.0),
MD5 `0a0f2a9ae58968ac5d1d2127049c3cba` (133,754,368 bytes). It was extracted
locally, without installing firmware on a device. Its shell's SELF system
version is `0x36500000000`; the retail 3.60 SELF reports `0x36000000000`.

SHA-256 of the decrypted segment 0 bytes used for verification. The retail
inputs are extracted from SELF files before runtime relocation; the original
PTEL reference is a dump of the loaded image:

| Profile | SHA-256 |
| --- | --- |
| Retail 3.60 | `83c52165de3fb8afc5b025edd0b0ab71e0967644952617cb60b6b9e6c329e8c2` |
| Retail 3.65 | `7e4264294a871de9174c1ec74fa7f3405727ed7d6ed001f112a5899f27c2b724` |
| PTEL/testkit 3.60 | `0ceff6e59af541ab72352e2f1674b576e43730aca8e708d315c29c8651df489c` |

## Correction to versions 1.0 and 1.1

The old `patches_360` table matches the PTEL binary. All 23 entries in the old
`patches_365` table match retail 3.60; none match the identified retail 3.65
image at the old offsets.

The previous version detection used `sceKernelGetSystemSwVersion`, which
[HENkaku hooks to return the configured spoofed version](https://github.com/TheOfficialFloW/update365/blob/master/henkaku/kernel.c).
Consequently, an old log saying "selected FW 3.65 profile" established only
the API's reported value. It did not establish the identity of the shell.
Spoofing another version could also reject a supported shell before validation.

## Retail patch locations

All offsets are relative to segment 0. Each mapped 3.65 site was checked against
the corresponding instruction sequence and its containing function in IDA;
the small integer alone was not used to identify a site.

| Purpose | Retail 3.60 | Retail 3.65 |
| --- | --- | --- |
| Page-container capacity | `0x0A8C70` | `0x0A8CC8` |
| Page-creation guard | `0x0A97E2` | `0x0A983A` |
| Page-count guard during top-menu processing | `0x0ADF00` | `0x0ADF58` |
| Page-count clamp comparison | `0x0AF166` | `0x0AF1BE` |
| Page-count clamp value | `0x0AF16A` | `0x0AF1C2` |
| Page-count guard during icon movement | `0x0B6D86` | `0x0B6DDE` |
| Page edit-mode guard | `0x0B8672` | `0x0B86CA` |
| Add-page control visibility | `0x0C4B66` | `0x0C4BBE` |
| Add-page control state | `0x0C59DE` | `0x0C5A36` |
| Add-page callback guard | `0x0D4044` | `0x0D409C` |
| Pages included in capacity count | `0x0552B0` | `0x055308` |
| Top-level icon capacity | `0x0552CA` | `0x055322` |
| Top-level limit message selection | `0x063A8E` | `0x063AE6` |
| Top-level limit message selection | `0x063A96` | `0x063AEE` |
| Counted-icon guard | `0x026728` | `0x026780` |
| Counted-icon capacity | `0x0552A4` | `0x0552FC` |
| Counted-icon limit message selection | `0x063A9A` | `0x063AF2` |
| Counted-icon limit message selection | `0x063AAA` | `0x063B02` |
| Overflow count arithmetic | `0x0AA254` | `0x0AA2AC` |
| Overflow message guard | `0x0AA2B2` | `0x0AA30A` |
| Overflow message limit value | `0x0AA2DE` | `0x0AA336` |
| Top-menu counted-icon guard | `0x0ADDC2` | `0x0ADE1A` |
| Remaining counted-icon capacity | `0x360656` | `0x360A9A` |

For example, the 3.65 page-creation guard reads the top-menu object's page
count at `+0x30` and compares it to 10 at `0x0A983A`. The same function has a
separate ten-entry appearance-table check at `0x0A98B0`; that check stays
unchanged. Likewise, the two ten-icon loops in the edit-mode function stay
unchanged while its page-count check at `0x0B86CA` is patched.

## Repeating the checks

`python3 tests/run.py` compiles the actual plugin startup code against host
mocks with AddressSanitizer and UndefinedBehaviorSanitizer. It checks all
profiles, unknown/mismatched identities, segment-size mismatches, module-query
failures, missing segments, every corrupted patch byte, every partial-injection
failure, and complete restoration on unload.

To run the same tests against actual decrypted text segments:

```sh
python3 tests/run.py \
  0x0552F692 /path/to/retail-360.text.bin \
  0x5549BF1F /path/to/retail-365.text.bin \
  0xEAB89D5C /path/to/ptel-360.text.bin
```

The host tests use placeholder replacement data to exercise installation and
rollback; Thumb replacement instructions are assembled in the VitaSDK build.
Firmware binaries are local verification inputs, not repository or release
assets. Binary analysis and host tests establish profile selection, patch
locations, and rollback behavior; they do not establish the visible LiveArea
result on a retail 3.65 device.
