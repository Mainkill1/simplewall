Windows x64 performance and compatibility release based on upstream 3.9.2.

- Cache rule lookups and move availability checks off the UI thread.
- Make Refresh asynchronous without rebuilding firewall filters.
- Fix notification toggle state, startup crashes, missing icons, and packet/connection rows.
- Coalesce profile saves and preserve synchronous export/shutdown behavior.

Validation: 16 regression tests with sanitizers, native Windows ABI tests, and live host verification of packet logging, connection rows and responsive Refresh. The executable is the exact host-tested binary.

This is a custom fork build using the public SDK compatibility adapter. The executable is unsigned.
