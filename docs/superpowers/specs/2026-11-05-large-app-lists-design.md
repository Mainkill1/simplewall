# Large app list performance

Implement the performance audit approved by the user, preserving app grouping, natural text sorting, rule membership, profile contents, and firewall decisions. Optimize the existing Win32 UI instead of replacing it with an owner-data control: that avoids changing grouping and selection behavior. Use cached read models with explicit invalidation, batched UI updates, and background file information. Keep deletion/purge decisions fresh. Save requests may be coalesced, with exit flushing and explicit export remaining synchronous.

Build and regression-test on Windows, then deploy the executable and resources to a dedicated host folder (or back up and replace an existing installation if discovered). Preserve existing profiles. Launch the UI without automatically enabling or disabling firewall filters.
