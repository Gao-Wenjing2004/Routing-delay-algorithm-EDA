# Unique evaluation report

## Accuracy

- Rows: `608000`
- Accuracy: `98.3901329267`
- Conservative grouped 95% interval: `[98.1247390389, 98.6135260634]`
- MAE: `13.729000 ps`
- Exact share: `52.341941%`

The interval is the envelope of source, source-region and target-region cluster bootstraps; it is intentionally wider than a random-row interval.

## Segment accuracy

### distance

| Group | Rows | Accuracy |
|---|---:|---:|
| 0-16 | 9119 | 98.63491508 |
| 129-256 | 192350 | 98.29313584 |
| 17-32 | 22556 | 99.21097336 |
| 257+ | 170743 | 97.74170315 |
| 33-64 | 70589 | 98.89904751 |
| 65-128 | 142643 | 98.89980686 |

### block_count

| Group | Rows | Accuracy |
|---|---:|---:|
| 0 | 608000 | 98.39013293 |

### gap_count

| Group | Rows | Accuracy |
|---|---:|---:|
| 0 | 608000 | 98.39013293 |
