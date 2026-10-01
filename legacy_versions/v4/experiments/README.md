# V4 experimental residual kernels

This directory contains opt-in research assets. The normal `build_v4.ps1` does not include them and keeps the 2,784-parameter compact V4 model.

- `train_v4_family_residual.py`: trains either `port_family_residual` or `port_family_compact` from the fixed training side only.
- `v4_family_residual_data.hpp`: 216-family model with family-pair and family-distance effects.
- `v4_family_compact_data.hpp`: smaller family-pair plus source/target-family effects.

Build an experimental executable by adding exactly one macro:

```powershell
g++ -O3 -DNDEBUG -DSRB_V4_ENABLE_FAMILY_RESIDUAL srb_fast_v4\estimate_v4.cpp -o estimate_v4_family.exe
g++ -O3 -DNDEBUG -DSRB_V4_ENABLE_FAMILY_COMPACT srb_fast_v4\estimate_v4.cpp -o estimate_v4_family_compact.exe
```

Run it with `--mode v4 --residual-kernel family`. Do not define both macros. These candidates use Golden-derived residual effects and are not architecture-only predictors; only the port-to-family mapping is architecture-derived.
