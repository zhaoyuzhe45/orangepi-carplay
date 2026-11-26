"""Execute every shell regression and preserve each exit code."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
failed = []
for test in sorted((root / 'tests').glob('*_test.sh')):
    result = subprocess.run(['sh', str(test)], cwd=root, capture_output=True, text=True)
    print('TEST %s exit=%d' % (test.name, result.returncode), flush=True)
    print(result.stdout, end='')
    print(result.stderr, end='')
    if result.returncode:
        failed.append(test.name)
print('FAILED:', ', '.join(failed) or 'none')
sys.exit(bool(failed))
