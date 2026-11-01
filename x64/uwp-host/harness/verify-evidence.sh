#!/usr/bin/env bash
# Host-only evidence validation; never converts a failed acceptance into PASS.
set -euo pipefail
cd "$(dirname "$0")"
python3 - <<'PY'
import hashlib,json,pathlib,re,subprocess,sys
base=pathlib.Path('/workspace/x64-run')
logs=base/'v2/R4H'
errors=[]
def require(ok,message):
    if not ok: errors.append(message)
commands=['backend-test','shim-test','wine-surface-check','wine-test','windows-futex-test','windows-smoke-test','evidence-replay']
for n,target in enumerate(commands,1):
    order=f'O{n}'
    try:
        video=base/f'videos/R4H-{order}.mp4'
        probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_format','-of','json',str(video)]))
        duration=float(probe['format']['duration'])
        require(any(s['codec_type']=='video' for s in probe['streams']) and 0<duration<=60,f'{order}: invalid video/duration')
        log=(logs/f'{order}-record.log').read_text()
        rc=int((logs/f'{order}-record.exit').read_text())
        require(f'Command: make -C x64/uwp-host/harness {target}' in log,f'{order}: missing acceptance command provenance')
        require('R4H terminal proof on Linux box (not Xbox device)' in log,f'{order}: missing capture provenance')
        require(f'Recorded command exit status: {rc}' in log,f'{order}: terminal/status mismatch')
        require(rc==0,f'{order}: recorded acceptance command exit={rc}')
        result=(base/f'RESULT-R4H-{order}.md').read_text()
        require(bool(re.search(r'^\|.*(?:THROUGH|Routing|Coverage|Test)',result,re.M|re.I)),f'{order}: missing coverage table')
        print(f'{order}: video={duration:.6f}s command exit={rc}; coverage/provenance inspected')
    except (OSError,ValueError,KeyError,subprocess.CalledProcessError) as e: errors.append(f'{order}: {e}')
try:
    regression=(logs/'O7-regression.log').read_text()
    require((logs/'O7-all.exit').read_text().strip()=='0','parent build failed')
    require((logs/'O7-regression.exit').read_text().strip()=='0','O7 required regression failed')
    require('backend tests: PASS' in regression and 'counter verification: PASS' in regression,'backend/shim checks missing')
    run=pathlib.Path((logs/'O7-wine-evidence.path').read_text().strip())
    require(f'Raw evidence retained: {run}' in regression,'run path not tied to regression log')
    require(subprocess.call([sys.executable,'verify_counters.py',str(logs/'O7-negative.report'),str(logs/'O7-positive.report'),str(logs/'O7-exit.report')])==0,'ELF positive/negative counter verification failed')
    require('569 passed, 0 failed' in (logs/'O6-full.log').read_text(),'Windows smoke results missing')
    # Full verifier independently checks hashes, real module/PID attribution,
    # positive counters, stock negative controls, bindings and wineboot attempts.
    rc=subprocess.call([sys.executable,'verify_wine.py',str(run)])
    require(rc==0,'real ntdll routing / no-wineboot gate failed')
    manifest=logs/'O7-recordings.sha256'
    for line in manifest.read_text().splitlines():
        digest,name=line.split('  ',1)
        require(hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest()==digest,f'recording changed: {name}')
except (OSError,ValueError) as e: errors.append(str(e))
if errors:
    for error in errors: print('BLOCKED: '+error)
    print('evidence verification: BLOCKED')
    sys.exit(1)
print('evidence verification: PASS')
PY
