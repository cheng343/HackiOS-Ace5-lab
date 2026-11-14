#!/bin/bash
# Build project-owned runtime code before an IPSW is available. No phone access.
set -euo pipefail
cd "$(dirname "$0")/../.."
export PATH="/usr/lib/llvm-18/bin:$PATH"
export THEOS="$PWD/local/ci/theos"
export XNU_CC=${XNU_CC:-/usr/bin/clang-18}
export XNU_CXX=${XNU_CXX:-/usr/bin/clang++-18}
source ./xnu-env.sh
mkdir -p out/runtime-ci/components out/runtime-ci/programs out/runtime-ci/host-tools
AIR=1 DRIVERS=softpipe,freedreno bash tools/mk-mesa.sh
GL=1 PREBUILT= COMPONENTS_ONLY=1 bash userland/build.sh
bash tools/ace5/build-gpu-userland.sh out/runtime-ci/components --metal
find userland/obj -maxdepth 1 -type f ! -name '*.o' ! -name '*.new' \
  -exec cp '{}' out/runtime-ci/programs/ ';'
bash tools/apfs/build-tool.sh "$PWD/out/runtime-ci/host-tools/apfs-tool"
cc -O2 -o out/runtime-ci/host-tools/unzbm xnu-deps/build/lzbitmap/unzbm.c \
  xnu-deps/build/lzbitmap/libzbitmap.c
cp LICENSE NOTICE.md out/runtime-ci/
python3 - <<'PY'
import hashlib, json, pathlib, subprocess
p = pathlib.Path('out/runtime-ci')
files = {str(f.relative_to(p)): {'bytes': f.stat().st_size,
    'sha256': hashlib.sha256(f.read_bytes()).hexdigest()}
    for folder in ('components', 'programs', 'host-tools')
    for f in (p / folder).rglob('*') if f.is_file()}
for required in ('programs/init', 'programs/stage0', 'programs/libredwood.dylib',
                 'components/libOSMesa.8.dylib', 'components/libredwood-gl.dylib',
                 'components/libredwoodmetal.dylib', 'components/libredwoodair.dylib'):
    assert required in files, required
(p / 'components.json').write_text(json.dumps({
    'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
    'ios_system_included': False, 'device_firmware_included': False,
    'bootable_image': False, 'files': files}, indent=2) + '\n')
PY
