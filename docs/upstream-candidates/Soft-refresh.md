# Make Refresh recheck availability without rebuilding firewall state

[Back to upstream candidates](Home.md) · **Discuss first · small design change; moderate implementation**

## What changes and why

Current upstream Refresh loads the profile and calls `_app_changefilters`. The fork queues one availability scan, does file checks outside the apps lock, and preserves tabs, rows, and selection without reinstalling filters.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/main.c
+++ b/src/main.c
@@ -2162,2 +2165,82 @@ INT_PTR CALLBACK DlgProc (
 
+		case RM_REFRESH_COMPLETE:
+		{
+			PITEM_TAB_CONTEXT tab_context = _app_listview_getcontext (hwnd, INT_ERROR);
  ...
@@ -3127,4 +3217,3 @@ INT_PTR CALLBACK DlgProc (
 
-					_app_profile_load (hwnd, NULL);
-					_app_changefilters (hwnd, TRUE, FALSE);
+					_app_command_refresh (hwnd);
 
--- a/src/messages.c
+++ b/src/messages.c
@@ -2978 +2985,11 @@ VOID _app_command_selectfont (
 }
+
+VOID _app_command_refresh (
+	_In_ HWND hwnd
+)
+{
+	if (InterlockedCompareExchange (&config.refresh_pending, 1, 0) != 0)
+		return;
+
+	_r_workqueue_queueitem (&refresh_queue, &_app_queue_refresh, hwnd);
+}
```

- `DlgProc` (src/main.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/main.c#L1952) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/main.c#L1958)
- `_app_command_refresh` (src/messages.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/messages.c#L2996)
- `_app_queue_refresh` (src/profile.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/profile.c#L1512)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/8a17a63b825d63c751ded48cc85fa26f21cd7f31) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 8a17a63b -- src/main.c src/messages.c src/profile.c
```

Source: [historical fork Windows validation](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/docs/superpowers/plans/2026-11-05-large-app-lists-progress.md).

## Smallest upstream scope and acceptance check

Do not silently remove a maintainer-intended reload/reapply command. Prefer a separate Recheck app files action or a split Refresh/Reapply workflow. Past fork host evidence: 20 commands retained tab and selection; max dispatch 1.275 ms, mean 0.262 ms. This measured dispatch, not scan completion or baseline speedup. Portable dedup and lock/object-lifetime tests passed.

