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
        'Steam must be running to unwrap the Steam copy of the game.\n'
        'Valve wraps the executable in Steam DRM that only decrypts after a live\n'
        'ownership check with the Steam client. Start Steam, sign in to the\n'
        'account that owns the game, then run the installation again.')


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
                'could not unwrap the Steam DRM copy of the game.\n'
                f'{result.stdout}{result.stderr}'.strip())
        shutil.move(str(staged), str(out_image))


def copy_clean(source: pathlib.Path, destination: pathlib.Path) -> None:
    """Copy one file without its extended attributes.  A vendor dylib or game
    copy that came through a browser carries com.apple.quarantine, which
    shutil.copy2 would preserve and which makes Gatekeeper prompt before the
    library may load; drop it the way the Resources copy below does."""
    subprocess.run(['ditto', '--noextattr', '--noqtn', str(source), str(destination)],
                   check=True)


def thin_i386(source: pathlib.Path, destination: pathlib.Path) -> None:
    """Write the i386 slice of the source; a thin i386 file copies through.

    The loader maps one thin i386 executable, so the PowerPC-era titles, which
    ship ppc/i386 universal binaries, have to be thinned first."""
    if source.read_bytes()[:4] in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
        subprocess.run(['lipo', '-thin', 'i386', str(source), '-output', str(destination)],
                       check=True)
    else:
        copy_clean(source, destination)


def unswizzle_macprotect(payload: pathlib.Path, destination: pathlib.Path) -> None:
    data = payload.read_bytes()
    whole = len(data) - len(data) % 16
    recovered = b''.join(data[i:i + 16][::-1] for i in range(0, whole, 16))
    with tempfile.TemporaryDirectory(prefix='popsilicon-unswizzle-') as tmp:
        universal = pathlib.Path(tmp)/'payload.fat'
        universal.write_bytes(recovered + data[whole:])
        thin_i386(universal, destination)


# Per-title build settings, keyed by the source bundle identifier.  The image
# file name must match the corresponding profile's image_file in
# native/src/game_profile.c so the loader finds and auto-detects it.
GAMES = {
    'com.popcap.peggle': {
        'display_name': 'Peggle Deluxe',
        'image_file': 'Peggle.image',
        'output_name': 'PeggleSilicon.app',
        'bundle_identifier': 'local.peggle.silicon',
        'bundle_name': 'PeggleSilicon',
    },
    'com.popcap.pegglenights': {
        'display_name': 'Peggle Nights',
        'image_file': 'PeggleNights.image',
        'output_name': 'PeggleNights.app',
        'bundle_identifier': 'local.peggle.nights',
        'bundle_name': 'PeggleNights',
    },
    'com.popcap.Bejeweled3': {
        'display_name': 'Bejeweled 3',
        'image_file': 'Bejeweled3.image',
        'output_name': 'Bejeweled3.app',
        'bundle_identifier': 'local.bejeweled3.silicon',
        'bundle_name': 'Bejeweled3',
    },
    'com.popcap.bejeweled2.app': {
        'display_name': 'Bejeweled 2 Deluxe',
        'image_file': 'Bejeweled2.image',
        'output_name': 'Bejeweled2.app',
        'bundle_identifier': 'local.bejeweled2.silicon',
        'bundle_name': 'Bejeweled2',
    },
    'com.popcap.zuma.app': {
        'display_name': 'Zuma Deluxe',
        'image_file': 'Zuma.image',
        'output_name': 'Zuma.app',
        'bundle_identifier': 'local.zuma.silicon',
        'bundle_name': 'Zuma',
    },
    'com.popcap.plantsvszombies': {
        'display_name': 'Plants vs. Zombies',
        'image_file': 'PlantsVsZombies.image',
        'output_name': 'PlantsVsZombies.app',
        'bundle_identifier': 'local.plantsvszombies.silicon',
        'bundle_name': 'PlantsVsZombies',
    },
    'com.raptisoft.Chuzzle': {
        'display_name': 'Chuzzle Deluxe',
        'image_file': 'Chuzzle.image',
        'output_name': 'Chuzzle.app',
        'bundle_identifier': 'local.chuzzle.silicon',
        'bundle_name': 'Chuzzle',
    },
    'com.sproutgames.feedingfrenzy': {
        'display_name': 'Feeding Frenzy Deluxe',
        'image_file': 'FeedingFrenzy.image',
        'output_name': 'FeedingFrenzy.app',
        'bundle_identifier': 'local.feedingfrenzy.silicon',
        'bundle_name': 'FeedingFrenzy',
        'extras': ('FFArchive.saf', 'resources'),
    },
    "com.PopCap.Zuma's Revenge!": {
        'display_name': "Zuma's Revenge!",
        'image_file': 'ZumaRevenge.image',
        'output_name': 'ZumaRevenge.app',
        'bundle_identifier': 'local.zumarevenge.silicon',
        'bundle_name': 'ZumaRevenge',
        'companion': 'Contents/Frameworks/SmartDX.framework/Versions/A/SmartDX',
    },
    'com.popcap.bookworm': {
        'display_name': 'Bookworm Deluxe',
        'image_file': 'Bookworm.image',
        'output_name': 'Bookworm.app',
        'bundle_identifier': 'local.bookworm.silicon',
        'bundle_name': 'Bookworm',
        'payload': 'Contents/Resources/Bookworm.payload',
        'resources': 'Contents/Resources/Bookworm.app/Contents/Resources',
    },
}


