# Large-list responsiveness: upstream submission brief

[Back to upstream candidates](Home.md) · **First upstream priority**

## Maintainer-facing summary

Large app and rule lists make some everyday actions slow because the UI retrieves the same sort data during comparisons and looks up affected rows separately for every application attached to a rule. Start by preparing sort keys once per existing row, then address rule-row refresh separately. Neither change needs a packet-history replacement, new dependencies, or a firewall-policy change.

## Exact split and code paths

| Order | Change | Fork code paths | What should stay the same |
| --- | --- | --- | --- |
| 1 | [Prepare sort keys once per row](Prepared-list-sort-keys.md) | `src/listview.c`: `_app_listview_sort`, `_app_listview_compare_callback`; `src/listview.h`: row context and key types | Checked-first precedence and sort direction; compare text, number and timestamp values with the original semantics. Preserve row identity, selection and grouping. |
| 2 | [Visit existing rows once for a bulk rule update](Bulk-rule-row-updates.md) | `src/profile.c`: `_app_setruleiteminfo`, `_app_setappiteminfo` | Same affected app IDs and check state. Scan Apps, Services and Packages with hash membership; measure the one-member case as well as a large rule. Leave the fork-only group insertion change out. |
| 3 | [Avoid redundant search refresh work](Search-less-list-work.md) | `src/search.c`: `_app_search_applyfilteritem`, `_app_search_applyfiltercallback`; `src/main.c`: search input and timer handling; `src/listview.c` / `src/listview.h`: shared update flags and checks | Same filter matches and relative ordering, with final visibility determining row updates. Isolate the visibility check from the 180 ms debounce if that makes review easier. |
| Later | [Advisory file existence](Async-file-availability.md), [rule membership index](Rule-membership-index.md), [metadata/redraw coalescing](Metadata-and-redraw.md) | Separate filesystem, cache invalidation and worker paths | Keep these out of the opening patch; test inaccessible files, save/exit lifetime and mutation handling independently. |

The combined fork change is [8a17a63b](https://github.com/Mainkill1/simplewall/commit/8a17a63b825d63c751ded48cc85fa26f21cd7f31), not an upstream cherry-pick. The linked pages show focused diff excerpts and pinned upstream code. For review in a clone with fork refs, extract only the named hunks:

```sh
git diff --function-context 24bbd2ce 8a17a63b -- src/listview.c src/listview.h
git diff --function-context 24bbd2ce 8a17a63b -- src/profile.c
git diff --function-context 24bbd2ce 8a17a63b -- src/search.c src/main.c
```

## How large is each PR?

Plan for **one behavior per PR**. Sizes count additions plus deletions in C source and headers, not net growth or test/docs lines. These are planning ranges for clean ports, not diffs already rebased or built. During sizing, upstream was at [`53654634`](https://github.com/henrypp/simplewall/commit/53654634da1fb15c834e7f7bf6190dc7056419bc), which only changed version constants since the audit pin.

| Separate PR | Estimated changed source lines |
| --- | ---: |
| Prepared sort keys | 200–260 |
| Single-pass affected rule rows | 20–35 |
| Search visibility and narrower refresh | 10–25 |
| Search debounce | 15–25 |
| Toolbar checked state | 30–45 |
| Async file availability | 65–100 |
| Metadata job deduplication and cleanup | 50–75 |
| Redraw batching | 40–65 |
| Rule membership index | 95–125 |
| Connection highlight index | 70–90 |

Range midpoint average: **72**; midpoint median: **58** (rounded). Without prepared sort keys, the average is **54**. The fork's scoped rule-row change is **+18/−4** (22 changed), and the toolbar slice is **+27/−9** (36 changed). The raw sort slice is **+117/−141** (258 changed), including a small adjacent redraw helper and excluding message wiring; do not treat it as a standalone patch. Excludes fork-only Rules tabs, investigation-only alert/packet-log work, and conditional Refresh behavior and UNC identity changes. Do not squeeze the sort's key preparation and comparator semantics into separate changes merely to meet a line quota.

## Evidence and submission gate

- **Behavior verified:** Fork portable tests for checkbox/numeric sort ordering, unchanged search visibility and rule-index reuse/invalidation passed. These use extracted production C and shims, not a running upstream Windows UI. The audit overall had 22 passes and 2 SDK-dependent skips.
- **Performance rationale:** Preparing one key per row avoids repeated control/model lookups from the comparison callback. It does not change the comparison sort's asymptotic complexity. The affected-row pass avoids repeated row searches for large memberships; at small memberships the old approach may be faster.
- **Not measured:** No matched Windows before/after time-to-first-paint or sort result was available. Historical Refresh command dispatch and old packet-log row query timings are different operations; don't use them as evidence for this speedup.
- **Before submission:** Build pinned upstream and upstream plus the first isolated patch using the same Windows SDK and Release settings. In the same VM, use the same anonymized 1k and 10k app/rule profiles and settings. Record initial tab paint, sort by name/timestamp (both directions), and selection/check behavior. Separately measure toggling a one-member rule and a large-membership rule for patch 2. Report median and worst repeated trial, sample size, and any baseline errors. Do not change WFP policy or bundle the new-app alert report with the list patch.

[New-app alert problem report](New-app-notification-stalls.md) · [Fork-only blank Rules tabs](Rule-tabs-not-blank.md)
