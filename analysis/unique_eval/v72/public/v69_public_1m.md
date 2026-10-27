# Unique evaluation report

## Accuracy

- Rows: `1000000`
- Accuracy: `98.3781031501`
- Conservative grouped 95% interval: `[98.2308291224, 98.5135687994]`
- MAE: `13.649233 ps`
- Exact share: `51.496300%`

The interval is the envelope of source, source-region and target-region cluster bootstraps; it is intentionally wider than a random-row interval.

## Official-score projection

- Center runtime: `117.710 s`
- Center total: `97.721569`
- Conservative range: `[97.603750, 97.829942]`
- Time/score reference quality: `scenario-only`

This is a projection, not a hidden-platform score. The range combines grouped accuracy uncertainty with the observed benchmark run range.

## Segment accuracy

### distance

| Group | Rows | Accuracy |
|---|---:|---:|
| 0-16 | 15611 | 98.36153965 |
| 129-256 | 306892 | 98.14475585 |
| 17-32 | 38602 | 98.87459779 |
| 257+ | 272711 | 97.88466516 |
| 33-64 | 121672 | 98.88880423 |
| 65-128 | 244512 | 98.88987047 |

### block_count

| Group | Rows | Accuracy |
|---|---:|---:|
| 0 | 1000000 | 98.37810315 |

### gap_count

| Group | Rows | Accuracy |
|---|---:|---:|
| 0 | 1000000 | 98.37810315 |
