#!/usr/bin/env python3
import argparse
import pathlib
import shutil
import subprocess
import tempfile


root = pathlib.Path(__file__).resolve().parent.parent
source_root = root / 'PopSiliconInstaller'

subprocess.run([str(root / 'tools/install_dependencies.sh'), '--installer'], check=True)

parser = argparse.ArgumentParser(description='Build the PopSilicon installer app.')
parser.add_argument(
    '--output',
    type=pathlib.Path,
    default=root / 'build/PopSilicon Installer.app',
    help='destination app bundle (default: build/PopSilicon Installer.app)',
)
args = parser.parse_args()

swift_sources = sorted((source_root / 'Sources').glob('*.swift'))
if not swift_sources:
    raise SystemExit(f'no Swift sources found in {source_root / "Sources"}')

bundle = args.output.expanduser()
bundle.parent.mkdir(parents=True, exist_ok=True)

targets = {
    'arm64': 'arm64-apple-macosx11.0',
    'x86_64': 'x86_64-apple-macosx10.15',
}

swiftc = pathlib.Path(subprocess.run(
    ['xcrun', '--find', 'swiftc'], check=True, capture_output=True, text=True
).stdout.strip())
concurrency_runtime = swiftc.parent.parent / 'lib/swift-5.5/macosx/libswift_Concurrency.dylib'
if not concurrency_runtime.is_file():
    raise SystemExit(f'missing the Swift concurrency back-deployment runtime: {concurrency_runtime}')

with tempfile.TemporaryDirectory(prefix='popsilicon-installer-') as temporary:
    slices = []
    for arch, target in targets.items():
        executable = pathlib.Path(temporary) / f'PopSiliconInstaller-{arch}'
        subprocess.run(
            [
                'xcrun', 'swiftc',
                '-swift-version', '6',
                '-target', target,
                '-O',
                '-whole-module-optimization',
                '-framework', 'AppKit',
                '-framework', 'SwiftUI',
                '-Xlinker', '-weak_framework', '-Xlinker', 'UniformTypeIdentifiers',
                '-Xlinker', '-rpath', '-Xlinker', '@executable_path/../Frameworks',
                *map(str, swift_sources),
                '-o', str(executable),
            ],
            check=True,
            cwd=root,
        )
        slices.append(executable)
    executable = pathlib.Path(temporary) / 'PopSiliconInstaller'
    subprocess.run(['lipo', '-create', *map(str, slices), '-output', str(executable)], check=True)

    if bundle.exists():
        shutil.rmtree(bundle)
    contents = bundle / 'Contents'
    (contents / 'MacOS').mkdir(parents=True)
    (contents / 'Frameworks').mkdir()
    shutil.copy2(executable, contents / 'MacOS/PopSiliconInstaller')
    shutil.copy2(concurrency_runtime, contents / 'Frameworks/libswift_Concurrency.dylib')
    shutil.copy2(source_root / 'Info.plist', contents / 'Info.plist')

subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(bundle)], check=True)
print(bundle)
