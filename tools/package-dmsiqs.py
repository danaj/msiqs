#!/usr/bin/env python3
"""Create a standalone dmsiqs source archive from the listed files."""

import argparse
import io
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile


VERSION = re.compile(r"(?m)^#define[ \t]+DS_VERSION[ \t]+([0-9]+)[uU]?[ \t]*$")
STAMP = re.compile(rb"[0-9a-fA-F]{8} [0-9]{4}-[0-9]{2}-[0-9]{2}\n")
BUILD_INFO = Path("DMSIQS_BUILD")
PREFIX = "dmsiqs-worker"


def build_stamp():
    if Path(".git").exists():
        try:
            result = subprocess.run(["git", "log", "-1", "--format=%H %cs", "HEAD"],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        except OSError:
            pass
        else:
            fields = result.stdout.split()
            if result.returncode == 0 and len(fields) == 2:
                stamp = fields[0][:8] + b" " + fields[1] + b"\n"
                if STAMP.fullmatch(stamp):
                    return stamp
    if BUILD_INFO.is_file():
        stamp = BUILD_INFO.read_bytes()
        if not stamp.endswith(b"\n"):
            stamp += b"\n"
        if not STAMP.fullmatch(stamp):
            raise ValueError("invalid DMSIQS_BUILD revision or date")
        return stamp
    return None


def package(names):
    files = dict.fromkeys(map(Path, names))
    for path in files:
        if path.is_absolute() or ".." in path.parts or path.is_symlink() or not path.is_file():
            raise ValueError(f"expected a relative regular file: {path}")
    match = VERSION.search(Path("dmsiqs_wire.h").read_text())
    if not match:
        raise ValueError("dmsiqs_wire.h does not define a numeric DS_VERSION")
    output = Path(f"{PREFIX}-{match[1]}.tar.gz")
    stamp = build_stamp()
    temporary = tempfile.NamedTemporaryFile(prefix=f".{output.name}.", dir=output.parent,
                                            delete=False)
    try:
        with temporary, tarfile.open(str(output), "w:gz", fileobj=temporary) as archive:
            for path in files:
                if path != BUILD_INFO:
                    archive.add(path, arcname=f"{PREFIX}/{path.as_posix()}", recursive=False)
            if stamp is not None:
                info = tarfile.TarInfo(f"{PREFIX}/{BUILD_INFO}")
                info.size = len(stamp)
                info.mode = 0o644
                archive.addfile(info, io.BytesIO(stamp))
        Path(temporary.name).replace(output)
    finally:
        Path(temporary.name).unlink(missing_ok=True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", help="relative source, tool, and documentation paths")
    args = parser.parse_args()
    try:
        print(package(args.files))
    except (OSError, ValueError, tarfile.TarError) as error:
        print(f"package-dmsiqs: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
