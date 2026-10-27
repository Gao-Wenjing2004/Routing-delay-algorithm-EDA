# Unique evaluation report

## Accuracy

- Rows: `608000`
- Accuracy: `95.7071313450`
- Conservative grouped 95% interval: `[95.3056028504, 96.0631650789]`
- MAE: `34.422951 ps`
- Exact share: `31.310033%`

The interval is the envelope of source, source-region and target-region cluster bootstraps; it is intentionally wider than a random-row interval.

## Segment accuracy

### distance

| Group | Rows | Accuracy |
|---|---:|---:|
| 0-16 | 9119 | 98.63491508 |
| 129-256 | 192350 | 94.85603689 |
| 17-32 | 22556 | 99.21097336 |
| 257+ | 170743 | 94.36638454 |
| 33-64 | 70589 | 98.89873670 |
| 65-128 | 142643 | 96.13903335 |

### block_count

| Group | Rows | Accuracy |
|---|---:|---:|
| 0 | 608000 | 95.70713134 |

### gap_count

| Group | Rows | Accuracy |
|---|---:|---:|
| 0 | 608000 | 95.70713134 |
