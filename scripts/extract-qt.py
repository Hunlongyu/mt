"""Extract pinned Qt archives with Python 3.12+, without depending on host tar/xz."""

import argparse
import re
import shutil
import sys
import tarfile
from pathlib import Path, PurePosixPath


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--licenses", action="store_true")
    args = parser.parse_args()
    if sys.version_info < (3, 12):
        parser.error("Python 3.12 or later is required")
    destination = args.destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    notice = re.compile(
        r"/LICENSES/[^/]+$|/src/3rdparty/(.+/)?"
        r"(LICENSE[^/]*|COPYING[^/]*|COPYRIGHT[^/]*|NOTICE[^/]*|qt_attribution.json|README[^/]*)$",
        re.IGNORECASE,
    )
    count = 0
    with tarfile.open(args.archive, mode="r|xz") as archive:
        for member in archive:
            if not args.licenses:
                archive.extract(member, path=destination, filter="data")
                count += 1
                continue
            if not notice.search(member.name):
                continue
            # Never silently omit a notice or follow a link outside the staging directory.
            relative = PurePosixPath(member.name)
            if not member.isfile() or relative.is_absolute() or ".." in relative.parts:
                raise ValueError(f"Unsupported license entry: {member.name}")
            target = destination.joinpath(*relative.parts)
            if not target.resolve().is_relative_to(destination):
                raise ValueError(f"License path escapes destination: {member.name}")
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.extractfile(member) as source, target.open("wb") as output:
                shutil.copyfileobj(source, output)
            count += 1
    if not count:
        raise ValueError(f"No matching files in {args.archive}")
    print(f"Extracted {count} entries from {args.archive.name}", flush=True)


if __name__ == "__main__":
    main()
