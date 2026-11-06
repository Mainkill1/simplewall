# Packet log freeze investigation

User request: replace the packet stream with an app list and details for one
selected app, handling approximately 200,000 events per day. History must survive
restarts (confirmed by user).

## Evidence from the installed build

Read-only elevated diagnostics against process 19800 on November 5, 2026:

- WFP event subscription active; file logging disabled.
- First sample: `log_id=28663`, six packet workers, five busy,
  `log_table count=11983`.
- Second sample: `log_id=28752`, `log_table count=12039`, and
  7,185–7,191 packet list rows.
- Five synchronous row-count queries to the actual main dialog took
  617, 612, 654, 491 and 423 milliseconds. The process's `Responding` flag was
  true, so that flag alone does not demonstrate usable UI responsiveness.
- Initial row-count probes used the process's `MainWindowHandle`, which did not
  contain the packet control; those failed probes are excluded. Enumerating
  process windows located the main dialog with control 113.

## Code paths responsible for excessive work and unsafe retention

- `src/main.c` initializes six concurrent packet workers.
- `_app_logwrite_ui` in `src/log.c` performs duplicate checking, count checking,
  control-based eviction and insertion in separate operations. They are not one
  atomic retention operation. Eviction depends on querying/deleting a live UI
  row, and failure to retrieve a row hash does not prevent another insertion.
- These workers directly modify the same Win32 list. The append compatibility
  mapping also reads the row count separately from insertion. UI state and
  retained record state can diverge. Actual cache size exceeds its configured
  source-code cap (`MAP_CACHE_MAX`, 900) by more than 13 times.
- Each accepted record calls `_app_listview_updateby_id` with zero flags. On the
  active packet tab this requests redraw, font/view setup, group refresh, full
  sorting, column measurement and status refresh. Requests are posted per event,
  without coalescing. Column measurement and sort-key preparation read list cells.
- Every retained record queues both-address resolution, even if not being
  inspected. Resolver completions redraw the list.
- `_app_getloghash` groups endpoint tuples but omits timestamp, allow/block and
  filter identity. Existing matching events are silently discarded rather than
  recorded or counted. This cannot provide an accurate event timeline.

These observations establish excessive per-event UI work, divergent retention,
and concurrent control mutation. They do not establish a specific deadlocked
thread stack; no debugger attachment or process termination was performed.

## Redesign requirements established so far

- Show apps first; selecting an app fetches only its details.
- Persist history across restarts.
- Enforce retention independently per app, outside the controls.
- Bound ingestion queues and UI result sets; expose any dropped records.
- Keep storage, querying and cleanup off the UI thread; coalesce UI updates.
- Distinguish app identity by full executable path or package identity, preserving
  unknown/system traffic rather than dropping it.
- Validate with at least 200,000 synthetic events, a dominant noisy app, restart
  recovery, retention isolation and repeated app switching while events arrive.

Individual-event retention versus aggregate-only storage is awaiting the user's
choice. Retention defaults, persistent storage implementation and final design
are not yet approved. No product code or installed executable has changed during
this investigation.
