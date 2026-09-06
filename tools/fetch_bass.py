#!/usr/bin/env python3
"""Fetch the BASS audio library into native/vendor/bass.

BASS (un4seen.com) is not part of this repository.  The games link against
it, so the runtime needs the x86_64 slice of un4seen's own libbass.dylib next
to the loader.  This downloads the official macOS package and unpacks the
three files the build uses: libbass.dylib, bass.h and the licence text.

Run it directly to prefetch; native/Makefile runs it when the files are
missing.  Offline, place libbass.dylib, c/bass.h and bass.txt from
bass24-osx.zip in native/vendor/bass yourself.
"""
from __future__ import annotations

import hashlib
import io
import pathlib
import shutil
import struct
import subprocess
import sys
import urllib.request
import zipfile

URL = 'https://www.un4seen.com/files/bass24-osx.zip'
# The libbass.dylib this project was tested with (BASS 2.4.17).  A newer
# upstream release is accepted with a note; the API is stable within 2.4.
TESTED_SHA256 = 'e81fb7b4d0009ba6343fbfcd840620704dcb840686d405b9734cd37150d31974'
FILES = {'libbass.dylib': 'libbass.dylib', 'c/bass.h': 'bass.h', 'bass.txt': 'bass.txt'}

root = pathlib.Path(__file__).resolve().parent.parent
destination = root / 'native/vendor/bass'


def has_x86_64_slice(dylib: bytes) -> bool:
    if len(dylib) < 8:
        return False
    magic, count = struct.unpack('>II', dylib[:8])
    if magic == 0xCAFEBABE:
        for index in range(count):
            offset = 8 + index * 20
            cputype = struct.unpack('>I', dylib[offset:offset + 4])[0]
            if cputype == 0x01000007:
                return True
        return False
    return dylib[:4] == b'\xcf\xfa\xed\xfe' and dylib[4:8] == b'\x07\x00\x00\x01'


def download() -> bytes | None:
    """curl uses the system trust store; Python's urllib often lacks CA
    certificates on macOS, so it is only the fallback."""
    curl = shutil.which('curl')
    if curl:
        result = subprocess.run([curl, '-sS', '-L', '--fail', '--max-time', '180', URL],
                                capture_output=True)
        if result.returncode == 0 and result.stdout:
            return result.stdout
        print(f'fetch_bass: curl failed: {result.stderr.decode(errors="replace").strip()}',
              file=sys.stderr)
    try:
        with urllib.request.urlopen(URL, timeout=120) as response:
            return response.read()
    except OSError as error:
        print(f'fetch_bass: urllib failed: {error}', file=sys.stderr)
        return None


def main() -> int:
    force = '--force' in sys.argv[1:]
    if not force and all((destination / name).is_file() for name in FILES.values()):
        print(f'BASS is already in {destination}')
        return 0
    print(f'Downloading {URL} ...')
    package = download()
    if package is None:
        print(f'fetch_bass: could not download BASS.\n'
              f'Download {URL} yourself and put libbass.dylib, c/bass.h and '
              f'bass.txt in {destination}', file=sys.stderr)
        return 1
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        names = set(archive.namelist())
        missing = [member for member in FILES if member not in names]
        if missing:
            print(f'fetch_bass: the package does not contain {missing}', file=sys.stderr)
            return 1
        destination.mkdir(parents=True, exist_ok=True)
        for member, name in FILES.items():
            (destination / name).write_bytes(archive.read(member))
    dylib = (destination / 'libbass.dylib').read_bytes()
    if not has_x86_64_slice(dylib):
        print('fetch_bass: libbass.dylib has no x86_64 slice; the loader cannot use it', file=sys.stderr)
        return 1
    digest = hashlib.sha256(dylib).hexdigest()
    if digest == TESTED_SHA256:
        print(f'BASS unpacked into {destination} (libbass.dylib verified)')
    else:
        print(f'BASS unpacked into {destination} (libbass.dylib {digest[:12]} differs from '
              f'the tested {TESTED_SHA256[:12]}: a newer upstream release)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
