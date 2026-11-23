# Keep file-existence checks out of painting and status

[Back to upstream candidates](Home.md) · **Next · moderate**

## What changes and why

Highlighting and status can touch slow paths while the window paints. The fork queues one existence check per pending row, uses a cached advisory result in UI paths and keeps the authoritative check for purge/count decisions. Unknown is provisionally treated as present until checked.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/controls.c
+++ b/src/controls.c
@@ -1059,3 +1070,3 @@ VOID _app_refreshstatus (
 
-	_app_getcount (&status);
+	_app_getcount_ui (&status);
 
--- a/src/profile.c
+++ b/src/profile.c
@@ -712,3 +712,3 @@ COLORREF _app_getappcolor (
 	{
-		if (_r_config_getboolean (L"IsHighlightInvalid", TRUE, L"colors") && !_app_isappexists (ptr_app))
+		if (_r_config_getboolean (L"IsHighlightInvalid", TRUE, L"colors") && !_app_isappexists_ui (ptr_app))
 		{
@@ -878,2 +879,16 @@ VOID _app_getcount (
  ...
+}
+
+VOID _app_getcount_ui (
+	_Out_ PITEM_STATUS status
+)
  ...
```

- `_app_getcount_internal` (src/profile.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/profile.c#L866)
- `_app_isappexists_ui` (src/profile.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/profile.c#L1489)
- `_app_queue_exists` (src/profile.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/profile.c#L1473)
- `_app_refreshstatus` (src/controls.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/controls.c#L1040) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/controls.c#L1048)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/8a17a63b825d63c751ded48cc85fa26f21cd7f31) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 8a17a63b -- src/profile.c src/controls.c
```

## Smallest upstream scope and acceptance check

Submit the display cache separately from Refresh and metadata extraction. Test inaccessible shares, disconnected drives, deletion while pending, failed queueing and redraw only on changes. Never use stale cache to remove an entry or select a WFP identity. Portable UI isolation/dedup test passed.

