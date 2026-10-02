# V9 ablation summary

- Validation slice: deterministic 20,000 rows from the public 1M, seed `20261002`.
- V8 validation accuracy: `94.670749`.
- Full ALT A*, cap 4096: 590/20,000 exact completions; all-search accuracy
  `95.344051`; mean added cost `5470.05 us/input`; negative projected total score.
- Experimental bidirectional search, cap 4096: 121/20,000 completions; accuracy
  `94.891012`; also negative projected total score.
- Public short set: 15,611 rows with Chebyshev distance at most 16.
- Compact kernel and full graph with port-state lower bound, cap 16:
  identical completion flag, expansion count and exact value on all 15,611 rows.
- Selected submission policy: deterministic structural micro-patterns, cap 16;
  public 1M selected/completed `10/10`, 52 expansions.
- Accuracy: V8 `94.603473234`; V9 `94.604410091`.
- Five-run Windows 1M medians: V8 `0.940940 s`; V9 `0.942671 s`. Exact-search
  internal time was roughly `0.2–0.4 ms` per 1M; wall-time difference is within
  normal run noise.

Conclusion: the hybrid mechanism is safe and compact, but the measured accuracy
gain is only `+0.000936857` point. The next meaningful work is graph contraction
and portal/path-suffix reuse, not increasing the raw expansion cap.
