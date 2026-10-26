# Unique evaluation report

## Accuracy

- Rows: `608000`
- Accuracy: `98.3027576768`
- Conservative grouped 95% interval: `[98.0451169787, 98.5225741768]`
- MAE: `14.504334 ps`
- Exact share: `50.717928%`

The interval is the envelope of source, source-region and target-region cluster bootstraps; it is intentionally wider than a random-row interval.

## Segment accuracy

### distance

| Group | Rows | Accuracy |
|---|---:|---:|
| 0-16 | 9119 | 98.63491508 |
| 129-256 | 192350 | 98.13967175 |
| 17-32 | 22556 | 99.21097336 |
| 257+ | 170743 | 97.64139580 |
| 33-64 | 70589 | 98.89904751 |
| 65-128 | 142643 | 98.85438889 |

### block_count

| Group | Rows | Accuracy |
|---|---:|---:|
| 0 | 608000 | 98.30275768 |

### gap_count

| Group | Rows | Accuracy |
|---|---:|---:|
| 0 | 608000 | 98.30275768 |
