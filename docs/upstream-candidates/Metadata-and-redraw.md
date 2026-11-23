# Deduplicate file metadata jobs and coalesce redraw

[Back to upstream candidates](Home.md) · **Next · moderate; two PRs if needed**

## What changes and why

Repeated tooltip/icon/version reads could queue duplicate jobs. The fork uses atomic pending-state transitions, cleans up references and backs off failures for 30 seconds. It also posts/coalesces redraw work instead of updating the control for each completion.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/helper.c
+++ b/src/helper.c
@@ -1691,17 +1684,13 @@ VOID _app_getfileinformation (
  ...
+	state = InterlockedCompareExchange (&ptr_app_info->load_state, 0, 0);
+	if ((state == 0 || (state == 2 && !ptr_app_info->is_loaded &&
+		_r_sys_gettickcount64 () >= (ULONG64)_InterlockedCompareExchange64 (&ptr_app_info->retry_after, 0, 0))) &&
+		InterlockedCompareExchange (&ptr_app_info->load_state, 1, state) == state)
 		_r_workqueue_queueitem (&file_queue, &_app_queue_fileinformation, ptr_app_info);
  ...
@@ -1769,3 +1758,3 @@ VOID NTAPI _app_queue_fileinformation (
 	if (hwnd && _r_wnd_isvisible (hwnd, FALSE) && ptr_app_info->listview_id != 0)
-		_r_listview_redraw (hwnd, ptr_app_info->listview_id);
+		_app_listview_requestredraw (hwnd, ptr_app_info->listview_id);
 
--- a/src/main.c
+++ b/src/main.c
@@ -2162,2 +2165,82 @@ INT_PTR CALLBACK DlgProc (
  ...
+			InterlockedExchange (&config.refresh_pending, 0);
+			if (tab_context)
+				_app_listview_requestredraw (hwnd, tab_context->listview_id);
+			_app_refreshstatus (hwnd);
+			return 0;
  ...
+		case RM_LISTVIEW_REDRAW:
+		{
+			SetTimer (hwnd, TIMER_REDRAW, 100, NULL);
+			return 0;
+		}
  ...
+				return 0;
+			}
+			if (wparam == TIMER_REDRAW)
+			{
+				PITEM_TAB_CONTEXT tab_context = _app_listview_getcontext (hwnd, INT_ERROR);
+				LONG pending = InterlockedExchange (&config.redraw_pending, 0);
+				KillTimer (hwnd, TIMER_REDRAW);
+				for (INT i = IDC_APPS_PROFILE; i <= IDC_LOG; i++)
+				{
  ...
```

- `_app_getfileinformation` (src/helper.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/helper.c#L1661) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/helper.c#L1724)
- `_app_queue_fileinformation` (src/helper.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/helper.c#L1728) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/helper.c#L1778)
- `_app_listview_requestredraw` (src/listview.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/listview.c#L1077)
- `DlgProc` (src/main.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/main.c#L1952) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/main.c#L1958)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/8a17a63b825d63c751ded48cc85fa26f21cd7f31) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 8a17a63b -- src/helper.c src/listview.c src/main.c
```

## Smallest upstream scope and acceptance check

First port job state, reference cleanup and retry limits; then batch redraw on the UI thread with explicit timer/post failure and shutdown handling. Close the app or remove rows with jobs pending; test repeated hover on missing/slow paths. Portable dedup/backoff and reference cleanup tests passed.

