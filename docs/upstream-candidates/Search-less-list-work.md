# Debounce search and update only rows whose visibility changes

[Back to upstream candidates](Home.md) · **Send first · small after upstream search changes**

## What changes and why

Upstream's text-change handler filters immediately; a changed filter can cause sort, resize, and status work. Fork restarts a 180 ms timer and measures row changes by final visibility instead of intermediate match state. It avoids unnecessary work without changing what matches.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/search.c
+++ b/src/search.c
@@ -310,3 +310,3 @@ BOOLEAN _app_search_applyfiltercallback (
 	if (is_changed)
-		_app_listview_updateby_id (hwnd, listview_id, PR_UPDATE_NOSETVIEW | PR_UPDATE_FORCE);
+		_app_listview_updateby_id (hwnd, listview_id, PR_UPDATE_NOSETVIEW | PR_UPDATE_NOSORT | PR_UPDATE_NORESIZE | PR_UPDATE_NOSTATUS | PR_UPDATE_FORCE);
 
@@ -328,3 +328,3 @@ BOOLEAN _app_search_applyfilteritem (
 	PR_STRING string;
-	BOOLEAN is_changed = FALSE;
+	BOOLEAN is_changed = FALSE, was_hidden = !!context->is_hidden;
 
--- a/src/main.c
+++ b/src/main.c
@@ -2162,2 +2165,82 @@ INT_PTR CALLBACK DlgProc (
  ...
+				return 0;
+			}
+			if (wparam == TIMER_SEARCH)
+			{
+				PITEM_TAB_CONTEXT tab_context;
+				KillTimer (hwnd, TIMER_SEARCH);
+				tab_context = _app_listview_getcontext (hwnd, INT_ERROR);
+				if (tab_context)
  ...
@@ -2782,3 +2866,7 @@ INT_PTR CALLBACK DlgProc (
 
-				_app_search_applyfilter (hwnd, tab_context->listview_id, string);
+				KillTimer (hwnd, TIMER_SEARCH);
+				if (_r_obj_isstringempty (string))
+					_app_search_applyfilter (hwnd, tab_context->listview_id, string);
+				else
+					SetTimer (hwnd, TIMER_SEARCH, 180, NULL);
 
```

- `DlgProc` (src/main.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/main.c#L1952) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/main.c#L1958)
- `_app_search_applyfiltercallback` (src/search.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/search.c#L324) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/search.c#L284)
- `_app_search_applyfilteritem` (src/search.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/search.c#L356) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/search.c#L316)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/8a17a63b825d63c751ded48cc85fa26f21cd7f31) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 8a17a63b -- src/main.c src/search.c
```

## Smallest upstream scope and acceptance check

Rebase the timer hook onto 5f3546b1's in-progress search UI; take the final-visibility fix and narrow refresh flags, not the fork's IDC_LOG early return. Test fast typing, clearing, tab changes and selection; portable unchanged-row test passed. Check that group/status text is still meaningful when list refreshes are suppressed.

