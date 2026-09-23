"""Run inside the supplied judge image, with only the release binary required."""
import csv
import hashlib
import itertools
import json
import os
from pathlib import Path
import resource
import shutil
import statistics
import subprocess
import time
import zipfile

ROOT = Path(__file__).resolve().parent
QA = ROOT/'qa'
EXE = ROOT/'bin/estimate'
LIMIT = 2_000_000_000
report = {'memory_limit_bytes': LIMIT, 'runs': [], 'checks': {}}

def digest(path, name='sha256'):
    h = hashlib.new(name)
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()

def limit_memory():
    resource.setrlimit(resource.RLIMIT_AS, (LIMIT, LIMIT))

def run(args, name, ok=True, executable=EXE, cwd='/tmp'):
    start = time.perf_counter()
    rss, threads = 0, 0
    with (QA/(name+'.stderr.txt')).open('wb') as log:
        p = subprocess.Popen([str(executable)]+[str(x) for x in args], cwd=cwd,
                             stdout=subprocess.DEVNULL, stderr=log, preexec_fn=limit_memory)
        while p.poll() is None:
            try:
                fields = dict(line.split(':', 1) for line in Path('/proc/{}/status'.format(p.pid)).read_text().splitlines())
                rss = max(rss, int(fields.get('VmHWM', '0 kB').split()[0])*1024)
                threads = max(threads, int(fields.get('Threads', '0')))
            except (OSError, ValueError):
                pass
            time.sleep(.005)
        code = p.returncode
    elapsed = time.perf_counter()-start
    log = (QA/(name+'.stderr.txt')).read_text()
    if ok:
        assert code == 0, (name, code, log)
        assert threads <= 1, (name, threads)
        assert rss < LIMIT, (name, rss)
    else:
        assert code != 0, (name, 'unexpected success')
    print(name, 'PASS', round(elapsed, 6), 'seconds', flush=True)
    return {'wall_seconds': elapsed, 'sampled_peak_rss_bytes': rss,
            'sampled_max_threads': threads, 'exit_code': code, 'stderr': log.strip()}

inp, out = QA/'requests_1m.csv', QA/'actual_1m.csv'
for i in range(5):
    result = run(['-in', inp, '-out', out], 'million_{}'.format(i+1))
    result['sha256'] = digest(out)
    result['md5'] = digest(out, 'md5')
    report['runs'].append(result)
assert len({x['md5'] for x in report['runs']}) == 1
report['checks']['five_run_md5_identical'] = True

rows = 0
with inp.open() as fi, out.open() as fo:
    ri, ro = csv.reader(fi), csv.reader(fo)
    assert next(ri) == ['From', 'To']
    assert next(ro) == ['From', 'To', 'Delay']
    for a,b in itertools.zip_longest(ri,ro):
        assert a is not None and b is not None, 'row count mismatch'
        assert len(b) == 3 and a == b[:2], (rows, a, b)
        assert b[2].isdigit(), (rows,b)
        rows += 1
assert rows == 1_000_000
report['checks']['row_count'] = rows
report['checks']['exact_header_order_and_integer_delay'] = True
baseline = QA/'external_reference_1m.csv'
assert digest(out) == digest(baseline), 'embedding changed V7 predictions'
report['checks']['embedded_equals_external_atlas_predictions'] = True

with inp.open('rb') as f:
    next(f)
    requests = [next(f).rstrip(b'\r\n') for _ in range(257)]
with out.open('rb') as f:
    next(f)
    expected_body = b''.join(next(f) for _ in range(257))
header = b'From,To,Delay\n'
small, actual = QA/'small.csv', QA/'small_actual.csv'
cases = {
    'crlf_bom_no_final_newline': (b'\xef\xbb\xbfFrom,To\r\n'+b'\r\n'.join(requests), expected_body),
    'empty_file': (b'', b''),
    'header_only': (b'From,To\n', b''),
    'byte_block_boundary': (b'From,To\n'+(b'\n'.join(requests)+b'\n')*650, expected_body*650),
}
for name,(data,expected) in cases.items():
    small.write_bytes(data)
    run(['-out',actual,'-in',small], name)
    assert actual.read_bytes() == header+expected, name
    report['checks'][name] = True
small.write_bytes(b'From,To\n'+requests[0]+b'\n')
before = small.read_bytes()
run(['-in',small,'-out',small], 'same_file_rejected', ok=False)
assert small.read_bytes() == before
run(['-in',small,'-out',actual,'-threads','2'], 'multithread_rejected', ok=False)
run(['-in',QA/'does_not_exist.csv','-out',actual], 'missing_input_rejected', ok=False)
run(['-in',small,'-out','/dev/full'], 'write_error_rejected', ok=False)
run([], 'missing_arguments_rejected', ok=False)
symlink = QA/'renamed_estimator'
if symlink.is_symlink():
    symlink.unlink()
symlink.symlink_to(EXE)
run(['-in',small,'-out',actual], 'symlink_foreign_cwd', executable=symlink)
assert actual.read_bytes() == header+expected_body.split(b'\n')[0]+b'\n'
report['checks']['standalone_binary_foreign_cwd_and_symlink'] = True
report['checks']['failure_exits'] = True
report['max_child_rss_bytes'] = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss*1024
assert report['max_child_rss_bytes'] < LIMIT
report['mean_1m_wall_seconds'] = statistics.mean(r['wall_seconds'] for r in report['runs'])
report['exe_sha256'] = digest(EXE)
report['exe_bytes'] = EXE.stat().st_size
report['external_reference_sha256'] = digest(baseline)
report['prediction_sha256'] = digest(out)
report['environment'] = subprocess.check_output(['bash','-c','cat /etc/os-release; g++ --version | head -n 1; ldd /workspace/bin/estimate'], text=True)
report['caveat'] = '1M public rows only; not a hidden evaluation or a measured billion-row test. This final model was trained on the public 1M.'
(QA/'validation_report.json').write_text(json.dumps(report, indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k not in ('runs','environment')},indent=2), flush=True)
