# Normalize UNC executable identities for log and WFP

[Back to upstream candidates](Home.md) · **Useful if requested · moderate; WFP review required**

## What changes and why

The fork normalizes UNC, Win32/NT UNC, MUP, and LanmanRedirector forms and constructs a native WFP path for identified share executables without requiring a reachable share. This can align a logged executable and its rule instead of skipping or duplicating it.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/wfp.c
+++ b/src/wfp.c
@@ -2131,3 +2131,19 @@ NTSTATUS _FwpmGetAppIdFromFileName1 (
 
-	if (type == DATA_APP_REGULAR || type == DATA_APP_NETWORK || type == DATA_APP_SERVICE)
+	if (type == DATA_APP_NETWORK)
+	{
+		// WFP identifies share executables by their native MUP name. Build the full
+		// identity even when the share is disconnected or the file is inaccessible.
+		path_root = _app_getnetworkapppath (path->buffer);
  ...
--- a/src/share_path.h
+++ b/src/share_path.h
@@ -0,0 +1,81 @@
  ...
+}
+
+static inline const wchar_t *sw_share_path_tail(const wchar_t *path)
+{
+    const wchar_t *tail = sw_share_prefix_tail(path);
  ...
```

- `sw_share_path_tail` (src/share_path.h): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/share_path.h#L63)
- `_app_getnetworkapppath` (src/helper.c): [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/helper.c#L567)
- `_FwpmGetAppIdFromFileName1` (src/wfp.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/wfp.c#L2119) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/wfp.c#L2119)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 2b0a5ca6 -- src/share_path.h src/helper.c src/wfp.c
```

## Smallest upstream scope and acceptance check

Keep conservative parsing and verify that displayed identity, profile reload, and actual WFP match agree for UNC and mapped SMB. Test unrelated same-named executables, invalid forms, disconnected shares and local paths. Portable prefix tests passed; no new Windows/WFP proof. Do not put this into the first easy PR series because an identity mismatch affects filter semantics.

