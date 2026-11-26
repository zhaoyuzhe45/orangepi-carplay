"""Prepare checked local build dependencies; firmware is supplied by the builder.

--firmware-root accepts an extracted tree containing system/lib64 and
system/lib64/hw. It never connects to, reads from, or modifies a live device.
"""
import argparse
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'scripts/dependencies.lock.json').read_text())


def checked(data, digest, label):
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f'SHA256 mismatch: {label}; obtain the supported original firmware input.')
    return data


def install_headers(proxy):
    spec = importlib.util.spec_from_file_location('gnss_headers', ROOT / 'scripts/fetch-gnss-build-headers.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.fetch(ROOT / 'build/deps/gnss/include', proxy)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({'http': proxy, 'https': proxy})) if proxy else urllib.request.build_opener()
    fetched = {}
    for name, remote in LOCK['microphone_sources'].items():
        url = 'https://android.googlesource.com/platform/' + remote + '?format=TEXT'
        with opener.open(url, timeout=30) as response:
            data = base64.b64decode(response.read())
        fetched[name] = checked(data, LOCK['headers']['shared-microphone'][name], name)
    for name, data in fetched.items():
        path = ROOT / 'build/shared-microphone/include' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def import_firmware(source):
    pending = []
    for entry in LOCK['firmware']:
        origin = source / entry['source']
        pending.append((ROOT / entry['destination'], checked(origin.read_bytes(), entry['sha256'], entry['source'])))
    # Validate the complete input set before changing any dependency.
    for target, data in pending:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    print(f'Imported {len(pending)} matching firmware libraries; no device was modified.')


def verify():
    for group, files in LOCK['headers'].items():
        base = ROOT / ('build/deps/gnss/include' if group == 'gnss' else 'build/shared-microphone/include')
        for name, digest in files.items():
            checked((base / name).read_bytes(), digest, f'{group}/{name}')
    for entry in LOCK['firmware']:
        checked((ROOT / entry['destination']).read_bytes(), entry['sha256'], entry['destination'])
    print('All header and original firmware dependencies match dependencies.lock.json.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--headers', action='store_true', help='download pinned AOSP headers')
    parser.add_argument('--firmware-root', type=Path)
    parser.add_argument('--check', action='store_true', help='verify the full local dependency set')
    parser.add_argument('--proxy', help='optional HTTP proxy, otherwise use system proxy settings')
    args = parser.parse_args()
    if not (args.headers or args.firmware_root or args.check):
        parser.error('choose --headers, --firmware-root, and/or --check')
    try:
        if args.headers:
            install_headers(args.proxy)
        if args.firmware_root:
            import_firmware(args.firmware_root.resolve())
        if args.check:
            verify()
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
