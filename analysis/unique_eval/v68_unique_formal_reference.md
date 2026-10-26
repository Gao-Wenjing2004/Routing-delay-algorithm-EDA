# Unique evaluation report

## Accuracy

- Rows: `608000`
- Accuracy: `98.6853086346`
- Conservative grouped 95% interval: `[98.4255990139, 98.9046824298]`
- MAE: `11.045238 ps`
- Exact share: `57.566612%`

The interval is the envelope of source, source-region and target-region cluster bootstraps; it is intentionally wider than a random-row interval.

## Official-score projection

- Center runtime: `185.559 s`
- Center total: `97.401918`
- Conservative range: `[96.954722, 97.585721]`
- Time/score reference quality: `release-reference`

This is a projection, not a hidden-platform score. The range combines grouped accuracy uncertainty with the observed benchmark run range.

## Segment accuracy

### distance

| Group | Rows | Accuracy |
|---|---:|---:|
| 0-16 | 9119 | 98.63491508 |
| 129-256 | 192350 | 98.75540253 |
| 17-32 | 22556 | 99.21097336 |
| 257+ | 170743 | 98.16487959 |
| 33-64 | 70589 | 98.89908791 |
| 65-128 | 142643 | 99.02804704 |

### block_count

| Group | Rows | Accuracy |
|---|---:|---:|
| 0 | 305329 | 99.05800751 |
| 1 | 100705 | 98.26326824 |
| 2 | 79102 | 98.59585549 |
| 3+ | 122864 | 98.16263092 |

### gap_count

| Group | Rows | Accuracy |
|---|---:|---:|
| 0 | 9151 | 97.36680290 |
| 1 | 29193 | 98.26449645 |
| 2 | 47738 | 98.67139649 |
| 3 | 59578 | 98.75531595 |
| 4 | 63925 | 98.72990225 |
| 5+ | 398415 | 98.73047011 |
