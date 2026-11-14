#!/bin/bash
# Generic build components only. A phone's live DTB never enters this workflow.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
export PATH="/usr/lib/llvm-18/bin:$PATH"
export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
export PYTHONUNBUFFERED=1
MU="$ROOT/local/ci/Mu-Silicium"
mkdir -p local/ci out/uefi-ci/components
git init -q "$MU"
git -C "$MU" remote add origin https://github.com/Project-Silicium/Mu-Silicium.git
git -C "$MU" fetch --depth 1 origin 42fe563791e51bf5500c2e470ed294ca0c94b97c
git -C "$MU" -c advice.detachedHead=false checkout --detach FETCH_HEAD
git -C "$MU" submodule update --init --recursive --depth 1
python3 -m venv local/ci/mu-venv
source local/ci/mu-venv/bin/activate
python3 -m pip install -r "$MU/pip-requirements.txt" PyYAML
python3 tools/ace5/prepare-linux-arm64.py "$MU"
python3 tools/ace5/prepare-uefi.py "$MU"
(cd "$MU" && python3 build_uefi.py -d giulia -m 0 -r RELEASE)
clang-18 --target=aarch64-linux-gnu -c "$MU/BootShim/BootShim.S" \
  -DREQUIRES_KERNEL_HEADER=1 -DFD_BASE=0xA7100000 -DFD_SIZE=0x300000 \
  -DPAYLOAD_BASE=0xA8000000 -o out/uefi-ci/BootShim.elf
llvm-objcopy-18 -O binary out/uefi-ci/BootShim.elf out/uefi-ci/components/BootShim.bin
cp "$MU/Build/giuliaPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd" out/uefi-ci/components/
cp "$MU/Resources/Configs/giulia.toml" out/uefi-ci/components/
cp "$MU/Resources/Scripts/mkbootimg.py" out/uefi-ci/components/
cp "$MU/Resources/ramdisk" out/uefi-ci/components/
for stage in diagnostic kernel; do
  CC=clang-18 LLD_LINK=lld-link-18 bash tools/ace5/build-loader.sh "$stage"
done
for directory in giulia giulia-kernel; do
  mkdir -p "out/uefi-ci/components/$directory"
  cp "out/$directory/loader.efi" "out/$directory/usb-payload.bin" \
    "out/$directory/loader-build.json" "out/uefi-ci/components/$directory/"
done
python3 - <<'PY'
import hashlib, json, pathlib, subprocess
p = pathlib.Path('out/uefi-ci/components')
assert (p / 'SILICIUM_UEFI.fd').stat().st_size == 0x300000
files = {str(f.relative_to(p)).replace('\\', '/'): {
    'bytes': f.stat().st_size, 'sha256': hashlib.sha256(f.read_bytes()).hexdigest()}
    for f in p.rglob('*') if f.is_file()}
(p / 'components.json').write_text(json.dumps({
    'device': 'PKG110 / giulia', 'mu_commit': '42fe563791e51bf5500c2e470ed294ca0c94b97c',
    'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
    'fd_base': '0xa7100000', 'fd_size': 0x300000, 'payload_base': '0xa8000000',
    'device_dtb_included': False, 'files': files}, indent=2) + '\n')
PY