def game_for_source(info: dict) -> dict:
    identifier = info.get('CFBundleIdentifier', '')
    game = GAMES.get(identifier)
    if not game:
        raise SystemExit(
            f'unsupported game bundle: {identifier or "(no identifier)"}.\n'
            'Supported: Peggle Deluxe, Peggle Nights, Bejeweled 3, '
            'Bejeweled 2 Deluxe, Chuzzle Deluxe, Plants vs. Zombies and '
            'Zuma Deluxe.')
    return game


parser = argparse.ArgumentParser(description='Build a PopSilicon compatibility app (PeggleSilicon or BejeweledSilicon) from the original game.')
parser.add_argument('source', type=pathlib.Path,
                    help='path to a Peggle Deluxe.app or Peggle Nights.app (retail or the Steam copy)')
parser.add_argument(
    '--output',
    type=pathlib.Path,
    default=None,
    help='destination app bundle (default: build/<game>.app)',
)
args = parser.parse_args()

source=args.source.expanduser()
if not source.is_dir():
    raise SystemExit(f'game bundle not found: {source}')
source_info=plistlib.loads((source/'Contents/Info.plist').read_bytes())
game=game_for_source(source_info)
payload=source/game['payload'] if 'payload' in game else None
executable=payload or source/'Contents/MacOS'/source_info.get('CFBundleExecutable','')
if not executable.is_file():
    raise SystemExit(f'game executable not found: {executable}')

subprocess.run(['make','-C',str(root/'native'),'-j4'],check=True)
loader=root/'native/build/game_loader'

bundle=(args.output.expanduser() if args.output
        else root/'build'/game['output_name'])
c=bundle/'Contents'
bundle.parent.mkdir(parents=True,exist_ok=True)
for d in ('MacOS','Resources','SharedSupport'): (c/d).mkdir(parents=True,exist_ok=True)

image=c/'SharedSupport'/game['image_file']
drm=is_steam_drm(executable)
# Regenerate the image when it is missing, or (for a plain retail source) when
# the source executable changed.  A DRM source needs an unwrap step, so only
# regenerate it when the image is absent.
if not image.exists() or (not drm and not payload and not filecmp.cmp(executable,image,shallow=False)):
 if drm:
  print(f"{executable.name} is a Steam DRM copy of {game['display_name']}; unwrapping its game code…")
  unwrap_steam_drm(loader,executable,image)
 elif payload:
  print(f"{executable.name} is a protected copy of {game['display_name']}; recovering its game code…")
  unswizzle_macprotect(payload,image)
 else:
  thin_i386(executable,image)
 subprocess.run(['ditto','--noextattr','--noqtn',str(source/game.get('resources','Contents/Resources')),str(c/'Resources')],check=True)

# A game that reaches part of itself through a second i386 library (Zuma's
# Revenge and SmartDX) gets that binary beside its image, where the loader
# maps it as guest code.  This sits outside the block above so that a cached
# image never leaves a stale or missing copy behind.
if 'companion' in game:
    origin=source/game['companion']
    if not origin.is_file():
        raise SystemExit(f'companion library not found: {origin}')
    thin_i386(origin,c/'SharedSupport'/origin.name)

copy_clean(loader,c/'MacOS/PeggleSilicon')
copy_clean(root/'native/vendor/bass/libbass.dylib',c/'MacOS/libbass.dylib')
p=dict(source_info);p.update(CFBundleExecutable='PeggleSilicon',CFBundleIdentifier=game['bundle_identifier'],CFBundleName=game['bundle_name'],LSMinimumSystemVersion='11.0',NSHighResolutionCapable=False)
(c/'Info.plist').write_bytes(plistlib.dumps(p))
subprocess.run(['xattr','-cr',str(bundle)],check=True)
subprocess.run(['codesign','--force','--deep','--sign','-',str(bundle)],check=True)

for extra in game.get('extras',()):
    origin=source/extra
    if origin.exists():
        subprocess.run(['ditto','--noextattr','--noqtn',str(origin),str(bundle/extra)],check=True)
print(bundle)
