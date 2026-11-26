"""Fetch source headers for the Android 8.1 board's GNSS HAL build.

GNSS 1.0 generated declarations come from the VNDK snapshot; their stable ABI
is linked against this board's actual 8.1 libraries, not VNDK runtime binaries.
"""
import argparse
import io
import hashlib
import json
from pathlib import Path
import tarfile
import urllib.request

ARCHIVES = [
    ("hidl", "platform/system/libhidl", "android-8.1.0_r1", "base/include"),
    ("utils", "platform/system/core", "android-8.1.0_r1", "libutils/include"),
    ("cutils", "platform/system/core", "android-8.1.0_r1", "libcutils/include"),
    ("libcxx", "platform/external/libcxx", "android-8.1.0_r1", "include"),
    ("gnss", "platform/prebuilts/vndk/v30", "5f9884aa352825291757dfd6694b874ad8c1805e",
     "arm64/include/generated-headers/hardware/interfaces/gnss/1.0"),
    ("base", "platform/prebuilts/vndk/v30", "5f9884aa352825291757dfd6694b874ad8c1805e",
     "arm64/include/generated-headers/system/libhidl/transport/base/1.0"),
    ("manager", "platform/prebuilts/vndk/v30", "5f9884aa352825291757dfd6694b874ad8c1805e",
     "arm64/include/generated-headers/system/libhidl/transport/manager/1.0"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parents[1] / 'build/deps/gnss/include')
    parser.add_argument("--proxy", help="Optional HTTP proxy URL")
    args = parser.parse_args()
    fetch(args.output, args.proxy)


def fetch(output_dir, proxy=None):
    expected = json.loads((Path(__file__).with_name('dependencies.lock.json')).read_text())['headers']['gnss']
    opener = urllib.request.build_opener(urllib.request.ProxyHandler(
        {"http": proxy, "https": proxy})) if proxy else urllib.request.build_opener()
    fetched = {}
    for name, repo, ref, source in ARCHIVES:
        url = f"https://android.googlesource.com/{repo}/+archive/{ref}/{source}.tar.gz"
        for attempt in range(3):
            try:
                data = opener.open(url, timeout=45).read()
                with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
                    for member in archive.getmembers():
                        relative = Path(member.name.split("/gen/", 1)[-1])
                        if relative.is_absolute() or ".." in relative.parts:
                            raise ValueError("Unsafe source archive path")
                        if not member.isfile():
                            continue
                        target = (Path('libcxx') / relative if name == 'libcxx' else relative).as_posix()
                        fetched[target] = archive.extractfile(member).read()
                print(f"Fetched {name}: {len(data)} bytes", flush=True)
                break
            except Exception:
                if attempt == 2:
                    raise
    for name, digest in expected.items():
        if name not in fetched or hashlib.sha256(fetched[name]).hexdigest() != digest:
            raise ValueError(f'Pinned GNSS header mismatch: {name}; no headers were installed')
    for name in expected:
        output = output_dir / name
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(fetched[name])


if __name__ == "__main__":
    main()
