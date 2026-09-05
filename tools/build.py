#!/usr/bin/env python3
import argparse, pathlib, subprocess, plistlib, shutil

root=pathlib.Path(__file__).resolve().parent.parent

subprocess.run([str(root/'tools/install_dependencies.sh')], check=True)

parser = argparse.ArgumentParser(description='Build the PeggleSilicon compatibility app.')
parser.add_argument('source', type=pathlib.Path, help='path to the original Peggle Deluxe.app')
parser.add_argument(
    '--output',
    type=pathlib.Path,
    default=root/'build/PeggleSilicon.app',
    help='destination app bundle (default: build/PeggleSilicon.app)',
)
args = parser.parse_args()

source=args.source.expanduser()
if not source.is_dir():
    raise SystemExit(f'game bundle not found: {source}')
subprocess.run(['make','-C',str(root/'native'),'-j4'],check=True)
bundle=args.output.expanduser();c=bundle/'Contents'
bundle.parent.mkdir(parents=True,exist_ok=True)
for d in ('MacOS','Resources','SharedSupport'): (c/d).mkdir(parents=True,exist_ok=True)
if not (c/'SharedSupport/Peggle.image').exists():
 shutil.copy2(source/'Contents/MacOS/Peggle',c/'SharedSupport/Peggle.image')
 subprocess.run(['ditto','--noextattr','--noqtn',str(source/'Contents/Resources'),str(c/'Resources')],check=True)
shutil.copy2(root/'native/build/game_loader',c/'MacOS/PeggleSilicon')
shutil.copy2(root/'native/vendor/bass/libbass.dylib',c/'MacOS/libbass.dylib')
p=plistlib.loads((source/'Contents/Info.plist').read_bytes());p.update(CFBundleExecutable='PeggleSilicon',CFBundleIdentifier='local.peggle.silicon',CFBundleName='PeggleSilicon',LSMinimumSystemVersion='11.0',NSHighResolutionCapable=False)
(c/'Info.plist').write_bytes(plistlib.dumps(p))
subprocess.run(['codesign','--force','--deep','--sign','-',str(bundle)],check=True)
print(bundle)
