# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows leaf-store protection, adapted from d4rksp4rt4n/Supermedo PR #2.

Only byte-verified 1.09 sites are redirected. No guest registers, flags,
addresses or native session state are fabricated. The original ELF is retained.
"""
import bisect
import hashlib
import json
import os
from pathlib import Path
import struct

SITES = Path(__file__).resolve().parent.parent / 'patches/redzone.json'
LEA_DOWN = bytes.fromhex('488d642480')
LEA_UP = bytes.fromhex('488da42480000000')

def enabled(env=os.environ):
    value=env.get('BB_RED_ZONE')
    if value is not None and value not in ('0','1'):
        raise ValueError('BB_RED_ZONE requires 0 or 1')
    return value=='1' if value is not None else os.name=='nt'

def validate_sites(image, sites, base):
    if not isinstance(base,int) or base<len(image) or base>512*1024*1024 or len(sites)>4096:
        raise ValueError('Invalid red-zone trampoline range')
    end=0
    for offset, original, lengths in sites:
        data=bytes.fromhex(original)
        if (type(offset) is not int or offset<end or len(data)<5 or len(data)>4096 or
            offset+len(data)>len(image) or not lengths or
            any(type(n)is not int or n==0 or abs(n)>15 for n in lengths) or
            sum(abs(n) for n in lengths)!=len(data)):
            raise ValueError('Invalid/overlapping red-zone site')
        end=offset+len(data)

def protect_red_zones(image, sites, base, skip=()):
    """Run guest stores of red-zone leaf functions with rsp 128 bytes lower (Windows).

    Windows writes an exception's frame right below rsp, over the System V red zone where leaf
    functions keep live locals (PS4 and Linux leave those 128 bytes alone). A store that faults
    on a page the GPU tracker write-protected is resumed with the locals overwritten: the
    attack-trail writer at 0x28ce7b0 then loads NULL from [rsp-0x60] (D018/D028/D087). Each site
    (tools/redzone_sites.py: offset, original bytes, instruction lengths, negative = store)
    jumps to a copy at `base` where the stores run below the red zone. Sites whose bytes
    differ or that overlap a relocation (`skip`: sorted targets) stay as they are. Returns
    the copies and the number of sites redirected. From Supermedo/bloodborne_pc PR #2."""
    validate_sites(image, sites, base)
    code = bytearray()
    applied = 0
    for offset, original, lengths in sites:
        original = bytes.fromhex(original)
        end = offset + len(original)
        near = bisect.bisect_right(skip, offset - 8)
        if image[offset:end] != original or (near < len(skip) and skip[near] < end):
            continue
        here = base + len(code)
        pos = offset
        for length in lengths:
            body = image[pos:pos + abs(length)]
            code += LEA_DOWN + body + LEA_UP if length < 0 else body
            pos += abs(length)
        code += b'\xe9' + struct.pack('<i', end - (base + len(code) + 5))
        code += b'\xcc' * (-len(code) % 16)
        image[offset:end] = b'\xe9' + struct.pack('<i', here - (offset + 5)) + b'\xcc' * (len(original) - 5)
        applied += 1
    return bytes(code), applied


def protect_image(image, segments, relocs):
    report=dict(enabled=enabled(),sites=0,available=0,trampoline_bytes=0)
    if not report['enabled']:return report
    raw=SITES.read_bytes()
    if len(raw)>1024*1024:raise ValueError('Red-zone table too large')
    sites=json.loads(raw)['sites']
    base=(len(image)+65535)&~65535
    code,applied=protect_red_zones(image,sites,base,sorted(r[0] for r in relocs))
    if code:
        if base+len(code)>512*1024*1024:raise ValueError('Red-zone image exceeds loader limit')
        image.extend(bytes(base-len(image))+code)
        segments.append((base,len(code),5))
    report.update(sites=applied,available=len(sites),table_sha256=hashlib.sha256(raw).hexdigest(),trampoline_bytes=len(code))
    print(f'Red zone: {applied}/{len(sites)} byte-verified leaf stores protected',flush=True)
    return report


def patch_conflicts(writes, linked):
    """Let XML patches own an overlapping site: undo only our verified trampoline first.

    The reusable base has already been linked, whereas XML selection changes on
    every launch. Never leave half of a custom patch applied over an E9 detour.
    Check the actual image and trampoline (not just an optional environment flag),
    so this is safe with both cache hits and disabled/unprotected images.
    """
    if not writes or not Path(linked).is_file():
        return writes
    sites=json.loads(SITES.read_text())['sites']
    restored=[]
    with Path(linked).open('rb') as stream:
        head=stream.read(16)
        if len(head)!=16 or head[:8]!=b'BBPROBE5':return writes
        size,=struct.unpack_from('<Q',head,8)
        length=stream.seek(0,2)
        start=length-size
        if size>512*1024*1024 or start<16:raise ValueError('Invalid linked image size')
        for offset,original,lengths in sites:
            data=bytes.fromhex(original)
            end=offset+len(data)
            if not any(p<end and offset<p+len(b) for p,b in writes):continue
            if end>size:raise ValueError('Red-zone site outside linked image')
            stream.seek(start+offset)
            detour=stream.read(len(data))
            if detour[:1]!=b'\xe9' or detour[5:]!=b'\xcc'*(len(data)-5):continue
            dest=offset+5+struct.unpack_from('<i',detour,1)[0]
            body=bytearray();pos=0
            for n in lengths:
                instruction=data[pos:pos+abs(n)];pos+=abs(n)
                body+=LEA_DOWN+instruction+LEA_UP if n<0 else instruction
            body+=b'\xe9'+struct.pack('<i',end-(dest+len(body)+5))
            if dest<0 or dest+len(body)>size:raise ValueError('Invalid red-zone trampoline')
            stream.seek(start+dest)
            if stream.read(len(body))!=body:raise ValueError('Unexpected red-zone trampoline')
            restored.append((offset,data))
    if restored:print(f'Red zone: {len(restored)} sites left to selected XML patches',flush=True)
    return restored+list(writes)
