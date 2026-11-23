# Reduce new-app prompt stalls without changing firewall behavior

[Back to upstream candidates](Home.md) · **High impact · small first experiment; worker/persistence follow-up**

## What changes and why

Stock discovery inserts an app and synchronously saves the profile from the log path; notification commands also save inline. Fork coalesces UI save requests (500 ms), captures immutable serialized bytes on the UI thread and writes via an ordered worker and `.pending` file. This removes inline disk writes in those paths, not all UI stalls. UI snapshot capture still costs time. fork.3 uses synchronous `SendMessageW` for main-thread discovery and the headless fallback still saves synchronously.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/log.c
+++ b/src/log.c
@@ -1323,2 +1442,91 @@ VOID CALLBACK _wfp_logcallback4 (
  ...
+	{
+		_app_listview_updateby_id (hwnd, app->type, PR_UPDATE_TYPE);
+		_app_profile_requestsave (hwnd);
+	}
+
  ...
@@ -1341,12 +1549,12 @@ VOID NTAPI _app_logthread (
  ...
-		if (ptr_app)
+		if (hwnd)
+			is_autoallowed = (BOOLEAN)SendMessageW (hwnd, RM_LOG_DISCOVER_APP, 0, (LPARAM)ptr_log);
+		else
 		{
  ...
-				_app_listview_updateby_id (hwnd, ptr_app->type, PR_UPDATE_TYPE);
-
-			_app_profile_save (hwnd);
+			ptr_app = _app_addapplication (NULL, DATA_UNKNOWN, ptr_log->path, NULL, NULL);
+			if (ptr_app)
  ...
--- a/src/main.c
+++ b/src/main.c
@@ -37,3 +39,3 @@ BOOLEAN _app_changefilters (
 
-	_app_profile_save (hwnd);
+	_app_profile_requestsave (hwnd);
 
@@ -420,3 +422,3 @@ VOID _app_config_apply (
 
-			_app_profile_save (hwnd);
+			_app_profile_requestsave (hwnd);
 
--- a/src/notifications.c
+++ b/src/notifications.c
@@ -115,3 +115,3 @@ BOOLEAN _app_notify_command (
 
-	_app_profile_save (hwnd);
+	_app_profile_requestsave (hwnd);
 
@@ -1221,3 +1223,3 @@ INT_PTR CALLBACK NotificationProc (
 
-						_app_profile_save (hwnd);
+						_app_profile_requestsave (hwnd);
 
```

- `_app_logthread` (src/log.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/log.c#L1324) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/log.c#L1532)
- `_app_notify_command` (src/notifications.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/notifications.c#L56) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/notifications.c#L56)
- `_app_profile_requestsave` (src/profile.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/profile.c#L1970)
- `_app_profile_write_snapshot` (src/profile.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/profile.c#L1924)
- `DlgProc` (src/main.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/main.c#L1952) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/main.c#L1958)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/8a17a63b825d63c751ded48cc85fa26f21cd7f31) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 2b0a5ca6 -- src/log.c src/notifications.c src/profile.c src/main.c
```

Separate evidence: [unmerged burst hardening commit](https://github.com/Mainkill1/simplewall/commit/fc6c41b719d0a912f189e8797eeb66af6c440992) and [fork.3 discovery code](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/log.c#L1496-L1559). Do not confuse an earlier branch with released code.

## Smallest upstream scope and acceptance check

First instrument first-block → popup paint, UI message latency during the prompt, and decision → filter apply; run with live packet logging separately on/off. Start with a behavior-preserving coalesced save or post-only show path, not automatic allow. The unmerged `fc6c41b7` branch is a reference for posting notification display and queuing WFP apply, not a shippable patch: review lifetime, stale decisions, queue drain, and ordering before considering it. If moving disk writes, keep previous file on failure, flush same-generation work before import/exit, and verify export freshness and filter decision semantics. Portable save routing, completion and failure tests passed; the fork's packet backlog measurement is not a new-app popup benchmark.

