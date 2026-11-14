#!/usr/bin/env python3
"""Configure and record the cloud XNU compiler cache (never cache BUILD/SDKs).

Only xnu-clang/xnu-clang++ calls use these wrappers. Host tools, loader and
regression-test compilations still use their ordinary compilers.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "out/ci"
STATE = ROOT / "local/ci"
IDENTITY_FILES = (
    "tools/ace5/ci-ccache.py", "tools/ace5/ci-ccache-compiler.sh",
    "xnu-env.sh", "xnu-build.sh", "xnu/apply.sh",
    "xnu-deps/setup.sh", "xnu-deps/host/bin/xnu-clang",
)


def digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def command(*args: str) -> str:
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT,
                                   timeout=60)


def save(path: Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def keys(identity: dict, sha: str, run_id: str, attempt: str) -> tuple[str, str, str]:
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("expected a full Git source SHA")
    if not run_id.isdigit() or not attempt.isdigit():
        raise ValueError("expected numeric GitHub run ID and attempt")
    abi = hashlib.sha256(json.dumps(identity, sort_keys=True,
                                    separators=(",", ":")).encode()).hexdigest()
    prefix = f"ace5-xnu-ccache-v1-Linux-ARM64-{abi}-"
    # Actions cache keys are immutable. Each attempt can save additional valid
    # entries, including after a failed build, without overwriting an older key.
    return prefix + sha + "-" + run_id + "-" + attempt, prefix + sha + "-", prefix


def compiler_packages() -> dict:
    # clang is dynamically linked on Ubuntu: driver content alone does not
    # describe libclang-cpp/libLLVM or the installed builtin/resource headers.
    packages = {}
    raw = command("dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${db:Status-Abbrev}\n")
    for line in raw.splitlines():
        fields = line.split("\t")
        if len(fields) != 3 or not fields[2].startswith("ii"):
            continue
        name = fields[0].split(":", 1)[0]
        if name in ("ccache", "libc6", "libstdc++6") or re.fullmatch(
                r"(?:clang-18|llvm-18|lld-18|libllvm18|libclang.*18)(?:-.*)?", name):
            packages[fields[0]] = fields[1]
    present = {name.split(":", 1)[0] for name in packages}
    if not {"clang-18", "llvm-18", "lld-18", "ccache"} <= present:
        raise ValueError("required compiler/cache package identity is missing")
    return packages


def build_identity(root: Path) -> dict:
    build = (root / "tools/ace5/ci-build.sh").read_text(encoding="utf-8")
    sdk = re.findall(r"fetch --depth 1 --filter=blob:none origin ([0-9a-f]{40})", build)
    toolchain = re.findall(r"\b[0-9a-f]{64}\b", build)
    if len(set(sdk)) != 1 or len(set(toolchain)) != 1:
        raise ValueError("review cache identity: SDK/toolchain pin format changed")
    return {
        "schema": 1,
        "os": platform.system(), "arch": platform.machine(),
        "os_release": Path("/etc/os-release").read_text(encoding="utf-8"),
        # Keep real CWD/debug paths in the compiler hash. Same-repository hosted
        # runners use this stable checkout path; incompatible locations get a
        # different restore prefix instead of reusing misleading debug paths.
        "workspace": str(root.resolve()),
        "compiler": {name: {"sha256": digest(Path("/usr/bin") / name),
                              "version": command("/usr/bin/" + name, "--version")}
                     for name in ("clang-18", "clang++-18")},
        "packages": compiler_packages(),
        "sdk_commit": sdk[0], "toolchain_sha256": toolchain[0],
        # Covers target/flags, SDK/dependency pins, generated-header recipes and
        # wrappers. Kernel patches/source are deliberately not the ABI prefix:
        # ccache hashes the real source, included headers and compiler flags.
        "recipes": {name: digest(root / name) for name in IDENTITY_FILES},
    }


def append_actions(path_var: str, values: dict) -> None:
    path = os.environ.get(path_var)
    if not path:
        raise ValueError(f"{path_var} is required on the cloud runner")
    with open(path, "a", encoding="utf-8") as output:
        for key, value in values.items():
            if "\n" in str(value) or "\r" in str(value):
                raise ValueError("unexpected newline in Actions value")
            output.write(f"{key}={value}\n")


def configure() -> None:
    if platform.system() != "Linux" or platform.machine() != "aarch64":
        raise ValueError("cache setup requires the Linux ARM64 cloud runner")
    identity = build_identity(ROOT)
    sha = os.environ["GITHUB_SHA"]
    key, source_prefix, prefix = keys(identity, sha, os.environ["GITHUB_RUN_ID"],
                                       os.environ["GITHUB_RUN_ATTEMPT"])
    cache = STATE / "ccache"
    wrappers = STATE / "ccache-bin"
    config = STATE / "ccache.conf"
    cache.mkdir(parents=True, exist_ok=True)
    wrappers.mkdir(parents=True, exist_ok=True)
    for name in ("clang-18", "clang++-18"):
        target = wrappers / name
        shutil.copyfile(ROOT / "tools/ace5/ci-ccache-compiler.sh", target)
        target.chmod(0o755)
    # Config lives OUTSIDE the restored cache. Do not accept stale config from
    # a previous attempt. No sloppiness, header exclusions or timestamp bypass.
    config.write_text("\n".join((
        f"cache_dir = {cache}", "max_size = 4GiB", "compiler_check = content",
        "hash_dir = true", "base_dir =", "sloppiness =", "hard_link = false",
        "compression = true", "stats = true", "",
    )), encoding="utf-8")
    save(OUT / "ccache-identity.json", {"identity": identity, "source_sha": sha,
         "key": key, "source_restore_prefix": source_prefix, "restore_prefix": prefix,
         "scope": "xnu-clang and xnu-clang++; fresh dependency/header/BUILD generation"})
    append_actions("GITHUB_OUTPUT", {"key": key, "source-prefix": source_prefix,
                                    "prefix": prefix, "path": str(cache)})
    append_actions("GITHUB_ENV", {
        "CCACHE_DIR": str(cache), "CCACHE_CONFIGPATH": str(config),
        "XNU_CC": str(wrappers / "clang-18"), "XNU_CXX": str(wrappers / "clang++-18"),
        "ACE5_CCACHE": "1",
    })
    print(f"Prepared 4 GiB compiler cache: {key}")


def start() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    save(OUT / "ccache-restore.json", {
        "requested_key": os.environ.get("ACE5_CACHE_KEY"),
        "matched_key": os.environ.get("ACE5_CACHE_MATCHED_KEY", ""),
        "exact_action_match": os.environ.get("ACE5_CACHE_HIT", ""),
        "note": "Actions restore matching is not a compiler cache hit statistic.",
    })
    (OUT / "ccache-config.txt").write_text(command("ccache", "--show-config"), encoding="utf-8")
    (OUT / "ccache-before.txt").write_text(command("ccache", "--show-stats", "--verbose"),
                                          encoding="utf-8")
    print(command("ccache", "--zero-stats"), end="")


def stats() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    # The 4 GiB limit is also enforced immediately before the archive is saved.
    print(command("ccache", "--cleanup"), end="")
    text = command("ccache", "--show-stats", "--verbose")
    raw = command("ccache", "--print-stats")
    (OUT / "ccache-after.txt").write_text(text, encoding="utf-8")
    (OUT / "ccache-stats.tsv").write_text(raw, encoding="utf-8")
    counters = {}
    for line in raw.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1].isdigit():
            counters[fields[0]] = int(fields[1])
    save(OUT / "ccache-stats.json", counters)
    print(text, end="")


def main() -> None:
    actions = {"configure": configure, "start": start, "stats": stats}
    if len(sys.argv) != 2 or sys.argv[1] not in actions:
        raise ValueError("Usage: ci-ccache.py configure|start|stats")
    actions[sys.argv[1]]()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
