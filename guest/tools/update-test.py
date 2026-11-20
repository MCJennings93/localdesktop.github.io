#!/usr/bin/env python3
"""Tests localdesktop-update's pure parts on any PC with Python 3:

    python3 guest/tools/update-test.py

The pacman.conf rewrite (holds, mirror list), the fallback mirror list, `pacman -Qu` parsing, the
progress bar's steps and telling a failed download from other failures.
"""
import importlib.machinery
import importlib.util
import os
import sys
import unittest

sys.dont_write_bytecode = True  # no __pycache__ in guest/managed, which ships as is

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_PATH = os.path.join(HERE, "../managed/usr/local/bin/localdesktop-update")
loader = importlib.machinery.SourceFileLoader("localdesktop_update", TOOL_PATH)
spec = importlib.util.spec_from_loader("localdesktop_update", loader)
update = importlib.util.module_from_spec(spec)
loader.exec_module(update)

CONF = """[options]
HoldPkg     = pacman glibc
IgnorePkg   = libpipewire pipewire pipewire-alsa wireplumber gamescope
Architecture = auto

[core]
Include = /etc/pacman.d/mirrorlist

[extra]
Include = /etc/pacman.d/mirrorlist
"""

MIRRORLIST = """# Arch Linux ARM
Server = http://mirror.archlinuxarm.org/$arch/$repo
# Server = http://dk.mirror.archlinuxarm.org/$arch/$repo
# Server = http://nj.us.mirror.archlinuxarm.org/$arch/$repo
#Server = http://mirror.archlinuxarm.org/$arch/$repo
"""


class UpdateConfig(unittest.TestCase):
    def test_lifts_pipewire_keeps_gamescope(self):
        out = update.update_config(CONF)
        self.assertIn("IgnorePkg   = gamescope\n", out)
        self.assertNotIn("pipewire", out)

    def test_keeps_users_own_holds(self):
        out = update.update_config(CONF.replace("gamescope", "gamescope linux-firmware"))
        self.assertIn("IgnorePkg   = gamescope linux-firmware\n", out)

    def test_drops_line_left_empty(self):
        out = update.update_config(CONF.replace(" gamescope", ""))
        self.assertNotIn("IgnorePkg", out)
        self.assertIn("HoldPkg     = pacman glibc\n", out)

    def test_swaps_mirrorlist_only_when_given(self):
        self.assertEqual(update.update_config(CONF).count("Include = /etc/pacman.d/mirrorlist"), 2)
        out = update.update_config(CONF, mirrorlist="/tmp/m")
        self.assertEqual(out.count("Include = /tmp/m\n"), 2)
        self.assertNotIn("/etc/pacman.d/mirrorlist", out)


class Mirrors(unittest.TestCase):
    def test_active_and_spare(self):
        active, spare = update.mirrors(MIRRORLIST)
        self.assertEqual(active, ["http://mirror.archlinuxarm.org/$arch/$repo"])
        # The commented-out copy of the active server isn't a fallback.
        self.assertEqual(spare, ["http://dk.mirror.archlinuxarm.org/$arch/$repo",
                                 "http://nj.us.mirror.archlinuxarm.org/$arch/$repo"])

    def test_fallback_order(self):
        active, _ = update.mirrors(MIRRORLIST)
        ranked = ["http://nj.us.mirror.archlinuxarm.org/$arch/$repo",
                  "http://dk.mirror.archlinuxarm.org/$arch/$repo"]
        self.assertEqual(update.fallback_mirrorlist(active, ranked).splitlines(), [
            "Server = http://mirror.archlinuxarm.org/$arch/$repo",
            "Server = http://nj.us.mirror.archlinuxarm.org/$arch/$repo",
            "Server = http://dk.mirror.archlinuxarm.org/$arch/$repo",
        ])


class Pending(unittest.TestCase):
    OUTPUT = """openssl 3.6.5-1 -> 4.1.0-1
gamescope 3.16.29-4 -> 3.17.0-1 [ignored]
openssh 10.6p1-2 -> 10.6p2-1
zlib 1:1.3.2-4 -> 1:1.4-1
"""

    def test_parse_skips_ignored(self):
        pending = update.parse_pending(self.OUTPUT)
        self.assertEqual([name for name, _, _ in pending], ["openssl", "openssh", "zlib"])
        self.assertEqual(pending[2], ("zlib", "1:1.3.2-4", "1:1.4-1"))

    def test_notable(self):
        pending = update.parse_pending(self.OUTPUT + "kwin 6.8.2-1 -> 6.8.3-1\n")
        self.assertEqual(update.notable(pending), ["openssl", "openssh", "zlib"])

    def test_summary(self):
        text = update.summary({"pending": 1, "notable": []})
        self.assertTrue(text.startswith("1 package update waiting."))
        text = update.summary({"pending": 276, "notable": ["openssl", "curl"]})
        self.assertTrue(text.startswith("276 package updates waiting, including openssl, curl."))


class Progress(unittest.TestCase):
    def test_steps(self):
        state = {}
        lines = [
            ":: Synchronizing package databases...",
            "Packages (4) a-1 b-1 c-1 d-1",
            " a-1-aarch64 downloading...",
            " b-1-aarch64 downloading...",
            ":: Processing package changes...",
            "upgrading a...",
            "installing d...",
            ":: Running post-transaction hooks...",
        ]
        steps = [update.progress(line, state) for line in lines]
        self.assertIsNone(steps[0])
        self.assertEqual(steps[1], ("Downloading updates", 2))
        self.assertEqual(steps[3], ("Downloading updates", 26))
        self.assertEqual(steps[4], ("Installing updates", 50))
        self.assertEqual(steps[5], ("Installing a", 61))
        self.assertEqual(steps[6], ("Installing d", 72))
        self.assertEqual(steps[7], ("Finishing", 96))

    def test_counter_form(self):
        state = {"total": 2}
        self.assertEqual(update.progress("(  1/2) upgrading a", state), ("Installing a", 72))
        self.assertEqual(update.progress("(2/2) upgrading b", state), ("Installing b", 95))

    def test_installed(self):
        self.assertEqual(update.installed("upgrading expat...\n"), "expat")
        self.assertEqual(update.installed("(  3/9) installing lz4"), "lz4")
        self.assertIsNone(update.installed("removing old-thing..."))
        self.assertIsNone(update.installed(":: upgrading is fun"))
        self.assertIsNone(update.installed("warning: directory permissions differ on /usr/share/cmake/"))

    def test_more_downloads_than_counted_stays_in_half(self):
        state = {}
        update.progress("Packages (1) a-1", state)
        for _ in range(3):
            step = update.progress(" a-1-aarch64 downloading...", state)
        self.assertEqual(step, ("Downloading updates", 50))


class DownloadFailed(unittest.TestCase):
    def test_retryable(self):
        self.assertTrue(update.download_failed("error: failed to commit transaction (download library error)"))
        self.assertTrue(update.download_failed("error: failed retrieving file 'curl-8.23.0-2-aarch64.pkg.tar.xz'"))
        self.assertTrue(update.download_failed("error: failed to synchronize all databases"))

    def test_not_retryable(self):
        self.assertFalse(update.download_failed(None))
        self.assertFalse(update.download_failed("error: failed to commit transaction (conflicting files)"))
        self.assertFalse(update.download_failed("error: unable to lock database"))


if __name__ == "__main__":
    unittest.main()
