#!/bin/bash
# Installed under these exact names by ci-ccache.py. xnu-clang execs this
# executable, then ccache sees the real compiler rather than another wrapper.
set -euo pipefail
: "${CCACHE_CONFIGPATH:?compiler cache has not been configured}"
case "$(basename "$0")" in
  clang-18) compiler=/usr/bin/clang-18 ;;
  clang++-18) compiler=/usr/bin/clang++-18 ;;
  *) echo 'Unexpected Ace 5 ccache wrapper name' >&2; exit 2 ;;
esac
exec /usr/bin/ccache "$compiler" "$@"
