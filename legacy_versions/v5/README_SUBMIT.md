# V5 single-executable submission

The submission package contains only `estimate.exe`.

```powershell
.\estimate.exe -in .\delay_estimate_request.csv -out .\delay_estimate_result.csv
```

Geometry, local Atlas, and endpoint costs are regenerated in memory on every run. No adjacent `.bin` file and no Golden CSV are required.
