# simplewall upstream candidates

**Recommendation:** Send the toolbar fix and the Rules-tab insertion fix first, after a quick upstream Windows reproduction. Follow with the narrow search fix. Start the large-list series with prepared sort keys and the single-pass rule-row refresh. Treat the new-app prompt as a high-priority investigation, not a claim that one patch fixed every freeze.

Compared [fork.3 (2b0a5ca6)](https://github.com/Mainkill1/simplewall/tree/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db) to [upstream master (8c8bbbae)](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa); fork point [`24bbd2ce`](https://github.com/henrypp/simplewall/commit/24bbd2ceb7daee9ef308e0b406be5eb039cb30a1). Reviewed the extra [unmerged burst-hardening branch](https://github.com/Mainkill1/simplewall/commit/fc6c41b719d0a912f189e8797eeb66af6c440992) as an experiment only. Current upstream already changed search UI, log grouping and notification icon rendering, so do not re-send those as fork fixes. Examples are from exact fork history, not a clean patch against current upstream. No PR or issue was submitted.

## Send these first

- [Fix the notification and logging toolbar toggles](Toolbar-checked-state.md) — Send first · small.
- [Give Rules rows a real group at insertion](Rule-tabs-not-blank.md) — Send first · small, reproduce first.
- [Debounce search and update only rows whose visibility changes](Search-less-list-work.md) — Send first · small after upstream search changes.

## Large-list performance, split by cause

- [Prepare list sort keys once per row](Prepared-list-sort-keys.md) — Next · moderate, isolated.
- [Refresh affected app rows in one pass](Bulk-rule-row-updates.md) — Next · small.
- [Cache rule membership for large status and list scans](Rule-membership-index.md) — Next · moderate; invalidation is the work.
- [Keep file-existence checks out of painting and status](Async-file-availability.md) — Next · moderate.
- [Deduplicate file metadata jobs and coalesce redraw](Metadata-and-redraw.md) — Next · moderate; two PRs if needed.
- [Make Refresh recheck availability without rebuilding firewall state](Soft-refresh.md) — Discuss first · small design change; moderate implementation.
- [Index connected app hashes per network poll](Connection-highlight-index.md) — Next · moderate.

## Responsiveness and compatibility, stage separately

- [Reduce new-app prompt stalls without changing firewall behavior](New-app-notification-stalls.md) — High impact · small first experiment; worker/persistence follow-up.
- [Batch live packet-list updates without shipping SQLite](Packet-log-stalls.md) — High impact · proposal only; do not cherry-pick the redesign.
- [Normalize UNC executable identities for log and WFP](Network-share-app-identities.md) — Useful if requested · moderate; WFP review required.

## Evidence, not marketing

- Audited in source, with pinned comparisons and excerpts on each page. Local portable tests on fork.3: **22 passed, 2 skipped** (public Routine checkout unavailable); share-path assertions passed. No new Windows UI, WFP, or end-to-end performance runs.
- [Earlier fork host tests](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/docs/superpowers/plans/2026-11-05-large-app-lists-progress.md) exercised checking states and soft Refresh. They do not provide a matched upstream/fork time-to-render result for large lists. For maintainers, measure cold tab paint and search/sort separately with the same profile, then p50/p95 window message return during a fixed-rate blocked-app burst.
- Two symptoms, two paths: discovering or acting on a new app can trigger profile and UI work; a packet backlog can overwhelm the live list even independent of the prompt. Don't combine the fixes under a generic “UI freeze” label. Test popup off/on with live log off/on and use known vs new app events to narrow the cause.

## Leave out of the first series

Keep the SQLite packet-history schema, dual-pane Packet History controls (including its copy/sort buttons), per-app allow capture, Security 5157/PID/service heuristics, timed auto-allow, process controls and fork update feed out of small upstream PRs. These are fork features, interact with policy or retention, or depend on new controls. Likewise, public Routine ABI/build shims fix the fork's compatibility toolchain; send them only if reproduced using an upstream-supported build and in the repository that owns the contract.

A good upstream PR changes one behavior, gives a short reproduction, states what stays unchanged, and reports a matched baseline/candidate result when it claims a speedup.

