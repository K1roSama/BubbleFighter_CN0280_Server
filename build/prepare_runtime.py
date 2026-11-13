"""Prepare the executable-local runtime without replacing private settings."""

import argparse
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]


def runtime_files():
  files = [
      (ROOT/"config/examples/server.desktop.example.conf", "config/server.conf", True),
      (ROOT/"config/examples/mysql.password.example", "config/mysql.password", True),
  ]
  for folder in ("config/examples", "data/published"):
    files += [(path, path.relative_to(ROOT).as_posix(), False)
              for path in sorted((ROOT/folder).iterdir()) if path.is_file()]
  files += [(path, "migrations/"+path.name, False)
            for path in sorted((ROOT/"src/persistence/migrations").glob("[0-9][0-9][0-9][0-9]_*"))
            if path.is_file() and path.suffix in (".sql", ".json")]
  return files


def prepare_runtime(directory):
  directory.mkdir(parents=True, exist_ok=True)
  for source, relative, preserve in runtime_files():
    destination = directory/relative
    if preserve and destination.exists():
      continue
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.is_file() or source.read_bytes() != destination.read_bytes():
      shutil.copyfile(source, destination)
  (directory/"logs").mkdir(exist_ok=True)
  (directory/"data/life-attendance").mkdir(parents=True, exist_ok=True)


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--bin", type=Path, required=True)
  args = parser.parse_args()
  prepare_runtime(args.bin.resolve())


if __name__ == "__main__":
  main()

