# Large App Lists Implementation Plan

Goal: Remove repeated full-list/rule/file work and install the validated Windows build.
Architecture: Preserve the Win32 controls; cache rule membership and filesystem metadata, prepare sort keys, batch bulk edits, debounce search and coalesce save/redraw requests.
Tech Stack: C, Win32, routine, MSVC, Python regression runner.
Spec: docs/superpowers/specs/2026-11-05-large-app-lists-design.md

Constraints: Preserve natural text sorting, enabled/disabled rule semantics, fresh purge checks, saved profiles and active firewall policy.
Review focus: rule mutations/reloads; duplicate queued metadata and failure cleanup; sort equal keys and checkbox ordering; pending saves at shutdown; search clearing/tab switching.

- [x] 1. Restore matching build dependencies and Windows toolchain; establish a baseline build.
- [x] 2. Write behavioral regression tests using production C functions with minimal platform shims; show failures for repeated rule scans, redundant search changes and metadata failure cleanup.
- [x] 3. Implement rule membership index, cached UI existence/status checks, indexed batch row updates, prepared sorting and debounced search.
- [x] 4. Deduplicate metadata loading, coalesce redraws and profile saves, cache connection highlights. Verify failure and shutdown behavior.
- [ ] 5. Build Release x64, run regressions and scale checks, obtain independent code review, resolve material issues.
- [x] 6. Back up existing host installation/profile if present, install and launch build; verify running executable and responsiveness.
