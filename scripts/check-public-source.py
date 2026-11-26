"""Check Git-visible source and the index without printing any secret content.

This is a targeted release gate, not a substitute for reviewing a staged diff.
"""
import argparse
from pathlib import Path, PurePosixPath
import re
import subprocess


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def validate(name, data):
    path = PurePosixPath(name)
    if path.suffix.lower() in {'.pem', '.pk8', '.key', '.keystore', '.password', '.p12', '.pfx', '.jks',
                              '.apk', '.so', '.dll', '.exe'}:
        return 'private material or locally supplied binary'
    if name.startswith(('deploy/opi4/bin/', 'tools/scrcpy/', 'build/')):
        return 'local-only directory'
    if name in {'src/lockdown_identity.h', 'src/lockdown_tls_identity.h'}:
        return 'generated local identity'
    if name.startswith('config/') and path.name != 'README.md' and path.suffix != '.example':
        return 'non-template configuration'
    if re.search(rb'-----BEGIN (?:RSA |EC |ENCRYPTED )?PRIVATE KEY-----\s*[\r\n]+[A-Za-z0-9+/=]{24}', data):
        return 'embedded PEM private key'
    if data[:4] in (b'\x7fELF', b'PK\x03\x04') or data[:2] == b'MZ':
        return 'binary package (publish separately after review)'
    if re.search(rb'(?i)(?:gh[pousr]_[a-z0-9]{30,}|github_pat_[a-z0-9_]{40,})', data):
        return 'access-token pattern'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    indexed = {n.decode('utf-8') for n in git(root, 'ls-files', '-z').split(b'\0') if n}
    visible = indexed | {n.decode('utf-8') for n in git(root, 'ls-files', '--others', '--exclude-standard', '-z').split(b'\0') if n}
    failures = []
    for name in sorted(visible):
        sources = []
        if name in indexed:
            sources.append(('index', git(root, 'show', ':' + name)))
        path = root / name
        if path.is_symlink():
            failures.append((name, 'working tree', 'symlink requires explicit release review'))
        elif path.is_file():
            sources.append(('working tree', path.read_bytes()))
        for location, data in sources:
            reason = validate(name, data)
            if reason:
                failures.append((name, location, reason))
    for name, location, reason in failures:
        print(f'FAIL {name} ({location}): {reason}')
    print(f'Checked {len(visible)} Git-visible files; {len(failures)} violation(s).')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
