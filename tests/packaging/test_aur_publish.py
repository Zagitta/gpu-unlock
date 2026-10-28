#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise publication against a local bare repository, never the real AUR."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / ".github/ci/aur-publish.sh"
REMOTE = "ssh://aur@aur.archlinux.org/gpu-unlock.git"
FILES = {"PKGBUILD", ".SRCINFO", "gpu-unlock.install", ".gitignore"}


class PublishTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.remote = self.root / "aur.git"
        self.package = self.root / "package"
        self.package.mkdir()
        self.env = os.environ.copy()
        self.env.update({
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": f"url.{self.remote.as_uri()}.insteadOf",
            "GIT_CONFIG_VALUE_0": REMOTE,
            # Any unintended SSH request fails rather than contacting the AUR.
            "GIT_SSH_COMMAND": "/bin/false",
            "AUR_GIT_NAME": "Test Maintainer",
            "AUR_GIT_EMAIL": "maintainer@example.invalid",
        })
        self.git("init", "--bare", "--initial-branch=master", str(self.remote))
        (self.package / "PKGBUILD").write_text(
            "# Maintainer: Test Maintainer <maintainer@example.invalid>\n"
            f"touch '{self.root / 'pkgbuild-was-executed'}'\n"
        )
        (self.package / ".SRCINFO").write_text(
            "pkgbase = gpu-unlock\n\tpkgver = 1.1.0\n\tpkgrel = 1\n"
        )
        (self.package / "gpu-unlock.install").write_text("# installation notices\n")
        (self.package / ".gitignore").write_text("/src/\n/pkg/\n")

    def git(self, *args):
        return subprocess.check_output(
            ["git", *args], env=self.env, text=True, stderr=subprocess.DEVNULL
        ).strip()

    def tip(self):
        return self.git("--git-dir", str(self.remote), "rev-parse", "refs/heads/master")

    def publish(self, expected="empty", success=True):
        result = subprocess.run(
            ["bash", str(SCRIPT), str(self.package)],
            env={**self.env, "EXPECTED_AUR_COMMIT": expected},
            text=True, capture_output=True,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / "pkgbuild-was-executed").exists())
        return result

    def test_initial_publish_only_copies_allowlisted_files_and_never_executes_pkgbuild(self):
        (self.package / "secret.key").write_text("not a real key\n")
        (self.package / "output.pkg.tar.zst").write_bytes(b"not a package")
        self.publish()
        files = self.git("--git-dir", str(self.remote), "ls-tree", "--name-only", "master")
        self.assertEqual(set(files.splitlines()), FILES)

    def test_repeat_is_noop_and_update_preserves_unrelated_remote_file(self):
        self.publish()
        first = self.tip()
        self.publish(first)
        self.assertEqual(self.tip(), first)
        checkout = self.root / "other-maintainer"
        self.git("clone", str(self.remote), str(checkout))
        (checkout / "LICENSE").write_text("existing remote license\n")
        self.git("-C", str(checkout), "add", "LICENSE")
        self.git("-C", str(checkout), "-c", "user.name=Other", "-c",
                 "user.email=other@example.invalid", "commit", "-m", "Add license")
        self.git("-C", str(checkout), "push", "origin", "master")
        latest = self.tip()
        (self.package / "gpu-unlock.install").write_text("# updated installation notices\n")
        self.publish(latest)
        self.assertNotEqual(self.tip(), latest)
        self.assertEqual(self.git("--git-dir", str(self.remote), "show", "master:LICENSE"),
                         "existing remote license")

    def test_stale_baseline_is_rejected_without_changing_remote(self):
        self.publish()
        first = self.tip()
        (self.package / "gpu-unlock.install").write_text("# changed\n")
        self.publish("0" * 40, success=False)
        self.publish("empty", success=False)
        self.assertEqual(self.tip(), first)

    def test_publication_requires_maintainer_and_expected_package_base(self):
        original = (self.package / "PKGBUILD").read_text()
        (self.package / "PKGBUILD").write_text("# Contributor: Someone\n")
        self.publish(success=False)
        (self.package / "PKGBUILD").write_text(original)
        (self.package / ".SRCINFO").write_text("pkgbase = unrelated-package\n")
        self.publish(success=False)
        self.assertEqual(self.git("--git-dir", str(self.remote), "for-each-ref",
                                 "--format=%(refname)", "refs/heads"), "")

    def test_symlinked_source_is_rejected(self):
        (self.package / "gpu-unlock.install").unlink()
        (self.package / "gpu-unlock.install").symlink_to(self.package / "PKGBUILD")
        self.publish(success=False)


if __name__ == "__main__":
    unittest.main()
