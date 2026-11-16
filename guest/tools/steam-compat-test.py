#!/usr/bin/env python3
"""Tests localdesktop-steam-compat on any PC with Python 3:

    python3 guest/tools/steam-compat-test.py

It writes a Steam folder with a synthetic appinfo.vdf (format 29) and config.vdf, runs the tool's
mapping pass on it, and checks what config.vdf maps afterwards.
"""
import importlib.machinery
import importlib.util
import json
import os
import struct
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True  # no __pycache__ in guest/managed, which ships as is

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_PATH = os.path.join(HERE, "../managed/usr/local/bin/localdesktop-steam-compat")
loader = importlib.machinery.SourceFileLoader("steam_compat", TOOL_PATH)
spec = importlib.util.spec_from_loader("steam_compat", loader)
compat = importlib.util.module_from_spec(spec)
loader.exec_module(compat)

LINUX_X86 = "431240"    # Linux and Windows, x86 only
LINUX_ONLY = "107100"   # Linux only
ARM64 = "570"           # has an arm64 Linux build
WINDOWS = "620"         # Windows only
TOOL_APP = "1628350"    # a runtime (type "tool")
PICKED = "4000"         # Linux x86, the user picked Proton for it


def appinfo(apps):
    """An appinfo.vdf, format 29, with {app id: common section} entries."""
    strings = []

    def key(name):
        if name not in strings:
            strings.append(name)
        return struct.pack("<I", strings.index(name))

    def kv(value):
        out = b""
        for name, item in value.items():
            if isinstance(item, dict):
                out += b"\x00" + key(name) + kv(item)
            else:
                out += b"\x01" + key(name) + item.encode() + b"\0"
        return out + b"\x08"

    body = b""
    for app, common in apps.items():
        data = b"\0" * 60 + kv({"appinfo": {"appid": app, "common": common}})
        body += struct.pack("<II", int(app), len(data)) + data
    body += struct.pack("<I", 0)
    table = 16 + len(body)
    strings_out = struct.pack("<I", len(strings)) + b"".join(s.encode() + b"\0" for s in strings)
    return struct.pack("<IIq", 0x07564429, 1, table) + body + strings_out


CONFIG = """"InstallConfigStore"
{
	"Software"
	{
		"valve"
		{
			"Steam"
			{
				"AutoUpdateWindowEnabled"		"0"
				"CompatToolMapping"
				{
					"0"
					{
						"name"		"proton_experimental"
						"config"		""
						"priority"		"75"
					}
					"%s"
					{
						"name"		"proton_9"
						"config"		""
						"priority"		"250"
					}
				}
			}
		}
	}
}
""" % PICKED


