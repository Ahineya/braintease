"""Record runtime coverage only for the build fingerprints actually tested."""
import json
from pathlib import Path
import subprocess


def check_root(path):
    """Check the raw native root, without the Ripple FAT disk wrapper."""
    checker = Path(__file__).resolve().parents[1] / 'ports/fuzix/tools/fsck.mjs'
    subprocess.run(['node', '--input-type=module', '-e',
                    "import fs from 'node:fs'; const {checkRoot}=await import(process.argv[1]);"
                    "const result=checkRoot(fs.readFileSync(process.argv[2]));"
                    "if(result.issues.length) throw Error(result.issues.join('\\n'));",
                    checker.as_uri(), str(path.resolve())], check=True)


def record_runtime(build, names, status):
    path = build / 'application-report.json'
    if not path.exists():
        return
    report = json.loads(path.read_text())
    for item in report:
        if item['name'] not in names:
            continue
        item['runtime'] = status
        stamp = build / 'application-logs' / (item['name'] + '.json')
        if stamp.exists():
            cached = json.loads(stamp.read_text())
            if cached.get('fingerprint') == item.get('fingerprint'):
                cached['runtime'] = status
                stamp.write_text(json.dumps(cached, indent=2) + '\n')
    path.write_text(json.dumps(report, indent=2) + '\n')
