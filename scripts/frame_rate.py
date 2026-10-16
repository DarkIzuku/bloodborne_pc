# SPDX-License-Identifier: GPL-3.0-or-later
"""High-FPS additions only: preserve the established 30/60/90 patch paths.

120 FPS values adapted from Awakenchan/BloodBorneButPc 4f93643e (Kyo's 90 FPS++).
Measured physics/AI steps from droogie/bbhost 566184a7, src/engine/frame_rate.cpp.
All addresses are preferred 1.09 VAs; only the prepared image is changed.
"""
import hashlib
import struct
from engine_assets import EBOOT_SHA
from render_pools import original_bytes

BASE = 0x400000
SLOT = 0x575A908  # Kyo's FPS++ cave writes the clamped measured frame time here.

# Original bytes, replacement bytes. No new allocation, thunk or guessed ABI.
STEPS = (
    (0x16A4416, 'c5fa58055e136803', 'c5fa5805ea640b04'),
    (0x1915FAB, 'c5fa1005b1034103', 'c5fa10055549e403'),
    (0x1916030, 'c5fa10052c034103', 'c5fa1005d048e403'),
    (0x1915F65, 'c5fa2ac9c5fa1005f3034103c5f259c8488b7b208b80e1010000a8040f94c00fb6c84c89f64c89fa4883c408',
     '488b7b20c5fa2ac9c5f2590def034103c5fa10058b49e40331c9f680e1010000040f94c14c89f64c89fa5890'),
    (0x222BCBF, '488d0d6a404c034883c11048898d40feffffc78548feffff8988083d',
     '488d0d7a404c0348898d40feffff8b0d35ec5203898d48feffff6690'),
    (0x1BF9B69, '48c745c000000000c745b88988083d', '8b05990db6038945b8660f1f440000'),
    (0x2713D8D, 'c5fa1025db606102', 'c5fa1025736b0403'),
)

def scaled_values(fps=120):
    f = lambda *values: b''.join(struct.pack('<f', v) for v in values)
    frame = bytes.fromhex('41c7442418') + f(1/fps)
    cap = struct.pack('<I', 8000)
    return {
        0xF383CA: f(1/fps),
        0x1CBDB42: bytes.fromhex('c7857cffffff') + f(0.0125*90/fps) + b'\x90'*6,
        0x1929676: struct.pack('<i', fps//6), 0x192C405: struct.pack('<i', fps//3),
        **{a: frame for a in (0x2434840,0x243485A,0x243487E,0x24348B3)},
        0x25629F0: b'\xbf' + struct.pack('<I', 990000//fps),
        0x2C012CC: b'\x81\xff'+cap+b'\x7e\x05\xbf'+cap+bytes.fromhex('e98aeb3b00'),
        0x4CF6420: f(*[3/fps]*4), 0x4D27FD4: f(-3/fps),
        0x4D27FE0: f(3/fps), 0x4D29170: f(1/fps),
    }

def replace_range(writes, va, replacement):
    """Replace a checked code span without losing adjacent bytes of split XML writes."""
    lo, hi = va - BASE, va - BASE + len(replacement)
    out = []
    for offset,data in writes:
        end = offset+len(data)
        if end <= lo or offset >= hi:
            out.append((offset,data)); continue
        if offset < lo: out.append((offset,data[:lo-offset]))
        if end > hi: out.append((hi,data[hi-offset:]))
    out.append((lo,replacement))
    return out

def adapt_high_frame_rate(writes, mode, elf):
    if mode not in ('120','uncap'): return writes
    if hashlib.sha256(elf).hexdigest() not in EBOOT_SHA:
        raise ValueError('High FPS requires the verified supported 1.09 executable')
    for address,old,new in STEPS:
        if len(bytes.fromhex(old)) != len(bytes.fromhex(new)) or original_bytes(elf,address-BASE,len(bytes.fromhex(old))) != bytes.fromhex(old):
            raise ValueError(f'High FPS: original timing instructions differ at {address:#x}')
    values = scaled_values()
    out, seen = [], set()
    for offset,data in writes:
        address=offset+BASE
        if address in values:
            if len(data)!=len(values[address]): raise ValueError(f'90 FPS++ layout changed at {address:#x}')
            seen.add(address); data=values[address]
        # Particle data is authored at 30 Hz, irrespective of presentation frequency.
        if address in (0x26FB39F,0x4CAD7FC): continue
        # Kyo's newer cloth model: stiffness squared, native damping/integrator.
        if any(lo<=address<hi for lo,hi in ((0xB6FBD4,0xB6FBD8),(0xB73F84,0xB73F8D),(0xB73FA8,0xB73FC1),(0x4D3BD10,0x4D3BD20))): continue
        if address==0x4D3BD00: data=struct.pack('<4f', *([0.25]*4))
        out.append((offset,data))
    if seen != values.keys(): raise ValueError(f'90 FPS++ is missing required timing writes: {values.keys()-seen}')
    for address,old,new in STEPS: out=replace_range(out,address,bytes.fromhex(new))
    if mode=='uncap':
        # Same frame-job/mode design as 90/120, with a zero minimum target.
        # Do not resurrect the separate legacy Uncap FPS++ model-job patch.
        for address in (0x2434840,0x243485A,0x243487E,0x24348B3):
            out=replace_range(out,address,bytes.fromhex('41c7442418')+struct.pack('<f',0))
        out=replace_range(out,0x25629F0,b'\xbf'+struct.pack('<I',0))
        cap=struct.pack('<I',1)
        out=replace_range(out,0x2C012CC,b'\x81\xff'+cap+b'\x7e\x05\xbf'+cap+bytes.fromhex('e98aeb3b00'))
        # The original two instructions load xmm0 and store the character dt.
        # Read FPS++'s measured dt, rather than advancing stamina/motion at a fixed 120.
        address=0x1CBDB42
        expected=bytes.fromhex('c5fa1083e0000000c5fa11857cffffff')
        if original_bytes(elf,address-BASE,len(expected))!=expected:
            raise ValueError('Uncapped character timing instructions differ')
        out=replace_range(out,address,bytes.fromhex('c5fa1005')+struct.pack('<i',SLOT-(address+8))+bytes.fromhex('c5fa11857cffffff'))
    print(f'Frame rate: {mode}; checked 1.09 physics/AI steps, FPS++ frame jobs retained',flush=True)
    return out
