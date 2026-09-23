"""Append the frozen Atlas to a Linux ELF; only the resulting ELF is submitted."""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct

parser = argparse.ArgumentParser()
parser.add_argument('elf', type=Path)
parser.add_argument('atlas', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
assert args.elf.resolve() != args.output.resolve()
with args.elf.open('rb') as f:
    assert f.read(4) == b'\x7fELF', 'input must be a Linux ELF'
with args.atlas.open('rb') as f:
    header = struct.unpack('<8s10I2Q', f.read(64))
assert header[:5] == (b'SRBE2E1\x00', 2, 496, 24, 72), header
assert header[10] == 10 and header[-2] % 16 == 0 and header[-1] % 16 == 0
assert args.atlas.stat().st_size == 64+2*sum(header[5:9])+22*(header[-2]+header[-1])//16
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.elf.open('rb') as src, args.output.open('wb') as dst:
    shutil.copyfileobj(src, dst, 1024*1024)
    dst.write(b'\x00' * (-dst.tell() % 4096))
    offset = dst.tell()
    with args.atlas.open('rb') as atlas:
        shutil.copyfileobj(atlas, dst, 1024*1024)
    dst.write(struct.pack('<16sQQ', b'SRB7ATLAS20260919'[:16], offset, args.atlas.stat().st_size))
args.output.chmod(0o755)
h = hashlib.sha256()
with args.output.open('rb') as f:
    for block in iter(lambda: f.read(1024*1024), b''):
        h.update(block)
print('executable_bytes={} atlas_offset={} sha256={}'.format(args.output.stat().st_size, offset, h.hexdigest()))
