"""Package a verified Release build using only public runtime templates."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from zipfile import ZIP_DEFLATED, ZipFile

from prepare_runtime import ROOT, runtime_files


PACKAGE = "BubbleFighter-CN0280-Server-win-x64.zip"
PACKAGE_ROOT = PACKAGE.removesuffix(".zip") + "/"
BINARIES = (
    "BubbleFighter.Server.exe",
    "libmysql.dll",
    "libssl-3-x64.dll",
    "libcrypto-3-x64.dll",
)


def digest(path):
  return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--bin", type=Path, default=ROOT/"Artifacts/Bin/x64/Release")
  parser.add_argument("--mysql-root", type=Path)
  parser.add_argument("--dependencies", type=Path,
                      default=ROOT/"Artifacts/Dependencies/vcpkg/x64-windows-static-md")
  parser.add_argument("--output", type=Path, default=ROOT/"Artifacts/Packages")
  args = parser.parse_args()
  spec = json.loads((ROOT/"build/dependencies.json").read_text(encoding="utf-8"))
  mysql = args.mysql_root or Path(os.environ.get("MySqlRoot") or
                                 ROOT/"Artifacts/Dependencies"/spec["mysql"]["directory"])
  commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
  if not re.fullmatch(r"[0-9a-f]{40}", commit):
    raise ValueError("Cannot identify the source commit")

  files = [(args.bin/name, name) for name in BINARIES]
  for folder, name in (("lib", "libmysql.dll"), ("bin", "libssl-3-x64.dll"),
                       ("bin", "libcrypto-3-x64.dll")):
    if digest(args.bin/name) != digest(mysql/folder/name):
      raise ValueError(f"Built runtime differs from the verified MySQL SDK: {name}")
  files += [(source, relative) for source, relative, _ in runtime_files()]
  files.append((ROOT/"LICENSE", "LICENSE"))
  files.append((mysql/"LICENSE", "licenses/mysql/LICENSE"))
  for name in ("libjuice", "sensitive_lexicon"):
    files.append((ROOT/"third_party"/name/"LICENSE", f"licenses/{name}/LICENSE"))
  share = args.dependencies/"share"
  for name in ("boost-asio", "nlohmann-json", "openssl", "spdlog", "fmt"):
    if not (share/name/"copyright").is_file():
      raise FileNotFoundError(f"Missing dependency license: {share/name/'copyright'}")
  files += [(path, "licenses/vcpkg/"+path.relative_to(share).as_posix())
            for path in sorted(share.glob("*/copyright"))]
  names = [name for _, name in files]
  if len(set(names)) != len(names):
    raise ValueError("Duplicate package paths")
  for source, _ in files:
    if not source.is_file():
      raise FileNotFoundError(source)

  manifest = {"product": "BubbleFighter CN0280 Server", "source_commit": commit,
              "platform": "Windows x64", "configuration": "Release",
              "mysql_version": spec["mysql"]["version"],
              "files": {name: digest(source) for source, name in files}}
  args.output.mkdir(parents=True, exist_ok=True)
  destination = args.output/PACKAGE
  candidate = destination.with_suffix(".tmp")
  with ZipFile(candidate, "w", compression=ZIP_DEFLATED, compresslevel=6) as archive:
    for source, name in files:
      archive.write(source, PACKAGE_ROOT+name)
    archive.writestr(PACKAGE_ROOT+"build-info.json", json.dumps(manifest, indent=2)+"\n")
  with ZipFile(candidate) as archive:
    executables = [name for name in archive.namelist() if name.lower().endswith(".exe")]
    if executables != [PACKAGE_ROOT+"BubbleFighter.Server.exe"]:
      raise ValueError(f"Expected one server executable, found {executables}")
    if archive.read(PACKAGE_ROOT+"config/mysql.password").decode("utf-8-sig").strip() != "CHANGE_ME":
      raise ValueError("The release must use the public password placeholder")
    if any(not path.startswith(PACKAGE_ROOT)
           or name.startswith(("bin/", "tests/", "tools/", "Artifacts/"))
           or name.endswith((".pdb", ".key", ".pem"))
           or re.search(r"(?:mysql-admin|local-mysql|mysql-local)", name)
           for path in archive.namelist()
           for name in (path.removeprefix(PACKAGE_ROOT),)):
      raise ValueError("Unexpected private or development file in the package")
    bad = archive.testzip()
    if bad:
      raise ValueError("Corrupt ZIP entry: "+bad)
  candidate.replace(destination)
  (args.output/"SHA256SUMS").write_text(f"{digest(destination)}  {PACKAGE}\n", encoding="ascii")
  print(destination.resolve())
  print((args.output/"SHA256SUMS").resolve())


if __name__ == "__main__":
  main()
