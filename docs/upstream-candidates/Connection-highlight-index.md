# Index connected app hashes per network poll

[Back to upstream candidates](Home.md) · **Next · moderate**

## What changes and why

Upstream scans connection rows to answer whether each app has a connection. Fork builds a per-poll app-hash set and compares membership before redrawing app highlights. It avoids repeatedly scanning the same connection collection.

## Code difference

Selected verbatim diff lines from the fork (omissions marked). This is an illustration, not an apply-ready patch. The merge base, not current upstream, is the left side; the pinned upstream links show what a new PR should target.

```diff
--- a/src/network.c
+++ b/src/network.c
@@ -174,2 +174,3 @@ PITEM_NETWORK_CONTEXT _app_network_getcontext ()
 		network_context->checker_ptr = _r_obj_createhashtablepointer (0x100);
+		network_context->connected_apps = _r_obj_createhashtable (sizeof (ULONG), 0x100, NULL);
 
@@ -784,21 +791,6 @@ BOOLEAN _app_network_isapphaveconnection (
  ...
-	}
-
+	result = _r_obj_findhashtable (network_context->connected_apps, app_hash) != NULL;
 	_r_queuedlock_releaseshared (&network_context->lock_network);
-
  ...
```

- `_app_network_isapphaveconnection` (src/network.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/network.c#L773) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/network.c#L783)
- `_app_network_printlistviewtable` (src/network.c): [upstream](https://github.com/henrypp/simplewall/blob/8c8bbbae82d42c325632c2cfe868679abfa000fa/src/network.c#L866) · [fork.3](https://github.com/Mainkill1/simplewall/blob/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db/src/network.c#L858)

[Fork commit, full context](https://github.com/Mainkill1/simplewall/commit/2b0a5ca6891f6c0f57387e29cf22d4db3e1880db) · [Pinned upstream baseline](https://github.com/henrypp/simplewall/tree/8c8bbbae82d42c325632c2cfe868679abfa000fa)

Exact local diff, from a clone with the fork commits:

```sh
git diff 24bbd2ce 2b0a5ca6 -- src/network.c
```

## Smallest upstream scope and acceptance check

Keep the index advisory, preserve protocol/state eligibility including current IPv6 behavior, and publish it under the correct lock. Test multiple sockets per app, disconnects, all-empty and equal-size/different-membership changes. No native measurement here; leave PID/service attribution out of the patch.

