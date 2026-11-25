#!/usr/bin/env python3
"""Build the native desktop and collector and assemble an installable package."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
build = root / "build-native"
windows = sys.platform == "win32"


def run(args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)


def project(name, manifest):
    directory = build / name
    directory.mkdir(parents=True, exist_ok=True)
    args = ["qmake", root / manifest, "CONFIG+=release", "CONFIG-=debug_and_release"]
    if sys.platform == "darwin":
        args.append("QMAKE_APPLE_DEVICE_ARCHS=x86_64 arm64")
    run(args, directory)
    run(["nmake", "/NOLOGO"] if windows else ["make", "-j4"], directory)
    return directory


def package_windows(desktop, node):
    stage = build / "stage"
    stage.mkdir()
    for directory, name in [(desktop, "hopview.exe"), (node, "hopview-node.exe")]:
        shutil.copy2(directory / name, stage)
    run(["windeployqt", "--release", "--qmldir", root / "src", "--no-translations",
         "--no-compiler-runtime", stage / "hopview.exe"])
    # These projects use the dynamic vcpkg triplet and the release MSVC runtime.
    for dll in (Path(os.environ["HOPVIEW_DEPS"]) / "bin").glob("*.dll"):
        shutil.copy2(dll, stage)
    redist = Path(os.environ["VCToolsRedistDir"]) / "x64"
    for dll in next(redist.glob("Microsoft.VC*.CRT")).glob("*.dll"):
        shutil.copy2(dll, stage)
    compiler = shutil.which("ISCC") or str(Path(os.environ["ProgramFiles(x86)"]) /
                                            "Inno Setup 6" / "ISCC.exe")
    run([compiler, "/Qp", root / "deploy/windows.iss"])


def package_mac(desktop, node):
    stage = build / "stage"
    stage.mkdir()
    app = stage / "Hopview.app"
    shutil.copytree(desktop / "hopview.app", app, symlinks=True)
    helper = app / "Contents/MacOS/hopview-node"
    shutil.copy2(node / "hopview-node", helper)
    run(["macdeployqt", app, f"-qmldir={root / 'src'}", f"-executable={helper}",
         "-always-overwrite", "-codesign=-"])
    run(["codesign", "--verify", "--deep", "--strict", app])
    (stage / "Applications").symlink_to("/Applications")
    run(["hdiutil", "create", "-ov", "-format", "UDZO", "-volname", "Hopview",
         "-srcfolder", stage, build / "Hopview-macOS.dmg"])


if __name__ == "__main__":
    if not windows and sys.platform != "darwin":
        sys.exit("Run this package build on Windows or macOS.")
    app = project("desktop", "hopview.pro")
    collector = project("node", "node.pro")
    # stage contains only the deployment tree this command created.
    stage = build / "stage"
    if stage.exists():
        shutil.rmtree(stage)
    if windows:
        package_windows(app, collector)
    else:
        package_mac(app, collector)
