#!/bin/bash
# Cloud-only build entry. SDKs/dependencies stay outside the Git file set.
set -euo pipefail
cd "$(dirname "$0")/../.."
export PATH="/usr/lib/llvm-18/bin:$PATH"
export THEOS="$PWD/local/ci/theos"
export JOBS=${JOBS:-2}
export XNU_CC=${XNU_CC:-/usr/bin/clang-18}
export XNU_CXX=${XNU_CXX:-/usr/bin/clang++-18}
if [ "${ACE5_CCACHE:-0}" = 1 ]; then
  # A single executable path is required: xnu-clang executes "$drv" directly.
  # Do not also set make's CCACHE variable (that would double-wrap xnu-clang).
  test -x "$XNU_CC" && test -x "$XNU_CXX" && test -f "${CCACHE_CONFIGPATH:?}"
  test "$XNU_CC" = "$PWD/local/ci/ccache-bin/clang-18"
  test "$XNU_CXX" = "$PWD/local/ci/ccache-bin/clang++-18"
fi
mkdir -p out/ci/artifacts local/ci/tests
[ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ] || {
  echo 'This workflow requires a Linux ARM64 runner' >&2; exit 1;
}
case "${1:-}" in
  tests)
    python3 scripts/check-public-source.py
    python3 tools/ace5/test-phone.py
    python3 tools/ace5/test-share.py
    python3 tools/ace5/test-direct.py
    python3 tools/ace5/test-connectivity-power.py
    python3 tools/ace5/test-qrtr-input.py --cc clang-18
    python3 tools/ace5/test-eagl-surface.py --cc clang-18
    python3 tools/ace5/test-giulia-battery.py --cc clang-18
    python3 tools/ace5/test-pcie-msi.py --cc clang-18
    python3 tools/ace5/test-kiwi-dma-profile.py
    python3 tools/ace5/test-rc0-training-config.py
    python3 tools/ace5/test-capture-rc0-training.py
    python3 tools/ace5/test-pcie-bringup.py --cc clang-18
    python3 tools/ace5/test-inspect-adreno750-fw.py
    python3 tools/ace5/test-platform-guards.py --cc clang-18
    python3 tools/ace5/test-gpu-lifecycle.py --cc clang-18
    python3 tools/ace5/test-loader-state.py --cc clang-18
    for name in loader-images s3910-report gmu-fw gmu-hfi gpu-direct gpu-live gpu-submit battery-state giulia-cpufreq kiwi-reg wifi-bringup; do
      clang-18 -std=gnu11 -O1 -UNDEBUG -g -fsanitize=address,undefined \
        "tools/ace5/test-$name.c" -o "local/ci/tests/$name"
      "local/ci/tests/$name"
    done
    ;;
  loader)
    CC=clang-18 LLD_LINK=lld-link-18 bash tools/ace5/build-loader.sh diagnostic
    CC=clang-18 LLD_LINK=lld-link-18 bash tools/ace5/build-loader.sh kernel
    for stage in giulia giulia-kernel; do
      mkdir -p "out/ci/artifacts/$stage"
      cp "out/$stage/loader.efi" "out/$stage/usb-payload.bin" \
        "out/$stage/loader-build.json" "out/ci/artifacts/$stage/"
    done
    ;;
  dependencies)
    archive=local/ci/iOSToolchain-aarch64.tar.xz
    toolchain_sha=9051e9d531de5e511f0fab40d925d625bc6efe7ed9dce821d179c09de9bc690b
    if ! printf '%s  %s\n' "$toolchain_sha" "$archive" | sha256sum -c - >/dev/null 2>&1 || [ ! -x "$THEOS/toolchain/linux/iphone/bin/ld" ]; then
      rm -rf "$THEOS/toolchain"
      curl -fL --retry 3 -o "$archive" \
        https://github.com/L1ghtmann/llvm-project/releases/download/test-210562a/iOSToolchain-aarch64.tar.xz
      printf '%s  %s\n' "$toolchain_sha" "$archive" | sha256sum -c -
      mkdir -p "$THEOS/toolchain"
      tar -xJf "$archive" -C "$THEOS/toolchain"
    fi
    sha256sum "$archive" > out/ci/toolchain.sha256
    test -x "$THEOS/toolchain/linux/iphone/bin/ld"
    if [ ! -d "$THEOS/sdks/iPhoneOS16.5.sdk/usr/include" ]; then
      rm -rf "$THEOS/sdks"
      git init -q "$THEOS/sdks"
      git -C "$THEOS/sdks" remote add origin https://github.com/theos/sdks.git
      git -C "$THEOS/sdks" sparse-checkout init --cone
      git -C "$THEOS/sdks" sparse-checkout set iPhoneOS16.5.sdk
      git -C "$THEOS/sdks" fetch --depth 1 --filter=blob:none origin 0222fd5413cf4b9af096f37b4621afa2688572f7
      git -C "$THEOS/sdks" -c advice.detachedHead=false checkout --detach FETCH_HEAD
    fi
    test "$(git -C "$THEOS/sdks" rev-parse HEAD)" = 0222fd5413cf4b9af096f37b4621afa2688572f7
    test -d "$THEOS/sdks/iPhoneOS16.5.sdk/usr/include"
    bash xnu-deps/setup.sh --stage1
    bash xnu/apply.sh
    bash xnu-deps/setup.sh --stage2
    python3 tools/ace5/ci-record.py inventory after-dependencies
    ;;
  xnu)
    bash xnu-build.sh -j"$JOBS" build
    kernel=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
    test -s "$kernel"
    cp "$kernel" out/ci/artifacts/mach.ace5
    ;;
  *) echo 'Usage: ci-build.sh tests|loader|dependencies|xnu' >&2; exit 2 ;;
esac
