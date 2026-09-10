# PopSilicon

PopSilicon runs the original 32-bit Intel PopCap Mac games on Apple silicon by
loading their i386 Mach-O image through Rosetta and translating the legacy
Carbon, CoreFoundation, OpenGL, audio, libc, pthread, and C++ ABI calls to
current macOS APIs. It ships as five products built from the same runtime:

- **PeggleSilicon** for Peggle Deluxe 1.0.5 and Peggle Nights 1.0.4. Peggle Deluxe has been playtested through all of Adventure Mode stages and works well. Peggle Nights has been tested up to Stage 3-5 and seems to work well.
- **BejeweledSilicon** for Bejeweled 3 1.1.12 and Bejeweled 2 Deluxe 1.0.0.
Bejeweled 3’s Classic, Zen, Lightning, Quest, Ice Storm, Poker, Butterflies, and Diamond Mine modes work well. Bejeweled 2’s Classic, Action, Puzzle, and Endless modes have been tested and work well.
- **ChuzzleSilicon** for Chuzzle Deluxe 1.0.0. Tested with basic gameplay and 
seems to work well.
- **PvZSilicon** for Plants vs. Zombies 1.0.41. Tested with basic gameplay in 
start of Adventure mode and seems to work well.
- **ZumaSilicon** for Zuma Deluxe 1.0.0. Passed basic testing and seems to 
work well.

A single loader serves all seven titles; it detects which game an image is and
applies the right profile.

The export step builds the native loader on the Mac where it runs. It therefore
needs Python 3 and Apple's Command Line Tools for Xcode, which provide `xcrun`,
Clang, `make`, and the macOS SDK. `codesign` and `ditto` are supplied by macOS.
The Python scripts use only the standard library. A full Xcode installation and
Homebrew are not required, and the finished game app does not need these tools
to run.

Check the dependencies with:

```sh
./tools/install_dependencies.sh
```

If the Command Line Tools are missing, the script opens Apple's installer and
prints the next step. Python 3 is included by some Command Line Tools releases;
if it is still unavailable afterward, install Python 3 from python.org or
Homebrew. Building the graphical installer itself additionally needs `swiftc`,
which the same Command Line Tools package provides.

Build the game from the supplied game copy:

```sh
python3 tools/build.py "/path/to/Peggle Deluxe.app"
open build/PeggleSilicon.app
```

For a graphical installer, build and open the included helper app:

```sh
python3 tools/build_installer.py
open "build/PopSilicon Installer.app"
```

The installer targets macOS 11 or newer.
Keep the helper app inside this repository so it can find the native runtime
and build script.

The installer first asks which product to install: **PeggleSilicon**,
**BejeweledSilicon**, **ChuzzleSilicon**, **PvZSilicon** or **ZumaSilicon**. Then choose the game
from the **Game** menu at the top (only that product's games are listed), drag
that game's original `.app` into the window, choose an export folder, and click
**Install** for that product. The helper invokes `tools/build.py` and creates
the app in the selected folder (`PeggleSilicon.app` for Deluxe,
`PeggleNights.app` for Nights, `Bejeweled3.app` for Bejeweled 3, `Bejeweled2.app`
for Bejeweled 2, `Chuzzle.app` for Chuzzle, `PlantsVsZombies.app` for Plants vs.
Zombies, `Zuma.app` for Zuma). Dropping a recognised game app selects it, and
its product, automatically, so the whole window follows whichever title you
drop.

If the selected game's Steam installation is present, the installer offers
**Replace Steam installation…** and installs directly from it. No separate
download is needed. Valve ships the two Peggle Steam executables and the
Plants vs. Zombies one wrapped in Steam DRM, which keeps the game code
encrypted, so the build first unwraps them. Because macOS can no longer run the 32-bit game, the unwrap is performed
by the compatibility runtime itself, which executes Valve's own decryptor to
recover the original code. That decryptor verifies ownership through a live
handshake with the Steam client, so for those games **Steam must be running
and signed in to the account that owns the game** during installation;
the build starts Steam if it is not already running. The Bejeweled, Chuzzle and
Zuma Steam copies are not DRM-wrapped, so they install without Steam running.
(Bejeweled 2's, Chuzzle's and Zuma's carry an extra `__STEAM` segment holding
Valve's
ownership stub, but the game code beside it is in the clear. That stub reads a
Mac OS X 10.4 dyld's private globals and cannot run here, so the loader skips
it — and, because the stub took over the executable's first module initializer,
recovers the one it displaced from the tail jump every copy of the stub ends
with, so the game's own initializer still runs.) After confirmation, the
original Steam app is renamed to `<name>.app.bak` and the Apple silicon build
takes its place under the executable name Steam launches (`Peggle` for Deluxe,
`Peggle Nights` for Nights, `Bejeweled3` for Bejeweled 3, `Bejeweled 2` for
Bejeweled 2, `Chuzzle` for Chuzzle, `PlantsvsZombies` for Plants vs. Zombies,
`Zuma` for Zuma). An older install whose game image is still the encrypted
executable is detected and can be repaired the same way from the backup.

The same unwrap runs for a standalone export if a DRM-wrapped app is supplied,
so both the Steam copy and a DRM-free copy of the game work as the source. The
recovered game executable and resources are copied into `Contents/SharedSupport`.
The games' audio library, BASS from un4seen.com, is not part of this
repository. (Bejeweled 2 and Zuma shipped with BASSMOD, an i386-only cut-down
relative of BASS, and Chuzzle links the 2.3-era BASS API; the runtime serves
them all from the same BASS build.) The first build runs `tools/fetch_bass.py`, which downloads the
official `bass24-osx.zip` into `native/vendor/bass` (ignored by git) and copies
`libbass.dylib` into the app next to the loader; run the script yourself to
prefetch, or unpack the package there by hand when offline. The resulting app
is ad hoc signed for local use.
