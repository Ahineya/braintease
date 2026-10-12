#!/usr/bin/env python3
"""Extract Doom's actual key state and ticcmd builder for a native/Ripple test.

Usage: extract_input_fixture.py prepared/g_game.c generated
The fixture links the port configuration and uses the upstream control defaults;
HUD/menu/chat callbacks are stubbed so only input-to-command behavior is tested.
"""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--platform", type=Path,
                    default=Path(__file__).resolve().parents[1] / "src/platform.c")
args = parser.parse_args()
source = args.source.read_text()
prefix = source[:source.index("int G_CmdChecksum")]
builder = source[source.index("void G_BuildTiccmd"):source.index("void G_DoLoadLevel (void)")]
responder = source[source.index("static void SetJoyButtons"):source.index("void G_Ticker (void)")]
controls = (args.source.parent / "m_controls.c").read_text()
args.output.mkdir(parents=True, exist_ok=True)
(args.output / "input_commands.inc").write_text(
    prefix + "\nstatic int G_NextWeapon(int direction);\n" + builder + responder)
(args.output / "controls_defaults.inc").write_text(
    controls[:controls.index("void M_BindBaseControls(void)")])
platform = args.platform.read_text()
held = platform[platform.index("static unsigned char held["):]
held = held[:held.index(";") + 1]
keyboard = platform[platform.index("static int doom_key("):platform.index("void I_StartTic(void)")]
(args.output / "platform_keyboard.inc").write_text(held + "\n" + keyboard)
