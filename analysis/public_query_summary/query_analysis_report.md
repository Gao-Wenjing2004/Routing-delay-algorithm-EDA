# Public SRB Query Structure and Reuse Analysis

> Scope: Full analysis of all 1,000,000 input rows.
> Architecture fingerprint: `6510489f75c3b8b7168891bcae4f493e037c1cc228351dde4e6429dd28526541`

## 1. Data source and parsing rules

- Input columns: `From, To, delay`.
- Parsed: 1,000,000; failed: 0. Failed rows and reasons are retained in `parse_failures.csv`.
- Endpoint parsing matches the baseline solver's semantic lookup: split `SRB_instance/PORT`, then require the instance name in `SRB_Inst.json` and the full port name in `SRB_Port.json`.
- Full port names retain direction letters, span family, bus index, Logic port identity, and Routing-Input status (Net target or not).
- Exact Query key is `(architecture fingerprint, canonical From, canonical To)`. Because this run uses one immutable architecture, counts are computed from the two endpoint identities.

Architecture summary:

| Item | Value |
|---|---:|
| Grid extent | 120 x 550 |
| Existing SRB instances | 60,200 |
| Missing cells / Block cells | 5,800 / 5,800 |
| Ports (Input / Output / Routing Input) | 496 (208 / 288 / 160) |
| Arcs / Nets / Gap Lines / Blocks | 8,192 / 160 / 19 / 7 |
| Maximum configured Net span | 12 |

## 2. Core results

| Metric | Value | Meaning |
|---|---:|---|
| Successful requests | 1,000,000 | denominator for request ratios |
| Unique Exact Queries | 1,000,000 | absolute endpoint pairs |
| Exact cache hits after first | 0 (0.0000%) | safe on the same architecture |
| Unique From | 903,129 | full absolute From endpoints |
| `1 - unique_from / total` | 9.6871% | search-count reduction ceiling |
| Requests whose From occurs >=2 | 18.7330% | request coverage, not the prior metric |
| Unique To | 956,421 | full absolute To endpoints |
| `1 - unique_to / total` | 4.3579% | reverse search-count reduction ceiling |
| Requests whose To occurs >=2 | 8.5343% | request coverage |
| Unique geometry templates | 999,440 | `(full ports, dx, dy)` |
| Geometry-template repeat upper bound | 0.0560% | not a safe delay-cache claim |
| Conservative safe template hits | 0.0000% | equals Exact Cache because absolute origin is fixed |
| BBox Block-related | 477,308 (47.7308%) | geometric classification |
| Endpoint-span Gap-related | 984,696 (98.4696%) | current V3 prefix geometry, not exact-path count |
| Actual V3 Atlas | 90182 (9.0182%) | exact unchanged branch classifier when available |
| Actual V3 model fallback | 909818 (90.9818%) | V3 has no full-search fallback |

## 3. From/To tree and batching potential

A full tree per distinct From would replace at most 96,871 independent per-query searches; a reverse tree per distinct To would replace at most 43,579. After Exact de-duplication, the corresponding grouping reductions are 96,871 and 43,579.

| K | Source-tree request coverage | Target reverse-tree request coverage |
|---:|---:|---:|
| 10 | 0.0046% | 0.0041% |
| 50 | 0.0206% | 0.0201% |
| 100 | 0.0406% | 0.0390% |
| 500 | 0.1768% | 0.1590% |
| 1000 | 0.3268% | 0.3090% |

These are theoretical request coverages. They do not prove that one full Dijkstra tree is faster than several target-directed A* searches, nor do they include tree memory cost.

## 4. Geometry templates, distance, and periodic evidence

Adding `(dx mod 8, dy mod 8)` to a key already containing exact `dx,dy` changes the unique count by **0**; the fields are deterministic and therefore redundant. The non-redundant periodic analysis uses `(|dx| mod 8, |dy| mod 8)` groups and Golden residuals.

Distance quantiles:

| Metric | P50 | P75 | P90 | P95 | P99 | Max |
|---|---:|---:|---:|---:|---:|---:|
| Manhattan | 199.00 | 310.00 | 411.00 | 462.00 | 535.00 | 653.00 |
| Chebyshev | 157.00 | 268.00 | 368.00 | 419.00 | 488.00 | 549.00 |
| Euclidean | 165.15 | 273.05 | 371.21 | 421.97 | 490.92 | 556.19 |

Golden descriptive fit: the port-pair-controlled directional linear baseline has RMSE 116.717 ps and R2 0.993730. Adding Manhattan hinge terms at 8/16/32/64/128/256 reduces in-sample RMSE by 0.0737%. Subtracting the 64 absolute mod-8 group means reduces in-sample RMSE by 0.4132%.

This is evidence of association only. The same public rows define and evaluate the residual groups; it is not hidden-set evidence and does not prove an 8x8 graph isomorphism or a safe Macro cache.

## 5. Block and Gap distinctions

- Endpoint bbox intersects a Block: 477,308 (47.7308%). This is exactly the current V3 `safe_regular_region` rejection condition.
- A valid candidate path with exactly one Net exists: 19; among them 0 touch a Block under baseline Net counting.
- Actual shortest-path Block influence is not available from the endpoint-only million-row Golden file. It is intentionally left `null`, not equated with bbox intersection or one-Net intersection.
- Endpoint span crosses a Gap Line for 984,696 requests. 521 raw geometry templates occur with multiple Gap signatures, directly showing that raw geometry alone can mix environments.
- The endpoint prefix sum is the exact term used by current V3, but the public endpoint rows do not prove that every exact shortest path crosses each Gap exactly according to endpoint displacement.

