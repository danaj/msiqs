#!/usr/bin/env python3
"""Build the persistent FactorDB worker from a current MPU-GMP checkout."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
# The ordinary MPU-GMP translation units from Makefile.PL, without Perl XS.
MPU_SOURCES = (
    "factor gmp_main lanczos siqs tinyecm64 primality bls75 ecpp aks utility "
    "misc_ui factmod znlog rootmod legendre_phi prime_iterator pr32_iter "
    "poly squfof126 ecm lucas_seq real isaac random_prime perfect_powers powerfree"
).split()


def gmp_flags(option, fallback):
    try:
        result = subprocess.run(["pkg-config", option, "gmp"], check=True, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        return shlex.split(result.stdout)
    except (OSError, subprocess.CalledProcessError):
        return fallback


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mpu-src", type=Path,
                        default=HERE.parent.parent / "Math-Prime-Util-GMP")
    parser.add_argument("--output", type=Path, default=Path("factordb-batch-worker"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cppflags", default=os.environ.get("CPPFLAGS", ""))
    parser.add_argument("--cflags", default=os.environ.get("CFLAGS", "-O3"))
    parser.add_argument("--ldflags", default=os.environ.get("LDFLAGS", ""))
    args = parser.parse_args()
    root = args.mpu_src.resolve()
    sources = [root / (name + ".c") for name in MPU_SOURCES]
    missing = [str(path) for path in sources if not path.is_file()]
    if missing:
        parser.error("missing MPU-GMP sources: " + ", ".join(missing))
    compiler = shlex.split(args.cc)
    if not compiler:
        parser.error("--cc must name a compiler")
    output = args.output.absolute()
    if os.name == "nt" and not output.suffix:
        output = output.with_suffix(".exe")
    try:
        with tempfile.TemporaryDirectory(prefix=".factordb-worker-", dir=output.parent) as temp:
            build = Path(temp) / output.name
            command = (
                compiler + shlex.split(args.cppflags) + shlex.split(args.cflags) +
                ["-DSTANDALONE", "-I", str(root)] + gmp_flags("--cflags", []) +
                [str(HERE / "factordb-batch-worker.c")] + [str(path) for path in sources] +
                shlex.split(args.ldflags) + gmp_flags("--libs", ["-lgmp"]) +
                ["-lm", "-o", str(build)]
            )
            subprocess.run(command, check=True)
            # A Windows-target compiler can add .exe even when cross-compiling.
            if not build.suffix and not build.exists() and build.with_suffix(".exe").is_file():
                build = build.with_suffix(".exe")
                output = output.with_suffix(".exe")
            os.replace(build, output)
    except (OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Worker build failed: {exc}\n")
    print(f"Built {output}")


if __name__ == "__main__":
    main()
