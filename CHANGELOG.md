# Changelog

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
