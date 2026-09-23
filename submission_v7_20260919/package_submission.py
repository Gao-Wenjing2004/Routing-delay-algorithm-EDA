"""Create a portable ZIP with a single Linux executable and Unix mode 0755."""
import hashlib
import json
from pathlib import Path
import shutil
import stat
import zipfile

root = Path(__file__).resolve().parent
exe = root/'bin/estimate'
archive = root/'submission.zip'
with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    info = zipfile.ZipInfo('bin/estimate', (2026,9,19,0,0,0))
    info.create_system = 3
    info.external_attr = (stat.S_IFREG | 0o755) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    with exe.open('rb') as src, z.open(info, 'w', force_zip64=True) as dst:
        shutil.copyfileobj(src, dst, 1024*1024)
with zipfile.ZipFile(archive) as z:
    assert z.namelist() == ['bin/estimate']
    assert z.testzip() is None
    info = z.getinfo('bin/estimate')
    assert stat.S_IMODE(info.external_attr >> 16) == 0o755
    digest = hashlib.sha256()
    with z.open(info) as f:
        for block in iter(lambda: f.read(1024*1024), b''):
            digest.update(block)
    assert digest.hexdigest() == json.loads((root/'qa/validation_report.json').read_text())['exe_sha256']
h = hashlib.sha256()
with archive.open('rb') as f:
    for block in iter(lambda: f.read(1024*1024), b''):
        h.update(block)
report = {'archive': archive.name, 'bytes': archive.stat().st_size,
          'sha256': h.hexdigest(), 'entries': ['bin/estimate'], 'unix_mode': '0755',
          'executable_sha256': digest.hexdigest(), 'crc_and_payload_hash_verified': True}
(root/'qa/package_report.json').write_text(json.dumps(report,indent=2)+'\n')
(root/'submission.zip.sha256').write_text(h.hexdigest()+'  submission.zip\n')
print(json.dumps(report,indent=2),flush=True)
