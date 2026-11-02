"""
Self-check for MOVSX from a register: every 8- and 16-bit source sign-extends.

Run: py -3 tools/recomp/test_movsx_regs.py

Regression guard for `movsx r32, bp` (and `sp`) lifting as a plain 16-bit read,
i.e. a zero-extension: the 16-bit list in _lift_movsx had ax..di but not bp or
sp. Bink's 8x8 inverse DCT in Fallout: Brotherhood of Steel (0x000CD7BC,
`movsx ebp, bp` on the row-6 coefficient) then saw every negative coefficient
there as +65536-ish, and the decoded video showed isolated white and coloured
blocks wherever such a coefficient occurred.

The lifted statement is evaluated here, not pattern-matched: the C macros it
uses are mapped to Python and the destination's value checked for each source.
"""

import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import Lifter  # noqa: E402


class _Op:
    def __init__(self, text, type="reg", reg=None, size=4, mem_size=None, imm=0):
        self.text = text
        self.type = type
        self.reg = reg
        self.size = size
        self.mem_size = mem_size
        self.imm = imm


class _Insn:
    def __init__(self, mnemonic, operands):
        self.mnemonic = mnemonic
        self.operands = operands


def _lift(mnemonic, ops):
    return "\n".join(Lifter().lift_instruction(_Insn(mnemonic, ops)))


# The runtime's register-part and extension macros (recomp_types.h), in Python.
def LO8(v): return v & 0xFF
def HI8(v): return (v >> 8) & 0xFF
def LO16(v): return v & 0xFFFF
def SX8(v): v &= 0xFF; return (v - 0x100 if v & 0x80 else v) & 0xFFFFFFFF
def SX16(v): v &= 0xFFFF; return (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF
def ZX8(v): return v & 0xFF
def ZX16(v): return v & 0xFFFF


def _run(stmt, regs):
    """Execute one lifted `dst = expr;` statement over `regs`."""
    m = re.match(r"\s*(\w+)\s*=\s*(.*);", stmt)
    assert m, stmt
    env = dict(regs, LO8=LO8, HI8=HI8, LO16=LO16, SX8=SX8, SX16=SX16, ZX8=ZX8, ZX16=ZX16)
    expr = m.group(2).replace("(uint32_t)", "").replace("(int32_t)", "")
    return m.group(1), eval(expr, {}, env) & 0xFFFFFFFF  # noqa: S307 -- our own output


# 32-bit register whose low 16 bits (or low/high 8) each source names.
_SRC16 = {"ax": "eax", "bx": "ebx", "cx": "ecx", "dx": "edx",
          "si": "esi", "di": "edi", "bp": "ebp", "sp": "esp"}
_SRC8 = {"al": "eax", "bl": "ebx", "cl": "ecx", "dl": "edx",
         "ah": "eax", "bh": "ebx", "ch": "ecx", "dh": "edx"}


def test_movsx_16bit_sources_sign_extend():
    for src, full in _SRC16.items():
        out = _lift("movsx", [_Op("edi" if full != "edi" else "eax",
                                  reg="edi" if full != "edi" else "eax"),
                              _Op(src, reg=src, size=2)])
        regs = {r: 0 for r in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp")}
        regs[full] = 0x1234FFF0                      # low half -16
        dst, actual = _run(out, regs)
        expected = 0xFFFFFFF0
        assert actual == expected, f"movsx {dst}, {src}: {out!r} -> {actual:#x}"


def test_movsx_same_register_bp():
    # The exact instruction from Bink's IDCT.
    out = _lift("movsx", [_Op("ebp", reg="ebp"), _Op("bp", reg="bp", size=2)])
    dst, actual = _run(out, {"ebp": 0x0012FF9C})     # bp = -100
    expected = (-100) & 0xFFFFFFFF
    assert (dst, actual) == ("ebp", expected), f"{out!r} -> {actual:#x}"


def test_movsx_8bit_sources_sign_extend():
    for src, full in _SRC8.items():
        out = _lift("movsx", [_Op("esi", reg="esi"), _Op(src, reg=src, size=1)])
        regs = {r: 0 for r in ("eax", "ebx", "ecx", "edx", "esi")}
        regs[full] = 0x0000F0F0                      # both bytes -16
        _, actual = _run(out, regs)
        expected = 0xFFFFFFF0
        assert actual == expected, f"movsx esi, {src}: {out!r} -> {actual:#x}"


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"  ok   {name}")
        except AssertionError as exc:
            failures += 1
            print(f"  FAIL {name}\n{exc}")
    print("movsx regs: " + ("OK" if not failures else f"{failures} FAILED"))
    sys.exit(1 if failures else 0)
