# SPDX-License-Identifier: GPL-2.0-only
"""Check the generated payload and the shared install layout."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "firmware"))
import generate_v67  # noqa: E402


class PackageTests(unittest.TestCase):
    def test_v67_payload_matches_pin(self):
        digest = hashlib.sha256(generate_v67.build_payload()).hexdigest()
        self.assertEqual(digest, generate_v67.PAYLOAD_SHA256)
        self.assertEqual(digest, self.manifest()["generated_payload"]["sha256"])

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
            hook = root / "usr/share/initramfs-tools/hooks/gpu-unlock"
            self.assertTrue(hook.stat().st_mode & 0o111)

    def test_initramfs_tools_hook_loads_nvidia_after_unlock(self):
        hook = (ROOT / "packaging/boot/initramfs-tools-hook").read_text()
        functions = ". /usr/share/initramfs-tools/hook-functions"
        self.assertIn(functions, hook)
        stub = 'manual_add_modules() { echo "$@" >"$DESTDIR/added"; }'
        for before, after in (
                ("gpu_unlock\n", "gpu_unlock\nnvidia_drm\n"),
                ("zfs\ngpu_unlock\n", "zfs\ngpu_unlock\nnvidia_drm\n"),
                # From /etc/initramfs-tools/modules, which is read first.
                ("nvidia\nnvidia-drm modeset=1\nzfs\ngpu_unlock targets=all\n",
                 "gpu_unlock targets=all\nnvidia\nnvidia-drm modeset=1\nzfs\n")):
            with tempfile.TemporaryDirectory() as directory:
                destdir = Path(directory) / "root"
                (destdir / "conf").mkdir(parents=True)
                (destdir / "conf/modules").write_text(before)
                bindir = Path(directory) / "bin"
                bindir.mkdir()
                modinfo = bindir / "modinfo"
                modinfo.write_text('#!/bin/sh\n[ "$4" = filename ] && '
                                   'echo "/lib/modules/6.0/kernel/$5.ko.zst"\nexit 0\n')
                modinfo.chmod(0o755)
                script = Path(directory) / "hook"
                script.write_text(hook.replace(functions, stub))
                subprocess.run(["sh", str(script)], check=True,
                               env={"PATH": f"{bindir}:/usr/bin:/bin",
                                    "DESTDIR": str(destdir), "version": "6.0"})
                self.assertEqual((destdir / "conf/modules").read_text(), after)
                self.assertEqual((destdir / "added").read_text(), "nvidia_drm\n")
                self.assertEqual((destdir / "etc/modprobe.d/gpu-unlock.conf").read_text(),
                                 "blacklist nvidia\nblacklist nvidia_modeset\n"
                                 "blacklist nvidia_drm\n")

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
