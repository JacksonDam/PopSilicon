#!/usr/bin/env python3
import argparse
import pathlib
import shutil
import subprocess
import tempfile


root = pathlib.Path(__file__).resolve().parent.parent
source_root = root / 'PeggleSiliconInstaller'

subprocess.run([str(root / 'tools/install_dependencies.sh'), '--installer'], check=True)

parser = argparse.ArgumentParser(description='Build the PeggleSilicon export helper app.')
parser.add_argument(
    '--output',
    type=pathlib.Path,
    default=root / 'build/PeggleSilicon Installer.app',
    help='destination app bundle (default: build/PeggleSilicon Installer.app)',
)
args = parser.parse_args()

swift_sources = sorted((source_root / 'Sources').glob('*.swift'))
if not swift_sources:
    raise SystemExit(f'no Swift sources found in {source_root / "Sources"}')

bundle = args.output.expanduser()
bundle.parent.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix='pegglesilicon-installer-') as temporary:
    executable = pathlib.Path(temporary) / 'PeggleSiliconInstaller'
    subprocess.run(
        [
            'xcrun', 'swiftc',
            '-swift-version', '6',
            '-target', 'arm64-apple-macosx11.0',
            '-O',
            '-whole-module-optimization',
            '-framework', 'AppKit',
            '-framework', 'SwiftUI',
            '-framework', 'UniformTypeIdentifiers',
            *map(str, swift_sources),
            '-o', str(executable),
        ],
        check=True,
        cwd=root,
    )

    if bundle.exists():
        shutil.rmtree(bundle)
    contents = bundle / 'Contents'
    (contents / 'MacOS').mkdir(parents=True)
    shutil.copy2(executable, contents / 'MacOS/PeggleSiliconInstaller')
    shutil.copy2(source_root / 'Info.plist', contents / 'Info.plist')

subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(bundle)], check=True)
print(bundle)
