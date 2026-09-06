# PeggleSilicon

PeggleSilicon runs the original 32-bit Intel Peggle Deluxe 1.0.5 Mac game on
Apple silicon by loading its i386 Mach-O image through Rosetta and translating
the legacy Carbon, CoreFoundation, OpenGL, audio, libc, pthread, and C++ ABI
calls to current macOS APIs.

The export step builds the native loader on the Mac where it runs. It therefore
needs Python 3 and Apple's Command Line Tools for Xcode, which provide `xcrun`,
Clang, `make`, and the macOS SDK. `codesign` and `ditto` are supplied by macOS.
The Python scripts use only the standard library. A full Xcode installation and
Homebrew are not required, and the finished `PeggleSilicon.app` does not need
these tools to run.

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
open "build/PeggleSilicon Installer.app"
```

The installer targets macOS 11 or newer.
Keep the helper app inside this repository so it can find the native runtime
and build script.

Drag the original `Peggle Deluxe.app` into the window, choose an export folder,
and click **Install PeggleSilicon**. The helper invokes `tools/build.py` and
creates `PeggleSilicon.app` in the selected folder.

If the standard Steam installation is present, the installer offers
**Replace Steam installation…** and installs directly from it. No separate
download is needed: Valve ships the Steam executable wrapped in Steam DRM, which
keeps the game code encrypted, so the build first unwraps it. Because macOS can
no longer run the 32-bit game, the unwrap is performed by the compatibility
runtime itself, which executes Valve's own decryptor to recover the original
code. That decryptor verifies ownership through a live handshake with the Steam
client, so **Steam must be running and signed in to the account that owns Peggle
Deluxe** during installation; the build starts Steam if it is not already
running. After confirmation, the original Steam app is renamed to
`Peggle Deluxe.app.bak` and the Apple silicon build takes its place with Steam's
expected `Contents/MacOS/Peggle` executable name. An older PeggleSilicon install
whose game image is still the encrypted executable is detected and can be
repaired the same way from the backup.

The same unwrap runs for a standalone export if a DRM-wrapped app is supplied,
so both the Steam copy and a DRM-free `Peggle Deluxe.app` work as the source.
The recovered game executable and resources are copied into
`Contents/SharedSupport`. BASS is bundled from `native/vendor/bass` and the
resulting app is ad hoc signed for local use.
