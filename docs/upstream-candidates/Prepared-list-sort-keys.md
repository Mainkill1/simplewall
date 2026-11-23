# Prepare list sort keys once per row

[Back to upstream candidates](Home.md) · **Next · moderate, isolated**

## What changes and why

In the old comparator, many comparisons fetched UI text, checked state, or looked up app state repeatedly. The fork builds a key array once and compares prepared values through stable row contexts. This reduces expensive control/model lookups from comparator frequency to one preparation pass; the sort itself is still O(n log n).

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/listview.c
+++ b/src/listview.c
@@ -884,147 +905,25 @@ INT CALLBACK _app_listview_compare_callback (
  ...
-			if (ptr_log1)
-				_r_obj_dereference (ptr_log1);
+	key1 = &sort->keys[item1->sort_index];
+	key2 = &sort->keys[item2->sort_index];
 
-			if (ptr_log2)
  ...
@@ -1080,3 +991,74 @@ VOID _app_listview_sort (
  ...
+			continue;
+
+		context->sort_index = i;
+		app_hash = (ULONG)context->id_code;
+		key->is_checked = has_checkboxes && _r_listview_isitemchecked (hwnd, listview_id, i);
  ...
+	}
+
+	// LVM_SORTITEMS compares stable row contexts, never moving row indexes.
+	SendMessageW (hlistview, LVM_SORTITEMS, (WPARAM)&sort, (LPARAM)&_app_listview_compare_callback);
+
+	for (INT i = 0; i < item_count; i++)
  ...
```

- `_app_listview_compare_callback` (src/listview.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/listview.c#L884) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/listview.c#L923)
- `_app_listview_sort` (src/listview.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/listview.c#L1037) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/listview.c#L954)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/8a17a63b825d63c751ded48cc85fa26f21cd7f31) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 8a17a63b -- src/listview.c
```

## Smallest upstream scope and acceptance check

Carry checkbox precedence, timestamp/numeric ordering, case behavior, row context lifetime, and order direction. Do not fold in profile I/O or packet tables. Portable ordering test passed; benchmark sort for 1k/10k rows on Windows and explicitly test ties and invalid/missing rows. No independent shell-to-paint speedup measurement exists.

