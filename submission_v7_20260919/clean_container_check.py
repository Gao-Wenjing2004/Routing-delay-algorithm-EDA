"""Run on the Linux build host: extract ZIP, then expose only its binary to judge image."""
import hashlib,json,os,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parent
QA=ROOT/'qa';CLEAN=QA/'clean';CLEAN.mkdir(exist_ok=True)
common=['podman','run','--rm','--network','none','--security-opt','label=disable']
subprocess.run(common+['-v',str(ROOT)+':/workspace','-w','/workspace','fpga-ziguang:latest',
    'python3','-c','import zipfile,stat; from pathlib import Path; z=zipfile.ZipFile("submission.zip"); i=z.getinfo("bin/estimate"); mode=stat.S_IMODE(i.external_attr>>16); assert z.namelist()==["bin/estimate"] and mode==0o755; z.extractall("qa/unpacked"); Path("qa/unpacked/bin/estimate").chmod(mode)'],check=True)
cmd=common+['--read-only','-v',str(QA/'unpacked')+':/submit:ro',
    '-v',str(QA/'requests_1m.csv')+':/input.csv:ro','-v',str(CLEAN)+':/output',
    '-w','/tmp','fpga-ziguang:latest','python3','-c',
    'import os,resource; resource.setrlimit(resource.RLIMIT_AS,(2000000000,2000000000)); os.execv("/submit/bin/estimate",["/submit/bin/estimate","-in","/input.csv","-out","/output/result.csv"])']
start=time.perf_counter();result=subprocess.run(cmd,capture_output=True,check=True)
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
    return h.hexdigest()
assert digest(CLEAN/'result.csv')==digest(QA/'actual_1m.csv')
report={'read_only_container':True,'network':'none','mounted_release_contains_only_binary':True,
        'memory_address_space_limit':2000000000,'archive_executable_mode':oct((QA/'unpacked/bin/estimate').stat().st_mode&0o777),
        'output_identical_to_validated_million':True,'output_sha256':digest(CLEAN/'result.csv'),
        'wall_seconds_including_container_start':time.perf_counter()-start,'stderr':result.stderr.decode()}
(QA/'clean_container_report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
