Windows x64 release with persistent, app-focused packet history. Includes the prior large-app-list performance fixes.

- Native split view based on the supplied UI design. Apps move to the top by latest blocked activity while the selected app stays stable.
- History survives restart; switch between original events and grouped repeats, filter by action/direction, and inspect one app at a time.
- Adjustable per-app retention, paged results, bounded queues, one background writer, cached reverse DNS, and explicit loss/eviction reporting.
- SQLite 3.54.1 is statically bundled; no extra runtime installation is needed.

Validation: 200,000-event production-store tests, grouped/raw/app paging, fair retention, metadata and reader-pinned journal pressure, injected rollback failures, sanitizer runs, native Windows store/SDK ABI tests, and real host UI checks. Live traffic was captured; restart preserved all 947 previously committed events; rapid filter-switch probes peaked at 2.4021 ms. The executable is the exact host-tested build.

Defaults: 200,000 events per app, seven days, 1 GiB global budget. Oldest records expire first; storage pressure can evict across apps. The previous discarded session cache cannot be recovered.

Custom unsigned build using the public SDK compatibility adapter. Back up profile/history before replacing installations; close the app before copying its database.
