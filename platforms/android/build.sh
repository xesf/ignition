#!/bin/sh
# Ignition - Android APK, built with the SDK's command-line tools only (no
# Gradle, no Java): the NDK compiles libignition.so per ABI, aapt2 links
# the manifest and the icon, apksigner signs.
#
#   platforms/android/build.sh [version]     -> build-android/ignition.apk
#
# version is X.Y.Z or vX.Y.Z (CI passes the tag; the default is the
# VERSION file); it sets versionName and versionCode (0.1.0 -> 100).
# Needs ANDROID_HOME (or ANDROID_SDK_ROOT) with an NDK, build-tools and
# an android-34 or later platform installed, a JDK (for apksigner and
# keytool), CMake, Ninja and Python 3.
#
# Signing: ANDROID_KEYSTORE, ANDROID_KEYSTORE_PASS and ANDROID_KEY_ALIAS
# name a release key. Without them a debug key is made in
# build-android/, which installs fine but cannot update an APK signed
# with another key.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$ROOT/build-android
VERSION=${1:-$(cat "$ROOT/VERSION")}
ABIS=${ABIS:-"armeabi-v7a arm64-v8a x86_64"}
MIN_SDK=26
TARGET_SDK=34

SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}
[ -n "$SDK" ] && [ -d "$SDK" ] || { echo "Set ANDROID_HOME to the Android SDK." >&2; exit 1; }
latest() { ls -1 "$1" 2>/dev/null | sort -V | tail -1; }
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/$(latest "$SDK/ndk")}
BT=$SDK/build-tools/$(latest "$SDK/build-tools")
PLATFORM=$(ls -1d "$SDK"/platforms/android-* | sort -V | tail -1)
ANDROID_JAR=$PLATFORM/android.jar
[ -f "$NDK/build/cmake/android.toolchain.cmake" ] || { echo "No NDK found under $SDK/ndk." >&2; exit 1; }
[ -x "$BT/aapt2" ] || { echo "No build-tools found under $SDK/build-tools." >&2; exit 1; }
[ -f "$ANDROID_JAR" ] || { echo "No platform found under $SDK/platforms." >&2; exit 1; }

# MAJOR.MINOR.PATCH -> MAJOR*10000 + MINOR*100 + PATCH.
VNAME=${VERSION#v}
VNAME=${VNAME%%-*}
case "$VNAME" in
    *.*.*) ;;
    *) VNAME=0.0.0 ;;
esac
VCODE=$(echo "$VNAME" | awk -F. '{ print $1 * 10000 + $2 * 100 + $3 }')
[ "$VCODE" -gt 0 ] || VCODE=1

rm -rf "$OUT/apk" "$OUT/res"
mkdir -p "$OUT/apk/lib"

for abi in $ABIS; do
    cmake -S "$HERE" -B "$OUT/$abi" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$MIN_SDK" \
        -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
        -DCMAKE_BUILD_TYPE=Release -DIGN_VERSION="$VNAME" -Wno-deprecated >/dev/null
    cmake --build "$OUT/$abi"
    mkdir -p "$OUT/apk/lib/$abi"
    cp "$OUT/$abi/libignition.so" "$OUT/apk/lib/$abi/"
    "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip "$OUT/apk/lib/$abi/libignition.so"
done

# The launcher icon, drawn by tools/make_icon.py.
for d in mdpi:48 hdpi:72 xhdpi:96 xxhdpi:144 xxxhdpi:192; do
    mkdir -p "$OUT/res/mipmap-${d%%:*}"
    python3 "$ROOT/tools/make_icon.py" "$OUT/res/mipmap-${d%%:*}/ic_launcher.png" "${d##*:}"
done
"$BT/aapt2" compile --dir "$OUT/res" -o "$OUT/res.zip"
"$BT/aapt2" link -o "$OUT/unsigned.apk" -I "$ANDROID_JAR" \
    --manifest "$HERE/AndroidManifest.xml" -R "$OUT/res.zip" \
    --min-sdk-version "$MIN_SDK" --target-sdk-version "$TARGET_SDK" \
    --version-code "$VCODE" --version-name "$VNAME"

# Native libraries go in uncompressed, so they can be mapped in place.
(cd "$OUT/apk" && zip -q -X -0 -r "$OUT/unsigned.apk" lib)
# 16 KB pages (Android 15 devices) need build-tools 35's -P; older ones
# page-align for 4 KB only.
if "$BT/zipalign" 2>&1 | grep -q -- '-P'; then PAGE="-P 16"; else PAGE="-p"; fi
"$BT/zipalign" $PAGE -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

if [ -n "${ANDROID_KEYSTORE:-}" ]; then
    KS=$ANDROID_KEYSTORE
    KS_PASS=${ANDROID_KEYSTORE_PASS:?set ANDROID_KEYSTORE_PASS}
    ALIAS=${ANDROID_KEY_ALIAS:?set ANDROID_KEY_ALIAS}
else
    KS=$OUT/debug.keystore
    KS_PASS=android
    ALIAS=androiddebugkey
    [ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass "$KS_PASS" -keypass "$KS_PASS" \
        -alias "$ALIAS" -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass "pass:$KS_PASS" --ks-key-alias "$ALIAS" \
    --out "$OUT/ignition.apk" "$OUT/aligned.apk"
rm -f "$OUT/unsigned.apk" "$OUT/aligned.apk" "$OUT/ignition.apk.idsig"
echo "Built $OUT/ignition.apk ($VNAME, versionCode $VCODE)"
