# Batch live packet-list updates without shipping SQLite

[Back to upstream candidates](Home.md) · **High impact · proposal only; do not cherry-pick the redesign**

## What changes and why

The fork investigation found 11,983–12,039 records in the old in-memory table despite a 900 source cap, plus five UI row-count queries taking 423–654 ms. It implicated per-event sort/resize/status/DNS work, concurrent list mutation and UI-owned retention, not a proven deadlock. fork.3 solves this via a larger persistent history redesign; upstream has since added grouping, but its current `_app_logwrite_ui` and worker still merit a separate burst test.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/log.c
+++ b/src/log.c
@@ -245,49 +237,6 @@ VOID _app_logwrite (
 
-VOID _app_logwrite_ui (
-	_In_ HWND hwnd,
-	_In_ PITEM_LOG ptr_log
-)
+VOID _app_logwrite_ui (HWND hwnd, PITEM_LOG ptr_log)
 {
-	ULONG_PTR table_size;
  ...
-	_r_queuedlock_releaseshared (&lock_loglist);
-
-	if (table_size >= MAP_CACHE_MAX)
-	{
-		item_id = _r_listview_getitemcount (hwnd, IDC_LOG) - 1;
  ...
@@ -1381,5 +1343,3 @@ VOID NTAPI _app_logthread (
 
-		// only for my own provider write log to a ui
-		if (ptr_log->is_myprovider && is_loguienabled)
-			_app_logwrite_ui (hwnd, ptr_log);
+		// Persistent history is handed off before this notification/CSV worker.
 	}
```

- `_app_logwrite_ui` (src/log.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/log.c#L246) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/log.c#L238)
- `_app_logthread` (src/log.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/log.c#L1324) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/log.c#L1532)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/4bf72620133eac9698c785f141c1f225be3de5ac) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 4bf72620 -- src/log.c src/packet_history.c
```

Source: [November 5 packet-log investigation](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/docs/investigations/2026-11-05-packet-log-freeze.md). [Upstream log grouping](https://github.com/henrypp/simplewall/commit/bfd4a5f4) does not by itself prove refresh batching or correct retention.

## Smallest upstream scope and acceptance check

Small upstream first step: retain existing list, CSV, and WFP capture semantics; schedule a single coalesced UI refresh and skip resizing/status for ordinary arrivals. Make the UI thread own row mutations and enforce the in-memory cap independently in a second patch. Compare fixed-rate burst with the tab visible/hidden, responsiveness, cap and visibility; do not substitute 200k/day spreadsheet math or a Windows Responding flag for evidence. The exact source diff shows where the fork removes old UI routing; the smaller batching suggestion is not an existing clean backport.

