# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded 1.09 game render pools, adapted from Awakenchan bf594531.

These are game scratch pools, independent of Vulkan RecordChunks. Only the
prepared image receives writes; original game files and multiplayer are untouched.
"""
import hashlib
import os
import struct
from engine_assets import EBOOT_SHA

SITES = (0x2195c09, 0x2195c46)
ORIGINAL = bytes.fromhex('ba00000400')
POOL_SIZE = 1024 * 1024

def original_bytes(elf, address, size):
    try:
        phoff, = struct.unpack_from('<Q', elf, 0x20)
        stride, count = struct.unpack_from('<HH', elf, 0x36)
        if stride < 56 or count > 1024 or phoff + count * stride > len(elf): return None
        for index in range(count):
            kind, flags, offset, vaddr, _, filesz, _, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + index * stride)
            if kind == 1 and flags & 1 and vaddr <= address and address + size <= vaddr + filesz:
                start = offset + address - vaddr
                return elf[start:start+size] if start + size <= len(elf) else None
    except (struct.error, OverflowError):
        pass
    return None

def render_pool_writes(elf, existing=(), env=os.environ):
    if env.get('BB_RENDER_POOLS') == '0': return []
    if hashlib.sha256(elf).hexdigest() not in EBOOT_SHA:
        print('Render pools: skipped, unverified executable', flush=True)
        return []
    if any(original_bytes(elf, site, len(ORIGINAL)) != ORIGINAL for site in SITES):
        print('Render pools: skipped, original instructions differ', flush=True)
        return []
    # Respect any XML/mod write touching either instruction, even a split write.
    if any(offset < site + len(ORIGINAL) and site < offset + len(data)
           for offset, data in existing for site in SITES):
        print('Render pools: skipped, external patch owns an instruction', flush=True)
        return []
    print('Render pools: two byte-verified 1 MiB game scratch pools', flush=True)
    return [(site + 1, struct.pack('<I', POOL_SIZE)) for site in SITES]
