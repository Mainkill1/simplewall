# Refresh affected app rows in one pass

[Back to upstream candidates](Home.md) · **Next · small**

## What changes and why

Instead of asking the control to locate a row separately for every member of a large rule, iterate existing rows once per app tab and test hash membership. The fork also batches redraw locking around each list pass.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/profile.c
+++ b/src/profile.c
@@ -936,6 +952,19 @@ VOID _app_setruleiteminfo (
 
-	while (_r_obj_enumhashtable (ptr_rule->apps, NULL, &hash_code, &enum_key))
+	for (INT list_id = IDC_APPS_PROFILE; list_id <= IDC_APPS_UWP; list_id++)
 	{
-		if (hash_code != 0)
  ...
+		{
+			hash_code = (ULONG)_app_listview_getitemcontext (hwnd, list_id, i);
+			if (!_r_obj_findhashtable (ptr_rule->apps, hash_code))
+				continue;
+			ptr_app = _app_getappitem (hash_code);
+			if (ptr_app)
+			{
+				_app_setappiteminfo (hwnd, list_id, i, ptr_app);
+				_r_obj_dereference (ptr_app);
+			}
  ...
@@ -1323,24 +1362,131 @@ BOOLEAN _app_isapphaverule (
  ...
+		for (ULONG_PTR i = 0; i < _r_obj_getlistsize (rules_list); i++)
 		{
-			if (_r_obj_findhashtable (ptr_rule->apps, app_hash))
+			ptr_rule = (PITEM_RULE)_r_obj_getlistitem (rules_list, i);
+			if (!ptr_rule || ptr_rule->type != DATA_RULE_USER)
  ...
+
+			enum_key = 0;
+			while (_r_obj_enumhashtable (ptr_rule->apps, NULL, &hash_code, &enum_key))
 			{
-				is_haverule = TRUE;
  ...
```

- `_app_setruleiteminfo` (src/profile.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/profile.c#L919) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/profile.c#L980)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/8a17a63b825d63c751ded48cc85fa26f21cd7f31) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 8a17a63b -- src/profile.c
```

## Smallest upstream scope and acceptance check

Port independently of the rule membership index. For a tiny rule and a huge list, a direct lookup might still win; compare one-app and many-app rules, and check checkbox/icon refresh across Apps, Services, and Packages. Do not market it as an unconditional speedup.

