# SPDX-License-Identifier: GPL-2.0-only
"""Check the generated payload and the shared install layout."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "firmware"))
import generate_v67  # noqa: E402


class PackageTests(unittest.TestCase):
    def test_v67_payload_matches_pin(self):
        digest = hashlib.sha256(generate_v67.build_payload()).hexdigest()
        self.assertEqual(digest, generate_v67.PAYLOAD_SHA256)
        self.assertEqual(digest, self.manifest()["generated_payload"]["sha256"])

    def test_compiled_payload_header_matches_generator(self):
        self.assertEqual((ROOT / "kernel/gpu_unlock/payload_words.h").read_text(),
                         generate_v67.payload_header())

    def test_payload_file_is_opt_in(self):
        manifest = self.manifest()
        members = {".PKGINFO": f"pkgver = {generate_v67.PACKAGE_VERSION}\n".encode()}
        for index, item in enumerate(manifest["source_artifacts"][2]["firmware"]):
            blob = f"GSP fixture {index}".encode()
            members[item["path"]] = blob
            item["size"] = len(blob)
            item["sha256"] = hashlib.sha256(blob).hexdigest()
        notice = manifest["source_artifacts"][2]["notice"]
        members[notice["path"]] = b"license fixture"
        notice["sha256"] = hashlib.sha256(members[notice["path"]]).hexdigest()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "package.tar"
            package.write_bytes(b"package fixture")
            manifest_path = root / "manifest.json"
            manifest_path.write_text(json.dumps(manifest))
            for mode in ("default", "payload", "header"):
                with self.subTest(mode=mode):
                    output = root / mode
                    header = root / "payload_words.h"
                    argv = ["generate_v67.py", str(package), str(output)]
                    if mode == "payload":
                        argv += ["--payload"]
                    if mode == "header":
                        argv += ["--header", str(header)]
                    with mock.patch.object(generate_v67, "MANIFEST", manifest_path), \
                         mock.patch.object(generate_v67, "PACKAGE_SHA256", hashlib.sha256(package.read_bytes()).hexdigest()), \
                         mock.patch.object(generate_v67, "extract_member", side_effect=lambda _, name: members[name]), \
                         mock.patch.object(sys, "argv", argv):
                        self.assertEqual(generate_v67.main(), 0)
                    payload = output / "cmp-signature.bin"
                    self.assertEqual(payload.exists(), mode == "payload")
                    if payload.exists():
                        self.assertEqual(payload.read_bytes(), generate_v67.build_payload())
                    if mode == "header":
                        self.assertEqual(header.read_text(), generate_v67.payload_header())

    def test_firmware_install_inventory(self):
        assets = Path(os.environ.get("GPU_UNLOCK_ASSETS", ""))
        manifest = self.manifest()
        names = set(manifest["derived_assets"]) | {"gsp_ga10x.bin", "gsp_tu10x.bin"}
        if not all((assets / name).is_file() for name in names):
            self.skipTest("Stage 1 pinned assets are not available")
        for item in manifest["source_artifacts"][2]["firmware"]:
            data = (assets / Path(item["path"]).name).read_bytes()
            self.assertEqual(len(data), item["size"])
            self.assertEqual(hashlib.sha256(data).hexdigest(), item["sha256"])
        for name, digest in manifest["derived_assets"].items():
            self.assertEqual(hashlib.sha256((assets / name).read_bytes()).hexdigest(), digest)
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["make", "-s", f"DESTDIR={directory}", f"ASSETS={assets}",
                            "install-firmware"], cwd=ROOT, check=True)
            installed = Path(directory) / "usr/lib/firmware/gpu-unlock/610.43.03"
            self.assertEqual({p.name for p in installed.iterdir()}, names)
            for name in names:
                self.assertEqual((installed / name).read_bytes(), (assets / name).read_bytes())

    def test_install_uses_standard_paths(self):
        version = (ROOT / "VERSION").read_text().strip()
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(["make", "-s", f"DESTDIR={directory}", "install-dkms",
                            "install-mkinitcpio", "install-initramfs-tools",
                            "install-dracut"],
                           cwd=ROOT, check=True)
            root = Path(directory)
            paths = {path.relative_to(root).as_posix() for path in root.rglob("*")
                     if path.is_file()}
            for expected in (f"usr/src/gpu-unlock-{version}/dkms.conf",
                             f"usr/src/gpu-unlock-{version}/main.c",
                             "etc/mkinitcpio.conf.d/zz-gpu-unlock.conf",
                             "usr/share/initramfs-tools/modules.d/gpu-unlock.conf",
                             "usr/lib/dracut/dracut.conf.d/gpu-unlock.conf",
                             "usr/share/doc/gpu-unlock/README.md"):
                self.assertIn(expected, paths)
            docs = [p.relative_to(ROOT).as_posix() for p in (ROOT / "docs").iterdir()]
            self.assertTrue(docs)
            for doc in docs:
                self.assertIn(f"usr/share/doc/gpu-unlock/{doc}", paths)
            self.assertFalse(any(path.endswith(".mod.c") for path in paths))
            # Another package's "softdep nvidia" line would silently win over
            # ours (kmod honours only the first), so the order comes from the
            # initramfs instead: gpu_unlock goes right before nvidia.
            self.assertFalse(any("modprobe.d" in path for path in paths))
            dropin = root / "etc/mkinitcpio.conf.d/zz-gpu-unlock.conf"
            for before, after in (("zfs nvidia nvidia_drm", "zfs gpu_unlock? nvidia nvidia_drm"),
                                  ("zfs", "zfs gpu_unlock?")):
                result = subprocess.run(["bash", "-c", f"MODULES=({before}); . {dropin}; echo ${{MODULES[*]}}"],
                                        capture_output=True, text=True, check=True)
                self.assertEqual(result.stdout.strip(), after)

    def test_docs_survive_source_archive(self):
        # Packages build from `git archive HEAD`, which honours export-ignore.
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=ROOT,
                             capture_output=True, text=True)
        if top.returncode or Path(top.stdout.strip()) != ROOT:
            self.skipTest("not a git checkout")
        docs = subprocess.run(["git", "ls-files", "docs"], cwd=ROOT, capture_output=True,
                              text=True, check=True).stdout.splitlines()
        self.assertTrue(docs)
        for doc in docs:
            attr = subprocess.run(["git", "check-attr", "export-ignore", "--", doc],
                                  cwd=ROOT, capture_output=True, text=True, check=True)
            self.assertTrue(attr.stdout.rstrip().endswith(": unset"), attr.stdout)

    def test_pkgbuild_pins_match_manifest(self):
        manifest = json.loads((ROOT / "firmware/artifacts-610.43.03.json").read_text())
        pkgbuild = (ROOT / "packaging/arch/PKGBUILD").read_text()
        for index in (0, 2):
            artifact = manifest["source_artifacts"][index]
            self.assertIn(artifact["sha256"], pkgbuild)
            self.assertIn(artifact["url"].rsplit("/", 1)[0], pkgbuild)
        self.assertIn(f"pkgver={(ROOT / 'VERSION').read_text().strip()}\n", pkgbuild)

    def test_native_versions_match(self):
        version = (ROOT / "VERSION").read_text().strip()
        spec = (ROOT / "packaging/rpm/gpu-unlock.spec").read_text()
        self.assertIn(f"Version: {version}\n", spec)
        for index in (0, 2):
            self.assertIn(self.manifest()["source_artifacts"][index]["sha256"], spec)
        changelog = (ROOT / "debian/changelog").read_text()
        self.assertTrue(changelog.startswith(f"gpu-unlock ({version}) "))

    @staticmethod
    def manifest():
        return json.loads((ROOT / "firmware/artifacts-610.43.03.json").read_text())
