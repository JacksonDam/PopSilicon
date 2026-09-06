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

Steam support is currently broken.

~~If the standard Steam installation is present and still unmodified, the installer
can use that `.app` directly and offers **Replace Steam installation…**. It checks
for the original 32-bit Intel executable before proceeding. After confirmation,
the original Steam app is renamed to `Peggle Deluxe.app.bak` and the Apple silicon
build takes its place with Steam's expected `Contents/MacOS/Peggle` executable name.~~

The original game executable and resources are copied unchanged into
`Contents/SharedSupport`. BASS is bundled from `native/vendor/bass` and the
resulting app is ad hoc signed for local use.
