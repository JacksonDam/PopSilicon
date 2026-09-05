#!/bin/sh
set -u

# The installer builds the compatibility loader on the Mac where the game is
# installed. Apple ships the compiler, SDK, and make separately from macOS as
# the Command Line Tools for Xcode package.
require_swift=0
if [ "${1:-}" = "--installer" ]; then
    require_swift=1
fi

if ! /usr/bin/xcode-select -p >/dev/null 2>&1; then
    echo "PeggleSilicon needs Apple's Command Line Tools for Xcode."
    echo "Opening Apple's installer; finish it, then run the installation again."
    /usr/bin/xcode-select --install >/dev/null 2>&1 || true
    exit 74
fi

missing=""
for command_name in make clang codesign ditto python3; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        missing="$missing $command_name"
    fi
done

if [ "$require_swift" -eq 1 ] && ! /usr/bin/xcrun --find swiftc >/dev/null 2>&1; then
    missing="$missing swiftc"
fi

if [ -n "$missing" ]; then
    echo "PeggleSilicon is missing these commands:$missing"
    echo "Install Apple's Command Line Tools with: xcode-select --install"
    echo "If Python 3 is still missing afterward, install Python 3 from python.org or Homebrew."
    exit 1
fi

exit 0
