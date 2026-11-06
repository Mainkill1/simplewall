# Persistent packet history and app split view

Status: approved by the user ("implement it"); implemented and verified on the Windows host.

## Intent and established requirements

Make it practical to investigate roughly 200,000 packet-log events per day without
freezing simplewall. The user wants an app list, details for the selected app,
independent limits per app, history across restarts, and the latest app to be
blocked at the top of the list.

Visual reference supplied by the user:
`/home/main/codex/simplewall/Simplewall Packets Log Split View.html`.
Its split layout, Blocked/Allowed selector, app filtering, selection header,
direction filters and Group repeats toggle define the UI. The mockup sorts by
count; the user's explicit recency requirement overrides that sample behavior.
Its individual-event and grouped views are interpreted as requiring original
events retained on disk, with grouping as a reversible display option.

The investigation in `docs/investigations/2026-11-05-packet-log-freeze.md`
records a 12,039-entry cache with an intended 900-entry cap, 7,191 UI rows and
423–654 ms row-count queries. The current concurrent control mutations and
per-event full updates must be replaced, not carried into a new layout.

## Approaches

1. **Native split view with indexed SQLite history — recommended.** Reuse the
   Win32 application, toolbar and theme. Persist individual events, query one
   app/page at a time, and group repeated traffic on demand. This supports
   durable investigation while bounding UI work. It adds a storage component.
2. **Native split view with aggregate-only storage.** Smaller storage, but loses
   individual attempt timestamps and cannot implement the mockup's reversible
   Group repeats toggle faithfully. Not selected for this proposal.
3. **Embedded HTML UI.** Could reproduce CSS directly, but adds a browser runtime
   and native bridge and still needs the same ingestion and history redesign.
   It provides no advantage for solving this freeze; not recommended.

Only approach 1 meets the supplied UI and timeline requirements without adding
a separate UI platform. This is the builder's recommendation, not a claim that
the user explicitly selected SQLite or approved this document.

## User interface

Implement the reference as native controls on the Packets log tab, respecting
existing light/dark themes, DPI scaling, keyboard navigation and resize behavior.
Other tabs and the surrounding toolbar remain native.

- Left pane: Blocked/Allowed selector, app filter, pinned All apps row, app name,
  icon, latest matching event time, destination count and retained event count.
- Blocked is the initial view. App rows sort by latest blocked event descending.
  Allowed view sorts by latest allowed event. Event timestamp plus ingestion
  sequence breaks ties deterministically. Restore this order from history after
  restart; an older, late-arriving event must not move an app above a newer one.
- Update app order and counters in batches, at most four times per second while
  visible. Preserve selection by durable app identity as rows move, preserving
  the left pane's scroll anchor where practical. Never automatically select a
  different app when another app becomes first. No repeated updates for the
  same dirty state accumulate in the UI message queue.
- Right pane: selected app's name and full identity/path; counts for retained
  events, inbound, outbound and distinct remote destinations; Clear this app's
  log and Allow app; All/Inbound/Outbound filters; Group repeats toggle.
- Group repeats defaults on. Grouped rows show count and last seen, remote
  address/host/port, protocol, direction and rule. Expose first seen in row
  details. Turning grouping off shows actual retained events, newest first.
  Preserve local address/port, original timestamp, filter identity and action
  in event details even when the summary table omits those columns.
- All apps uses the same bounded queries with an additional App column. App
  actions are disabled when no individual app is selected.
- Use a virtual list backed by pages of 500 rows and a bounded page cache. Never
  materialize the entire retained history in a Windows list control. Page
  loading, direction filtering, sorting and grouping run off the UI thread.
- Each selection/filter query has a generation number. Ignore stale results;
  rapid app switching cannot display another app's history under the new header.
- Live updates preserve a user's scrolled position and selection. At the newest
  page they may update in place; while inspecting older events, show that newer
  events are available instead of jumping to them.
- Resolve hostnames only for requested visible destinations using a bounded,
  deduplicated background cache. DNS is enrichment, never required for storage
  or display. Label it as current reverse DNS, not a hostname observed at the
  original event time. No synchronous DNS or file metadata work in UI callbacks.
- Clear this app's log clears its blocked and allowed retained history after
  confirmation. It does not clear other apps or change firewall rules.
  Allow app invokes the existing explicit allow action for the selected known
  app; no traffic or policy changes occur from selection, refresh or grouping.
- Allowed traffic follows existing capture/exclusion preferences. If allowed
  capture is off, explain the empty view; do not silently enable it.

## Storage and retention

Builder-proposed defaults, adjustable globally and with per-app event-count
overrides: retain up to **200,000 events per app**, for up to **7 days**, under a
**1 GiB total history budget**. The earliest applicable bound removes oldest
events. A noisy app reaches its own limit without consuming other apps' per-app
allowances; the global emergency budget can evict the oldest events across apps
and that behavior must be visible in the retention settings. Limits apply to
raw retained events, not grouped display rows. Counters describe retained
history, not lifetime totals. Expose the retained time range and loss/eviction
status so a user can tell when a timeline is incomplete.

