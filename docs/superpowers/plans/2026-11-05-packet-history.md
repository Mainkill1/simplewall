# Packet History Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans task-by-task.

**Goal:** Deliver the approved native app split view with durable bounded packet history.
**Architecture:** Portable SQLite storage and query module; bounded Windows event ingestion; UI-thread-owned native split panel with paged owner-data lists. One history writer and a cancellable reader use independent connections.
**Tech Stack:** C, Win32, SQLite amalgamation, MSVC, Python/C behavioral tests.
**Spec:** docs/superpowers/specs/2026-11-05-packet-history-design.md

## Global Constraints
Retain individual events. Defaults 200,000 events/app, 7 days, 1 GiB budget. 500-row pages; 8,192 records/32 MiB ingestion; 256-event/250 ms batches. Latest matching event sorts apps first. Preserve selected identity and firewall profile/policy.

## Review Focus
- Late/tied events and same-basename apps must retain distinct identities and stable ordering.
- Saturation, write failure and shutdown must release snapshots and expose loss.
- Fast selection and filter changes must discard stale pages.
- Retention and clear must update summaries and preserve other apps.
- Existing generic packet handlers must not interpret new owner-data row contexts.

### Task 1: Durable storage and queries
Files: vendor/sqlite/*, src/packet_store.[ch], tests/test_packet_history.py, tests/packet-store-test.c, project/build definitions.
Interfaces: PH_STORE opaque; PH_EVENT owned UTF-8 input; ph_open/close, ph_begin/append/commit, ph_apps, ph_query, ph_retention, ph_clear, ph_limits.
- [ ] Write C integration tests for exact repeats, group counts, recency, stable identity, retention isolation, restart, unsupported schema, read-only failures and 200k ingestion.
- [ ] Compile/run and confirm missing storage implementation fails; implement against actual bundled SQLite; pass integration tests and sanitizer run.
- [ ] Pin SQLite source and provenance; test Windows-compatible build settings; commit.

### Task 2: Bounded history lifecycle and capture
Files: src/packet_history.[ch], src/log.c, src/main.c, src/global.h, project files.
Interfaces: _app_packet_init(HWND), _app_packet_submit(PITEM_LOG), _app_packet_shutdown(), _app_packet_message(HWND,UINT,WPARAM,LPARAM,LRESULT*).
- [ ] Write queue behavior tests (count/bytes saturation, rejected release, close/drain) before implementation and demonstrate failure.
- [ ] Bound upstream captured events too; hand snapshots to single writer; batch commits; independent reader; retained gap/error status; graceful stop/drain. Remove packet capture's direct list mutation and resolver flood.
- [ ] Verify portable queue and storage tests plus existing 16 tests. Commit.

### Task 3: Native split view
Files: src/packet_history.c, src/main.c, src/messages.c, src/listview.c, src/resource.h.
Interfaces: task 2 message router owns packet panel and all children; old IDC_LOG becomes panel host. Generic list paths bypass this host.
- [ ] Create native UI regression script asserting split controls, blocked/allowed/direction/group toggles, latest-blocked order, stable selection, clear/limit controls, bounded pages and rapid switching with stale queries.
- [ ] Implement panel with filterable app list, persistent selected identity, recency batches, app header/stats, group/raw paged rows, clear/allow, limits, explicit errors/gaps, lazy hostname enrichment and retained event details.
- [ ] Verify real Windows controls under synthetic database capture and restart; keep older-page scroll stable. Commit.

### Task 4: Scale verification, review and deployment
Files: tests/windows-packet-history.ps1, tools build/install helpers, documentation.
- [ ] Run 200k real-store stress/retention/overload tests; existing suites/sanitizers; native ABI and Release x64 build.
- [ ] Fresh whole-branch review; resolve material findings with regression tests.
- [ ] Back up executable and user data; install and launch; verify capture, persistence, stable app selection and measured responsiveness. Update fork/release only after evidence.

## Execution ledger
User approved design and explicitly instructed implementation. Native inline execution, existing non-main feature branch retained; no extra worktree or execution-method prompt required. Baseline: 16 portable tests passed. Plan and spec use the same public store and UI lifecycle interfaces; no pre-flight interface conflicts.

Task 1: store tests RED (missing implementation), GREEN (200k events 5.398s), ASan/UBSan GREEN (13.896s). SQLite 3.54.1 archive verified against official SHA3-256. Queue budget test RED then GREEN.

Task 2: implemented bounded capture and history budgets, owned references, single writer, independent cancellable reader, durable gap totals, fair bounded cleanup, and shutdown drain. Capture no longer mutates packet controls.
Task 3: implemented native split view, latest matching activity order, stable app identity/selection, grouped/raw pages, filters, detail dialog, per-app clear/allow, retention controls, cached background reverse DNS and legacy shortcut routing. The prior suspend handler no longer clears persistent history.
Ruling: keep existing feature branch and Windows build checkout; user explicitly instructed implementation. SQLite source remains unmodified and statically linked. Journal budget uses physical DB/WAL/SHM sizes with headroom and rejects writes under reader-pinned pressure, then safely truncates after recovery. No claim of an exact ceiling for transient SQLite temporary files.
Review: independent reviewer identified stale recency after clear, unfair dirty cleanup, legacy LVM traversal hangs, orphan metadata/journal growth and non-atomic eviction counters. Reproduced and fixed with behavior regressions; gap persistence now increments the UI revision as requested in final review.
Validation so far: portable production-store suite passes 200k ingestion plus raw/grouped/app paging, churn, held-reader pressure, failure-trigger rollback, unsupported-schema preservation and restart. Existing 16 regressions pass. ASan/UBSan store/queue checks pass. Native x64 store and SDK ABI tests passed; Release x64 build passed.
Host: installed process 33692, SHA-256 1f5470b90f3d339e323f7d9e80da4357fb2b5b262861e2126306cc4351aedfb5. Backup: user install/backups/packet-history-20261105-175307. Native controls/20 rapid filter switches passed, max main-window probe2.2631ms. Read-only Windows SQLite check saw133 committed live events after startup. Further live/restart verification pending.

Final host validation: build253c063075c425f360f2a849941dad648c1c33e015b22c91d67c106f320d5b9e installed/running as process33764. Restart preserved947/947 previously committed records. Live curl test increased stored events266→270 and placed curl first in recency order. Native app selection remained stable under incoming traffic; rapid20 filter switches max main-window probe2.4021ms. PrintWindow inspection verified the actual dark split UI, friendly app names/icons, grouped rows, reverse-DNS enrichment and retained counts. Profile and prior binaries backed up in the user installation/backups directory. Remaining: source publication and release verification.

Publication complete: fork master source snapshot4bf72620133eac9698c785f141c1f225be3de5ac has exactly the local implementation tree. Release v3.9.2-fork.2 published by successful workflow42147962294. GitHub asset digests match the installed executable253c063075c425f360f2a849941dad648c1c33e015b22c91d67c106f320d5b9e and ZIP372652f3e45412cdd6332f1d4be13306c4435de8fc9a3de8d3e1f414454c1a4a. Native live controls, stable selection, capture and restart persistence verified. No required implementation or deployment work remains.
Validation boundary: 200k stress and pressure/failure injection ran against isolated production-store databases; native UI checks used actual host traffic, avoiding synthetic firewall profiles on a host with active filters. UI reads at most500 result rows and500 app rows per page. Existing packet/CSV preferences and firewall rules were preserved.
