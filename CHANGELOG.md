# Changelog

## 1.9.0 - 2026-09-12

- Replace shared PAF cache/consumer hooks with private virtual tables on
  SceShell-owned icon-pool and image instances. Other processes retain native
  PAF code; no additional kernel plugin is required.
- Keep persistent image handles and single-victim LRU eviction. On allocation
  failure, release one eligible oldest texture and retry before selecting another.
- Keep missing artwork pending and retain a texture reference until native
  widget binding finishes. Clean up instance records on native destruction and
  refuse hot unload while detours or instances remain live.
- Use checked four-byte SceShell detours with explicit PUSH replay, including the
  five-argument image-request ABI. Validate retail 3.60, retail 3.65 and PTEL 3.60
  firmware profiles. Shared PAF executable code and tables are not patched.
- Build in Release mode with runtime logging disabled. Retain 50 pages, 500
  top-level icons, 4,000 counted icons and the existing recovery implementation.

A diagnostic build of this cache policy completed boot and scripted scrolling,
allocation pressure and reload tests on retail 3.60. Temporary white placeholders
still appear in the fastest cold/evicted-page pass and recover. The exact logless
release artifact has offline ARM/build validation; it was not installed for that
physical test. Retail 3.65 and PTEL cache validation is offline. The hidden-app
recovery issues #8 and #10 remain unresolved.

## 1.8.0 - 2026-09-10

- Publish the recovery interception fix from rc1 in the regular release: replace
  shared `SceLibKernel` lifecycle hooks with the validated SceShell recovery-ready
  callback and temporary recovery-module stop redirect. Retain all seven recovery
  capacity patches, cleanup protection, and the icon-cache correction.
- Build in Release mode with `-O2` and both runtime logging options disabled.
  Existing diagnostic logs are left untouched.
- Raise the counted application/content-icon limit from 1,000 to 4,000 in both
  SceShell and hidden-app recovery. Keep 50 pages, 500 top-level slots, and ten
  icons per page. The published v1.8.0-rc1 artifacts remain unchanged.
- Update the plugin module version to 1.8.

Reporter results with rc1 include working VitaShell rename/edit in #6, resolution
of delayed crashes on one setup in #4, and successful PSTV page creation after
correcting the configuration in #9. Remaining PSP/PSX launch and system hangs in
#6 are unresolved. These reports do not establish hidden-app restoration with
the replacement callback or hardware acceptance at the full 4,000-icon capacity.

## 1.8.0-rc1 - 2026-09-10 (prerelease)

- Replace shared `SceLibKernel` module lifecycle interception with the validated
  SceShell recovery-ready callback and a temporary recovery-module stop redirect.
  Keep all seven recovery capacity patches and the icon-cache correction.
- Retain patch handles after failed cleanup, cancel unsafe module stop, and reject
  plugin unload while recovery patches or the native ready callback are active.
- Add diagnostic events for every recovery validation and installation boundary,
  expected/actual bytes on mismatch, native callback entry/return, and cleanup.
- Ship this candidate with logging enabled, build ID `v1.8.0-rc1`, sampled cache
  counters, current/previous log rotation, and explicit flushes at recovery
  boundaries. Handle short log writes and report write failures.
- Add regression checks for diagnostic failure paths and callback lifetime.

This is an offline-validated candidate, not a hardware-confirmed stable release.
Hidden-app recovery and the reported application, screenshot, freeze, and
sleep/wake symptoms require reporter results with this exact build.

## 1.7.0 - 2026-09-09

### Fixed

- Remove the pre-start `SceDbRecovery` allocator-entry hook. Its eight-byte
  taiHEN/substitute jump overlaps a loader relocation targeting the allocator's
  imported stack-guard MOVW. A pure-passthrough hook reproduced the same retail
  3.65 shutdown, while the seven direct recovery patches completed normally.
- Stop validating or resolving the unrelated allocator prologue and LSDB count
  export. Recovery now validates and installs only the seven instruction patches
  it needs, avoiding both the v1.6.0 guard-address rejection and the late-
  relocation collision.
- Require the configured top-level limit to equal page capacity. At the current
  50 pages and 10 icons per page, the native allocator is already bounded to 500
  top-level entries without the unsafe hook.

### Added

- Add compile-time comprehensive diagnostics for shell, recovery, and icon-cache
  startup and lifecycle analysis. They remain disabled by default and are absent
  from the release binary.

