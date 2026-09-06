#!/usr/bin/env python3
import argparse, filecmp, os, pathlib, subprocess, plistlib, shutil, tempfile, time

root=pathlib.Path(__file__).resolve().parent.parent

# Valve's Mach-O DRM wrapper leaves its own source paths in the appended stub;
# the loader checks the same marker.  A wrapped executable has its code
# encrypted, so it must be unwrapped before it can be used as the game image.
STEAM_DRM_MARKER = b'/src/drm/mach-o/'


def is_steam_drm(executable: pathlib.Path) -> bool:
    return STEAM_DRM_MARKER in executable.read_bytes()


def steam_is_running() -> bool:
    return subprocess.run(['pgrep', '-x', 'steam_osx'],
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def ensure_steam_running() -> None:
    """The DRM verifies ownership by a live handshake with the Steam client, so
    Steam must be running (and signed in, owning the game) to unwrap the code."""
    if steam_is_running():
        return
    # Launch Steam in the background (no focus steal) and give it time to come up.
    subprocess.run(['open', '-g', '-a', 'Steam'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        if steam_is_running():
            time.sleep(3)  # let the client finish signing in
            return
        time.sleep(1)
    raise SystemExit(
        'Steam must be running to unwrap the Steam copy of Peggle Deluxe.\n'
        'Valve wraps the executable in Steam DRM that only decrypts after a live\n'
        'ownership check with the Steam client. Start Steam, sign in to the\n'
        'account that owns Peggle Deluxe, then run the installation again.')


def unwrap_steam_drm(loader: pathlib.Path, drm_executable: pathlib.Path,
                     out_image: pathlib.Path) -> None:
    """Recover a clean game image from a Steam-DRM-wrapped executable.

    The DRM decryptor is 32-bit Intel code that only runs under the compat
    loader, so the loader itself performs the unwrap (LP32_UNWRAP_STEAM) and
    writes a retail-equivalent image the normal load path accepts.
    """
    ensure_steam_running()
    with tempfile.TemporaryDirectory(prefix='pegglesilicon-unwrap-') as tmp:
        staged = pathlib.Path(tmp)/'Peggle.image'
        # The loader finds libbass next to itself (make copies the dylib into
        # native/build), so no DYLD_* variables are needed; arch(1) would strip
        # them anyway.
        env = dict(os.environ, LP32_UNWRAP_STEAM=str(staged))
        result = subprocess.run(
            ['arch', '-x86_64', str(loader), str(drm_executable)],
            env=env, capture_output=True, text=True)
        if result.returncode != 0 or not staged.is_file():
            raise SystemExit(
                'could not unwrap the Steam DRM copy of Peggle Deluxe.\n'
                f'{result.stdout}{result.stderr}'.strip())
        shutil.move(str(staged), str(out_image))


def copy_clean(source: pathlib.Path, destination: pathlib.Path) -> None:
    """Copy one file without its extended attributes.  A vendor dylib or game
    copy that came through a browser carries com.apple.quarantine, which
    shutil.copy2 would preserve and which makes Gatekeeper prompt before the
    library may load; drop it the way the Resources copy below does."""
    subprocess.run(['ditto', '--noextattr', '--noqtn', str(source), str(destination)],
                   check=True)


parser = argparse.ArgumentParser(description='Build the PeggleSilicon compatibility app.')
parser.add_argument('source', type=pathlib.Path,
                    help='path to a Peggle Deluxe.app (retail or the Steam copy)')
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
executable=source/'Contents/MacOS/Peggle'
if not executable.is_file():
    raise SystemExit(f'game executable not found: {executable}')

subprocess.run(['make','-C',str(root/'native'),'-j4'],check=True)
loader=root/'native/build/game_loader'

bundle=args.output.expanduser();c=bundle/'Contents'
bundle.parent.mkdir(parents=True,exist_ok=True)
for d in ('MacOS','Resources','SharedSupport'): (c/d).mkdir(parents=True,exist_ok=True)

image=c/'SharedSupport/Peggle.image'
drm=is_steam_drm(executable)
# Regenerate the image when it is missing, or (for a plain retail source) when
# the source executable changed.  A DRM source needs an unwrap step, so only
# regenerate it when the image is absent.
if not image.exists() or (not drm and not filecmp.cmp(executable,image,shallow=False)):
 if drm:
  print(f'{executable.name} is a Steam DRM copy; unwrapping its game code…')
  unwrap_steam_drm(loader,executable,image)
 else:
  copy_clean(executable,image)
 subprocess.run(['ditto','--noextattr','--noqtn',str(source/'Contents/Resources'),str(c/'Resources')],check=True)

copy_clean(loader,c/'MacOS/PeggleSilicon')
copy_clean(root/'native/vendor/bass/libbass.dylib',c/'MacOS/libbass.dylib')
p=plistlib.loads((source/'Contents/Info.plist').read_bytes());p.update(CFBundleExecutable='PeggleSilicon',CFBundleIdentifier='local.peggle.silicon',CFBundleName='PeggleSilicon',LSMinimumSystemVersion='11.0',NSHighResolutionCapable=False)
(c/'Info.plist').write_bytes(plistlib.dumps(p))
subprocess.run(['codesign','--force','--deep','--sign','-',str(bundle)],check=True)
print(bundle)
