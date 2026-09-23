"""Verify downloaded release bytes, ZIP mode, embedded asset and source snapshot."""
import hashlib,json,stat,struct,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()
package=json.loads((ROOT/'qa/package_report.json').read_text())
exe=ROOT/'bin/estimate';asset=ROOT.parent/'srb_fast_v7/endpoint_packed.bin'
assert digest(ROOT/'submission.zip')==package['sha256']
assert digest(exe)==package['executable_sha256']
with zipfile.ZipFile(ROOT/'submission.zip') as z:
    assert z.namelist()==['bin/estimate']
    info=z.getinfo('bin/estimate');assert stat.S_IMODE(info.external_attr>>16)==0o755
    h=hashlib.sha256()
    with z.open(info) as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk)
    assert h.hexdigest()==package['executable_sha256']
with exe.open('rb') as f:
    assert f.read(4)==b'\x7fELF'
    f.seek(-32,2);magic,offset,size=struct.unpack('<16sQQ',f.read(32))
    # The binary footer stores exactly 16 bytes, matching C++ memcmp(...,16).
    assert magic==b'SRB7ATLAS20260919'[:16] and offset%4096==0
    assert offset+size+32==exe.stat().st_size and size==asset.stat().st_size
    f.seek(offset);h=hashlib.sha256();left=size
    while left:
        chunk=f.read(min(left,1024*1024));assert chunk;h.update(chunk);left-=len(chunk)
    asset_sha=digest(asset);assert h.hexdigest()==asset_sha
sources={p.name:digest(p) for p in (ROOT/'src').iterdir() if p.is_file()}
for name,value in sources.items():assert value==digest(ROOT.parent/'srb_fast_v7'/name)
receipt={'zip_sha256':package['sha256'],'executable_sha256':package['executable_sha256'],
         'atlas_sha256':asset_sha,'zip_payload_crc_and_sha256_verified':True,
         'embedded_atlas_equals_build_asset':True,'source_sha256':sources}
(ROOT/'qa/local_receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps(receipt,indent=2))