Use a dedicated history database in the existing per-user data directory, apart
from the firewall profile. Use SQLite with transactional batched writes and
crash recovery. Bundle/pin a buildable SQLite implementation rather than relying
on an optional operating-system DLL. Record its provenance and license in the
build/release materials. Exact dependency version and integration are left to
the implementation plan, verified against its official distribution.

App identity is the canonical executable path or package identity, including
service identity when the event actually supplies one. Do not persist the
current 32-bit hash as a unique database identity. Preserve System and unknown
traffic with explicit separate identities; do not merge unrelated known apps
by basename. Snapshot event identity and rule text so history survives app
removal, rename and filter recreation. Missing/uninstalled apps remain inspectable.

Store each accepted event with a monotonic database ID, full event time, app ID,
action, address family, local/remote addresses and ports, protocol, direction,
rule/filter snapshot and available user/layer information. No hash-based
deduplication of original events. Group by app, action, address family,
remote address/port, local address, protocol, direction and rule snapshot;
exclude the ephemeral local port from grouping but preserve it on raw events.
Compute distinct destinations as remote address/port/protocol combinations.

Index common app/action/time queries and keep app summaries for counts/recency
consistent with retention and clear operations. Grouped queries operate only
on retained events and must be cancellable/supersedable. Schema migration and
version checks happen before enabling history capture. Preserve a database
that cannot be migrated/opened; report the error instead of replacing it.

Bound ingestion by both record count and bytes (initial targets: 8,192 queued
records and 32 MiB), plus one in-flight write batch. Enqueue never waits on UI,
DNS or disk. On saturation, count rejected events and record/display an explicit
gap when storage recovers. Batch at most 256 events or 250 ms, whichever comes
first. Define durable history as committed transactions; a crash can lose the
pending batch/queue, while normal shutdown stops subscription, drains accepted
events and commits before closing history. Error paths must avoid waiting for
UI callbacks while holding storage locks.

Perform retention in bounded worker transactions independent of UI rows, on
write and idle maintenance, with age cleanup also on startup. Include database,
indexes, WAL and temporary storage when monitoring the global budget; reserve
headroom and use incremental reclamation/checkpointing so maintenance remains
bounded. The precise enforcement scheme is an implementation-plan decision;
do not promise an exact physical byte ceiling if SQLite temporary growth can
exceed it. If disk space is exhausted or writes fail, report capture degradation
and dropped counts; never block firewall processing or silently claim events
were persisted. Do not log packet payloads.

Existing optional CSV logging remains independent. The old session cache is
not a durable history source; do not pretend earlier discarded events can be
recovered. No automatic CSV import is required for this change.

## Thread ownership and integration

The capture path submits owned event snapshots to a bounded history queue.
One writer owns mutation/retention/clear commands. Independent reader work uses
consistent snapshots without blocking the UI. Workers never send synchronous
messages to packet controls or hold history locks while interacting with
controls. UI controls are mutated only by the UI thread, using immutable,
bounded results and coalesced notifications. Replace the packet path's direct
row insertion, UI-dependent eviction and per-record full update calls.

Do not turn the existing six packet workers into six database writers. Preserve
the existing notification/app-discovery/CSV paths while making their ownership
and handoff into history explicit. Review the upstream event queue as well as
the history queue: bounding only the second queue must not leave unbounded
captured-event allocations during overload. Release snapshots on every reject,
clear, stale-result and shutdown path.

## Validation and completion criteria

- Behavioral tests for per-app retention isolation, duplicate event preservation,
  grouped counts/first/last times, action/rule separation, identity collisions,
  unknown apps, clear, expiry, restart recovery and schema/write failures.
- Ingest at least 200,000 synthetic events with a dominant noisy app, enough
  distinct destinations to exercise many pages, IPv4/IPv6 and repeated events.
  Verify exact committed counts before retention and appropriate counts after
  each limit. Simulate burst overload and assert bounded allocations and visible
  loss accounting. Measure ingestion time, query latency and peak memory.
- Test live recency ordering including tied/late timestamps, stable selection
  and scrolling, rapid selection/filter changes and stale response rejection.
- Test UI responsiveness on the host during ingestion and maintenance, with
  packet tab both visible and hidden. Target sub-100 ms main-window probes under
  normal load; report measured percentile/max latencies rather than relying on
  the process Responding flag. No synthetic traffic changes firewall policy.
- Run existing portable/sanitizer tests, native SDK ABI tests and a Release x64
  build. Review ownership, query bounds, shutdown and database failure handling.
- Back up the current installed executable and existing user data, install the
  validated build on the host and verify capture, restart persistence and app
  switching. Publish the subsequent fork binary only after that verification;
  the prior release remains available for rollback.

## Scope and decisions left to implementation

This is a packet-history subsystem and packet-tab replacement, not an application
rewrite. No packet payload capture, automatic historical CSV import, automatic
firewall-policy changes, or recovery of events already discarded by the old
build. Preserve existing profiles and resource/icon fixes.

The builder chooses schema details, indexes, query cancellation strategy,
native-control arrangement, dependency integration, tests and exact batching
tuning within these requirements. The proposed retention defaults and the
interpretation of the mockup's grouping toggle must be presented for approval.
