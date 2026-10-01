# V5 data provenance

| Artifact/data | Source | Generated when | Uses public Golden? |
|---|---|---|---:|
| `local_arch_data.hpp` | Official Port/Arc/Net/Gap/Block JSON | Before compilation | No |
| Geometry array | Embedded raw architecture | Every `estimate` run, in memory | No |
| Local Atlas | Embedded raw architecture | Every `estimate` run, in memory | No |
| Endpoint model | Runtime-generated Local Atlas | Every `estimate` run, in memory | No |
| `estimate.exe` | Source plus raw architecture constants | Compilation | No |
| files under `output/` | Request CSV and local validation | Test only | May contain predictions; never read as model input |

The submitted executable opens only the `-in` request CSV and `-out` result CSV. It does not open any `.bin`, `delay_estimate_ans.csv`, or Golden file.
