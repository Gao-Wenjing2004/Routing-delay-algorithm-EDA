"""Verify the frozen K48 release without inference or external dependencies."""
from pathlib import Path
import hashlib
import json
import stat
import zipfile

ROOT = Path(__file__).resolve().parent


def identity(path):
    path = Path(path)
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block)
    return {'bytes': path.stat().st_size, 'sha256': h.hexdigest()}


def main():
    manifest = json.loads((ROOT / 'qa/release_manifest.json').read_text(encoding='utf-8'))
    files = {f'src/{name}': info for name, info in manifest['source_files'].items()}
    files.update(manifest['runtime_files'])
    files[manifest['build_script']['path']] = {k: manifest['build_script'][k] for k in ('bytes', 'sha256')}
    archive_info = manifest['submission_archive']
    files[archive_info['path']] = {k: archive_info[k] for k in ('bytes', 'sha256')}
    for name, expected in files.items():
        if identity(ROOT / name) != expected:
            raise SystemExit('Frozen-file identity mismatch: ' + name)
    archive = ROOT / archive_info['path']
    assert archive.stat().st_size < 100_000_000
    runtime = manifest['runtime_files']
    with zipfile.ZipFile(archive) as bundle:
        assert sorted(bundle.namelist()) == sorted(runtime), 'Unexpected ZIP members'
        assert bundle.testzip() is None, 'ZIP CRC mismatch'
        for name, expected in runtime.items():
            info = bundle.getinfo(name)
            assert info.file_size == expected['bytes'], name
            expected_mode = 0o755 if name == 'bin/estimate' else 0o644
            assert stat.S_IMODE(info.external_attr >> 16) == expected_mode, name
            h = hashlib.sha256()
            with bundle.open(name) as stream:
                for block in iter(lambda: stream.read(1 << 20), b''):
                    h.update(block)
            assert h.hexdigest() == expected['sha256'], name
    print(json.dumps({'passed': True, 'source_files': len(manifest['source_files']),
                      'runtime_files': len(runtime), 'submission_archive_bytes': archive.stat().st_size,
                      'archive_SHA256': archive_info['sha256'], 'inference_or_training_run': False}))


if __name__ == '__main__':
    main()