## 6. Existing V3 branch coverage

| Branch | Requests | Ratio | Mean Manhattan | Block ratio | Gap ratio |
|---|---:|---:|---:|---:|---:|
| atlas | 90,182 | 9.0182% | 42.70 | 0.0000% | 88.4412% |
| fallback_distance | 889,982 | 88.9982% | 240.99 | 51.4373% | 99.5811% |
| fallback_block | 19,525 | 1.9525% | 61.00 | 100.0000% | 94.1972% |
| fallback_other | 311 | 0.0311% | 65.39 | 0.0000% | 93.8907% |

The current V3 uses Atlas or the learned O(1) fallback. It does not invoke the baseline full A*/Dijkstra solver; therefore full-search count is zero in this executable, not a claim that exact search is unnecessary. Branch labels are reproduced by source-equivalent classification without changing solver behavior; per-branch production latency was not instrumented in this phase.

## 7. Cache/structure coverage interpretation

- **Semantically safe cache key, but no public-set hits:** Exact Query reuse is safe on the same architecture fingerprint; this public dataset contains zero repeated Exact Queries.
- **Repeatable but not yet safely reusable:** raw `(portA,portB,dx,dy)` geometry and bbox-conditioned templates.
- **Structured opportunity pools:** Chebyshev <= 8/16/32 Macro candidates, no-bbox-Block long requests for a periodic model, and bbox-Block requests for a Portal prototype.
- **Still requires validation or fallback:** every candidate for which translation invariance, boundary behavior, Gap separability, Block topology, and port compatibility have not been proved. Current V3 fallback coverage is reported separately when classifier labels are present.

The conservative environment signature includes exact source boundary distances; consequently it fixes the absolute source coordinate and collapses to Exact Query reuse. Any higher template reuse reported by weaker signatures is an opportunity upper bound, not a safe cache hit rate.

## 8. Correctness checks

- Random endpoint round-trip checks: 100 rows; all passed: `True`.
- Exact packed-key reversible check: `True`.
- Template packed-key reversible check: `True`.
- Parse/orientation/Gap/distance conservation checks: `True`, `True`, `True`, `True`.
- Configured Block cells exactly equal missing instance coordinates: `True`.
- V3 metadata discrepancy: configured maximum Net span is 12, while `srb_hybrid.hpp` declares 8. Because lookup bounds are checked, this currently indicates avoidable fallback risk rather than demonstrated wrong delay.

## 9. What these data cannot prove

1. Equal geometry templates have equal exact delay across translations.
2. A shortest path stays inside the endpoint bounding box or crosses Gap Lines only according to endpoint displacement.
3. An 8x8 period is exact merely because some Net families have bounded spans or residual means vary by mod 8.
4. One complete source/target tree is faster or smaller than the current A* workload.
5. A Block Portal graph preserves exact delay without explicit portal/state construction and Golden-path tests.
6. Public-data residual improvements generalize to hidden evaluation data.

## 10. Next 5 experiments

1. Construct a controlled replay workload by duplicating public Exact Queries; verify deterministic equality, architecture-version invalidation, and measured cache overhead without treating the synthetic hit rate as public-data evidence.
2. For Top-10/50/100 From and To, compare full-tree build/query time and memory against the same queries under current A*.
3. Select repeated ordinary templates across different origins; run exact solver pairs stratified by boundary clearance, Gap signature, and Block relation to measure true invariance violations.
4. Build an 8x8 vs 16x16 vs 32x32 local Macro prototype on a small port subset, evaluate out-of-origin exact agreement, and inspect periodic residual stability on held-out regions.
5. Instrument a separate exact-solver research binary to label actual path Block/Gap crossings; only then size Portal and Gap-prefix opportunities.

## 11. Reproduction

```powershell
& 'C:\Users\yanggoo\anaconda3\python.exe' `
    'tools\analyze_public_queries.py' `
    --input 'plusone-srb_fast_v3\srb_fast_v3\arch\delay_estimate_ans.csv' `
    --arch-dir 'plusone-srb_fast_v3\srb_fast_v3\arch' `
    --output-dir 'analysis\public_queries_full' `
    --seed 20260827 `
    --detailed-block-gap `
    --atlas-file 'plusone-srb_fast_v3\srb_fast_v3\local_atlas.bin'
```

- Analysis elapsed time: 26.528 seconds.
- Peak process RSS: 946.047 MiB.
- These values cover parsing, statistics, CSV/JSON generation, and chart generation; they do not include an external exact solver run.

## 12. Charts

### From To Frequency Long Tail

![From To Frequency Long Tail](charts/from_to_frequency_long_tail.png)

### Distance Distribution

![Distance Distribution](charts/distance_distribution.png)

### Port Pair Heatmap

![Port Pair Heatmap](charts/port_pair_heatmap.png)

### Dx Dy Distribution

![Dx Dy Distribution](charts/dx_dy_distribution.png)

### Mod8 Distribution

![Mod8 Distribution](charts/mod8_distribution.png)

### Template Reuse By Distance

![Template Reuse By Distance](charts/template_reuse_by_distance.png)

### Block Gap Request Ratios

![Block Gap Request Ratios](charts/block_gap_request_ratios.png)

### V3 Branch Coverage

![V3 Branch Coverage](charts/v3_branch_coverage.png)

