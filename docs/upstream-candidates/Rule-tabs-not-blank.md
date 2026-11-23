# Give Rules rows a real group at insertion

[Back to upstream candidates](Home.md) · **Send first · small, reproduce first**

## What changes and why

Blocklist, System Rules, and User Rules could look empty if the group-enabled control rejected insertion with a callback group. Assign an existing group when inserting, honor the returned row index and free the context if insertion fails.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/listview.c
+++ b/src/listview.c
@@ -128,2 +128,16 @@ VOID _app_listview_addappitem (
 
+INT _app_listview_getrulegroup (
+	_In_ PITEM_RULE ptr_rule,
+	_In_ BOOLEAN is_hidden
  ...
@@ -146,4 +161,9 @@ VOID _app_listview_addruleitem (
 
-	_r_listview_additem (hwnd, listview_id, item_id, LPSTR_TEXTCALLBACK, I_IMAGECALLBACK, I_GROUPIDCALLBACK, _app_listview_createcontext (rule_idx));
-	_app_setruleiteminfo (hwnd, listview_id, item_id, ptr_rule, is_forapp);
+	context = _app_listview_createcontext (rule_idx);
+	item_id = _r_listview_additem (hwnd, listview_id, item_id, LPSTR_TEXTCALLBACK, I_IMAGECALLBACK, _app_listview_getrulegroup (ptr_rule, FALSE), context);
+
+	if (item_id != INT_ERROR)
+		_app_setruleiteminfo (hwnd, listview_id, item_id, ptr_rule, is_forapp);
+	else
+		_app_listview_destroycontext ((PITEM_LISTVIEW_CONTEXT)context);
 
```

- `_app_listview_getrulegroup` (src/listview.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/listview.c#L129)
- `_app_listview_addruleitem` (src/listview.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/listview.c#L129) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/listview.c#L143)
- `_app_setruleiteminfo` (src/profile.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/profile.c#L919) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/profile.c#L980)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 2b0a5ca6 -- src/listview.c src/profile.c
```

## Smallest upstream scope and acceptance check

Keep the shared group resolver and consistent hidden/disabled/special/global mapping. Do not change rule evaluation or packet-log groups. Fork test passed; verify native tabs including disabled and app-scoped rules with upstream's current SDK. If native upstream already inserts correctly, pitch defensive returned-index handling only or drop it.