### Hardware validation

- On retail 3.65, an affected database with 500 visible and 73 hidden
  applications recovered to 573 visible applications across 15 pages. The cache
  correction was enabled; scrolling, edit mode, one-minute idle, and repeated
  sleep/wake completed without an OS failure.
- The recovery module started and stopped normally, and all seven temporary
  patches were released. Equivalent affected-library testing remains outstanding
  on retail 3.60 and PTEL 3.60, as does the absolute 500-top-level-icon/50-page
  boundary.

### Operational note

- Disabling the plugin does not contract an expanded `app.db`. A stock shell can
  fail to boot when the saved page or application counts exceed stock limits;
  restore a stock-compatible database backup when disabling the plugin.

## 1.6.0 - 2026-09-07

### Fixed

- Validate the recovery allocator's stack-guard address against the actual
  SceLibKernel import, not the recovery module's text range. The old check could
  silently leave the native 500-application recovery limits active.
- Correct the recovery test fixture's import relocation and add regressions for
  the rejected runtime address, all guard-operand bits, the real firmware import
  records, and the reported 510-visible/one-hidden state. Affected-device shutdown
  and restoration still require a retest; these results are offline validation.

### Unchanged

- Release build with runtime logging disabled and the icon-cache correction
  enabled for retail 3.60, retail 3.65, and PTEL 3.60.
- 500 top-level icons, 50 pages, 10 icons per page, and the separate 1,000
  counted-application limit. No database-format change is introduced; recovery
  continues to use the existing firmware algorithm.

## 1.5.0 - 2026-09-07

### Fixed

- Enable the complete icon-cache correction on retail 3.65 and PTEL 3.60, not
  only retail 3.60. Every supported profile now includes LRU surface eviction,
  pending artwork reload handling, and pinned native consumer handoff.
- Select the matching shell initializer, pool-pointer slot, and PAF module
  identity. Validate the relocated pool reference before using it, including
  when the pool is already initialized. Unknown or mismatched modules are
  still rejected without installing cache hooks.

### Changed

- Enable the cache correction by default for fresh source builds. Keep the
  existing build option for explicitly requesting a limits/recovery-only build.
- Keep Release mode, logging disabled, 500 top-level icons, 50 pages, 10 icons
  per page, and the separate 1,000 counted-application limit.

### Validation scope

The cache startup/LRU regression suite now runs for all three shell/PAF pairs,
with logging both disabled and enabled. Actual retail 3.60, retail 3.65, and PTEL
PAF and shell bytes pass profile validation and taiHEN prologue-relocation tests.
Cross-profile identities, segment sizes, corrupted code, relocated pointers,
partial hook installation, and cleanup are covered. The shared consumer-reload,
capacity, and recovery suites remain enabled. Retail 3.65/PTEL runtime behaviour
and a populated 500-icon library still require hardware validation; implemented
feature parity is not a claim of identical hardware-test coverage.

## 1.4.0 - 2026-09-07

### Added

- Support for 500 top-level icons across 50 pages on the identified retail 3.60,
  retail 3.65, and PTEL 3.60 shell profiles. Ten icons per page and the separate
  1,000 counted-application/content limit are unchanged.
- Native boot-recovery patches for applications already hidden by the previous
  500-counted-application limit. Recovery uses the same page and capacity limits
  as the shell, with validated module identities and start/stop/unload handling.
- Full-byte validation and rollback tests for the wider capacity instruction
  blocks, native ARM admission/message tests, and recovery-boundary regressions.

### Changed

- The release is built in optimized Release mode with logging disabled. It
  retains the retail 3.60 icon-cache correction introduced in v1.3.0.
- Logless builds perform no diagnostic file operations; existing log files from
  older versions are left untouched. No database schema or theme-table changes
  are required.

### Validation scope

Offline validation covers the supported shell instruction profiles, recovery
instructions, startup/rollback, and cache regressions. The earlier 255-icon
recovery build completed a normal boot on the local retail 3.60 Vita with an
unchanged layout database. That device had no hidden applications, so restoration
of an existing hidden-app library was not exercised. The 500-icon/50-page build
has not yet been tested on hardware. The icon-cache correction remains specific
to retail 3.60; large-library rendering on other profiles is not hardware-verified.

For earlier releases, see the repository's GitHub release history.
