# Large app list implementation progress

Plan: `2026-11-05-large-app-lists.md`.

Implemented:

- Rule membership index with invalidation for enable, membership, deletion, insertion and reload changes.
- UI file existence cache with asynchronous checks, while purge/count decisions retain fresh checks.
- Single-pass bulk rule row updates and prepared sort keys using stable row contexts.
- Search debounce and final visibility comparison; filtering skips unnecessary sorts, measurement and status recounts.
- Deduplicated metadata jobs, reference cleanup on errors, 30-second failure backoff and coalesced redraws.
- Per-app connection lookup rebuilt once per network poll, with redraws only when membership changes.
- Coalesced immutable profile snapshots, ordered background writes through a temporary file, synchronous export, and explicit callback completion at shutdown/import.
- Public SDK build adapter restored from upstream history; corrected current logging, timestamp, toolbar and Z-order API mappings. Removed four unused resource declarations whose image files are absent from this checkout.

Validation:

- Baseline and optimized Release x64 builds succeed with MSVC v143 and Windows SDK 10.0.26100.
- Twelve C behavioral regression tests pass, including AddressSanitizer and UndefinedBehaviorSanitizer runs.
- Tests cover index reuse/invalidation, sort ordering, unchanged search visibility, UI filesystem isolation, metadata dedup/backoff/cleanup, save routing, failed write preservation and write completion waits.
- Independent review identified stale exports, save requests routed to notification windows, and the old SDK's dequeue-only queue wait. All three corrected and covered by tests/review of the export path.
- Installed executable SHA-256: `34c2d93086def5fab1872ae2220d67e2c67670f3ec0c58f87dfaf231bfaf8c7a`.
- Host installation: `C:\Users\main\AppData\Local\Programs\simplewall-performance\simplewall.exe`; desktop shortcut created. Existing profile/settings files were not overwritten.

Host follow-up verification:

- Desktop dumps `simplewall-1793883131.dmp` and `simplewall-1793883146.dmp` both recorded access violations at `simplewall.exe+0x70b5a`, symbolized as `_r_res_verqueryvalue` called by `_app_getfileversioninfo`. The adapter missed the output-first argument mapping for `_r_res_queryversion`, causing a write into read-only version-resource data.
- Added version-query and SID-conversion mappings. A native Windows regression reproduced `0xC0000005` with the crashing adapter and passes with the corrected mappings.
- Toolbar text localization passed `I_DEFAULT` (-8) into the old SDK, which interpreted it as an actual image index. The adapter now translates the preserve-image sentinel; its regression failed before and passes after the fix.
- Corrected build installed and restarted. Verified process 32004 running the installed executable and responding, with toolbar and rule icons visibly restored. No new dumps appeared after restart; original dumps retained.
- Existing profile/settings files preserved. Host firewall state remains under user control. The synthetic large-profile UI benchmark is not run while the user has filters enabled. Portable operation-count tests verify that repeated rule lookups rebuild once and repeated UI existence queries queue once, without disk checks on the UI thread.


Notification toggle and Refresh follow-up:

- Replaced toolbar press-state updates with explicit checked-state updates, clearing the transient pressed bit. Startup, settings, tray and toolbar actions now share the same helper.
- Refresh/F5 now queues a coalesced, low-priority availability check. It preserves app objects, rows, sort order, selection and tab; it neither reloads the profile nor rebuilds WFP filters. File checks run after releasing the apps lock.
- Fifteen behavioral tests pass with address/undefined-behavior sanitizers. Independent review reported no material findings.
- Installed build SHA-256: `c60f0251b59acc98fe5d29a90d8e5a62d573ece7518af38604760da682c294e6`.
- Native UI test verified notification states `4 -> 5 -> 4` (enabled/off -> enabled/on -> enabled/off), restored the original preference, and confirmed all list counts, selections and the active tab survived 20 Refresh commands. Maximum command dispatch was 1.275 ms; mean was 0.262 ms on this host. These measure UI command return, not completion of background filesystem checks.


Packet log follow-up:

- The restarted process had an active WFP subscription, an idle log worker queue, and 145 records in its internal log table, but zero visible log rows. Native WFP enumeration independently confirmed blocked test traffic from simplewall's provider.
- The current application's `INT_ERROR` append sentinel was forwarded verbatim by the public SDK to `LVM_INSERTITEM`; Windows rejected the negative index. The same path prevented connection rows from appearing.
- The compatibility mapping now translates append into the current list item count while preserving explicit insertion indexes and row context. The production packet-row test failed before the mapping and passes after it. A native Windows list control likewise failed before and passes after the fix.
- Sixteen portable regression tests pass with address/undefined-behavior sanitizers; the native SDK ABI regression passes. Release x64 builds cleanly.
- Updated build installed and running as process 19800, SHA-256 `4f476b73c270769b100c2909031539f3146a98841c8fad6f6a77a43dba512ebf`.
- Live blocked-connection test increased visible packet-log rows from 11 to 44; the Connections view contained 153 rows. Notification toggle and soft Refresh checks passed again during this deployment. Original user preferences were restored after testing.