class SteamCompat(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.steam = os.path.join(self.dir.name, "Steam")
        self.state = os.path.join(self.dir.name, "mapped.json")
        os.makedirs(os.path.join(self.steam, "config"))
        os.makedirs(os.path.join(self.steam, "appcache"))
        self.write_appinfo({
            LINUX_X86: {"type": "Game", "oslist": "windows,linux", "osarch": "64"},
            LINUX_ONLY: {"type": "game", "oslist": "linux"},
            ARM64: {"type": "game", "oslist": "windows,linux", "osarch": "arm64"},
            WINDOWS: {"type": "game", "oslist": "windows"},
            TOOL_APP: {"type": "Tool", "oslist": "linux"},
            PICKED: {"type": "game", "oslist": "linux"},
        })
        self.write_config(CONFIG)

    def tearDown(self):
        self.dir.cleanup()

    def write_appinfo(self, apps):
        with open(os.path.join(self.steam, "appcache", "appinfo.vdf"), "wb") as f:
            f.write(appinfo(apps))

    def write_config(self, text):
        with open(os.path.join(self.steam, "config", "config.vdf"), "w") as f:
            f.write(text)

    def mapping(self):
        with open(os.path.join(self.steam, "config", "config.vdf")) as f:
            tokens = compat.tokenize(f.read())
        self.assertTrue(compat.balanced(tokens))
        block = compat.mapping_block(tokens)
        return {app: name for app, (_, name) in compat.entries(tokens, block).items()}

    def run_tool(self):
        return compat.run(self.steam, self.state)

    def test_reads_appinfo(self):
        info = compat.read_appinfo(os.path.join(self.steam, "appcache", "appinfo.vdf"))
        self.assertEqual(info[LINUX_X86], (frozenset(("windows", "linux")), "64", "game"))
        self.assertEqual(info[ARM64][1], "arm64")
        self.assertEqual(info[TOOL_APP][2], "tool")

    def test_maps_x86_linux_games_only(self):
        self.run_tool()
        mapping = self.mapping()
        self.assertEqual(mapping[LINUX_X86], compat.TOOL)
        self.assertEqual(mapping[LINUX_ONLY], compat.TOOL)
        for app in (ARM64, WINDOWS, TOOL_APP):
            self.assertNotIn(app, mapping)
        self.assertEqual(mapping["0"], "proton_experimental")  # the default stays

    def test_keeps_the_users_pick(self):
        self.run_tool()
        self.assertEqual(self.mapping()[PICKED], "proton_9")

    def test_new_entries_have_per_app_priority(self):
        self.run_tool()
        with open(os.path.join(self.steam, "config", "config.vdf")) as f:
            tokens = compat.tokenize(f.read())
        block = compat.mapping_block(tokens)
        start = compat.entries(tokens, block)[LINUX_X86][0]
        body = tokens[start + 1:compat.block_end(tokens, start)]
        self.assertEqual(dict(zip(body[::2], body[1::2]))['"priority"'], '"250"')

    def test_second_run_changes_nothing(self):
        self.run_tool()
        with open(os.path.join(self.steam, "config", "config.vdf")) as f:
            first = f.read()
        self.assertIn("mapped 0", self.run_tool())
        with open(os.path.join(self.steam, "config", "config.vdf")) as f:
            self.assertEqual(f.read(), first)

    def test_a_game_the_user_unmapped_stays_unmapped(self):
        self.run_tool()
        with open(os.path.join(self.steam, "config", "config.vdf")) as f:
            tokens = compat.tokenize(f.read())
        compat.apply(tokens, compat.mapping_block(tokens), {LINUX_ONLY: None})  # as the client would
        self.write_config(compat.serialize(tokens))
        self.run_tool()
        self.assertNotIn(LINUX_ONLY, self.mapping())
        self.assertEqual(self.mapping()[LINUX_X86], compat.TOOL)

    def test_a_game_that_gets_an_arm64_build_is_unmapped(self):
        self.run_tool()
        self.write_appinfo({LINUX_X86: {"type": "game", "oslist": "windows,linux", "osarch": "arm64"},
                            LINUX_ONLY: {"type": "game", "oslist": "linux"}})
        self.run_tool()
        self.assertNotIn(LINUX_X86, self.mapping())
        with open(self.state) as f:
            self.assertEqual(json.load(f), [LINUX_ONLY])

    def test_a_game_mapped_by_hand_is_remembered(self):
        text = CONFIG.replace('"%s"' % PICKED, '"%s"' % LINUX_X86).replace("proton_9", compat.TOOL)
        self.write_config(text)
        self.run_tool()
        with open(self.state) as f:
            self.assertIn(LINUX_X86, json.load(f))

    def test_adds_the_mapping_section_when_missing(self):
        self.write_config('"InstallConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n'
                          '\t\t\t{\n\t\t\t\t"cip"\t\t"x"\n\t\t\t}\n\t\t}\n\t}\n}\n')
        self.run_tool()
        self.assertEqual(self.mapping()[LINUX_ONLY], compat.TOOL)

    def test_leaves_a_broken_config_alone(self):
        self.write_config(CONFIG[:-3])
        self.assertIn("left alone", self.run_tool())
        with open(os.path.join(self.steam, "config", "config.vdf")) as f:
            self.assertEqual(f.read(), CONFIG[:-3])

    def test_no_config_yet(self):
        os.remove(os.path.join(self.steam, "config", "config.vdf"))
        self.assertEqual(self.run_tool(), "no config.vdf yet")


if __name__ == "__main__":
    unittest.main()
