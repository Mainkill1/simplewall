# Cache rule membership for large status and list scans

[Back to upstream candidates](Home.md) · **Next · moderate; invalidation is the work**

## What changes and why

Upstream's `_app_isapphaverule` scans user rules for each lookup. Fork builds an app-hash index with separate bits for any membership and enabled membership, rebuilding when the mutation generation changes. Useful when many rows or status checks hit the same unchanged rules.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/profile.c
+++ b/src/profile.c
@@ -1318,2 +1352,7 @@ BOOLEAN _app_isapphavedrive (
 
+VOID _app_rulecache_invalidate ()
+{
+	InterlockedIncrement (&rule_cache_generation);
+}
+
 BOOLEAN _app_isapphaverule (
@@ -1323,24 +1362,131 @@ BOOLEAN _app_isapphaverule (
  ...
+	static R_QUEUED_LOCK cache_lock = PR_QUEUED_LOCK_INIT;
+	static PR_HASHTABLE membership = NULL;
+	static LONG built_generation = 0;
 	PITEM_RULE ptr_rule;
-	BOOLEAN is_haverule = FALSE;
  ...
+	_r_queuedlock_acquireshared (&cache_lock);
+
+	if (membership && built_generation == generation)
+	{
+		entry = (ULONG*)_r_obj_findhashtable (membership, app_hash);
  ...
 
-	for (ULONG_PTR i = 0; i < _r_obj_getlistsize (rules_list); i++)
+	if (!membership || built_generation != generation)
 	{
-		ptr_rule = (PITEM_RULE)_r_obj_getlistitem (rules_list, i);
  ...
-				break;
+				entry = (ULONG*)_r_obj_findhashtable (membership, hash_code);
+				flags = (entry ? *entry : 0) | 1 | (ptr_rule->is_enabled ? 2 : 0);
+				if (entry)
+					*entry = flags;
  ...
```

- `_app_isapphaverule` (src/profile.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/profile.c#L1319) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/profile.c#L1406)
- `_app_rulecache_invalidate` (src/profile.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/profile.c#L1401)
- `_app_setruletoapp` (src/helper.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/helper.c#L1166) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/helper.c#L1227)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/8a17a63b825d63c751ded48cc85fa26f21cd7f31) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 8a17a63b -- src/profile.c src/helper.c
```

## Smallest upstream scope and acceptance check

Audit every enable, edit, membership change, delete, insert, import and reload; preserve enabled/disabled semantics and the rules-before-cache lock order. If the maintainer wants a smaller first patch, compute a per-refresh map instead of taking on global invalidation. Portable reuse/invalidation test passed; no current-upstream native benchmark.

