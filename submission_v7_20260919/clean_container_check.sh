#!/bin/sh
set -eu
cd "$(dirname "$0")"
release_dir=$(pwd)
mkdir -p qa/clean
podman run --rm --network none --security-opt label=disable \
  -v "$release_dir:/workspace" -w /workspace fpga-ziguang:latest \
  python3 -c 'import zipfile,stat; from pathlib import Path; z=zipfile.ZipFile("submission.zip"); i=z.getinfo("bin/estimate"); mode=stat.S_IMODE(i.external_attr>>16); assert z.namelist()==["bin/estimate"] and mode==0o755; z.extractall("qa/unpacked"); Path("qa/unpacked/bin/estimate").chmod(mode)'
podman run --rm --network none --security-opt label=disable --read-only \
  -v "$release_dir/qa/unpacked:/submit:ro" \
  -v "$release_dir/qa/requests_1m.csv:/input.csv:ro" \
  -v "$release_dir/qa/clean:/output" -w /tmp fpga-ziguang:latest python3 -c \
  'import os,resource; resource.setrlimit(resource.RLIMIT_AS,(2000000000,2000000000)); os.execv("/submit/bin/estimate",["/submit/bin/estimate","-in","/input.csv","-out","/output/result.csv"])' \
  2>qa/clean/stderr.txt
podman run --rm --network none --security-opt label=disable \
  -v "$release_dir:/workspace" -w /workspace fpga-ziguang:latest python3 -c '
import hashlib,json
from pathlib import Path
q=Path("qa")
def digest(p):
 h=hashlib.sha256()
 with p.open("rb") as f:
  for c in iter(lambda:f.read(1048576),b""):h.update(c)
 return h.hexdigest()
actual=digest(q/"clean/result.csv")
assert actual==digest(q/"actual_1m.csv")
mode=oct((q/"unpacked/bin/estimate").stat().st_mode&0o777)
assert mode=="0o755"
r=dict(read_only_container=True,network="none",mounted_release_contains_only_binary=True,memory_address_space_limit=2000000000,archive_executable_mode=mode,output_identical_to_validated_million=True,output_sha256=actual,stderr=(q/"clean/stderr.txt").read_text())
(q/"clean_container_report.json").write_text(json.dumps(r,indent=2)+"\n")
print(json.dumps(r,indent=2))
'
