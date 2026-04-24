#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# --- Locate NDK ---
if [ -n "$ANDROID_NDK_HOME" ]; then
    NDK="$ANDROID_NDK_HOME"
elif [ -n "$ANDROID_HOME" ] && [ -d "$ANDROID_HOME/ndk" ]; then
    NDK=$(ls -d "$ANDROID_HOME/ndk/"* 2>/dev/null | sort -V | tail -1)
elif [ -d "$HOME/Library/Android/sdk/ndk" ]; then
    NDK=$(ls -d "$HOME/Library/Android/sdk/ndk/"* 2>/dev/null | sort -V | tail -1)
else
    echo "ERROR: Cannot find Android NDK. Set ANDROID_NDK_HOME." >&2
    exit 1
fi
echo "NDK: $NDK"

case "$(uname)" in
    Darwin) HOST_TAG=darwin-x86_64 ;;
    Linux)  HOST_TAG=linux-x86_64 ;;
    *)      echo "Unsupported OS" >&2; exit 1 ;;
esac

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
CC="$TOOLCHAIN/bin/aarch64-linux-android34-clang"
STRIP="$TOOLCHAIN/bin/llvm-strip"

if [ ! -f "$CC" ]; then
    echo "ERROR: Compiler not found: $CC" >&2
    exit 1
fi

# --- Compile daemon ---
echo "Compiling power_daemon..."
mkdir -p module/bin
$CC -O2 -Wall -Wextra -o module/bin/power_daemon native/power_daemon.c
$STRIP module/bin/power_daemon
echo "Binary size: $(wc -c < module/bin/power_daemon | tr -d ' ') bytes"

# --- Package module ZIP ---
echo "Packaging module..."
rm -f server_phone_module.zip
cd module
zip -r ../server_phone_module.zip . -x '*.DS_Store'
cd ..

echo ""
echo "=== Build complete ==="
echo "Output: server_phone_module.zip"
ls -lh server_phone_module.zip
