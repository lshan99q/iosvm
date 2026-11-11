#!/usr/bin/env python3
"""Put names on the user-space addresses the kernel's bring-up aids print.

usage: symstack.py LOG [PROCESS]

Reads the "ustacks:" (boot-args ustacks=NAME ustacks_at=SECONDS [ustacks_ms=PERIOD]), "usertrap:" and
"userabort:" lines of a console log (and, before a "userabort:", the "First throw call stack" of the exception
that led to it), subtracts the shared cache slide and looks every address up in the Darwin dyld
shared cache with "ipsw dyld a2s". Lookups take a few seconds each, so the answers are kept
in sysimage/a2s.json. Addresses outside the cache (the program itself) are shown as they are.
"""
import json
import os
import re
import subprocess
import sys

BASE = os.environ.get('IOSVM', '/host/iosvm-work/hakios-xnu-kernel') + '/sysimage'
DEFAULT_CACHE = '/host/iosvm/build/images/rootfs/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64'
CACHE = os.environ.get('SYMSTACK_CACHE') or (DEFAULT_CACHE if os.path.exists(DEFAULT_CACHE + '.symbols') else os.path.join(BASE, '20F66__iPhone10,3_6/dyld_shared_cache_arm64'))
# ipsw wants a ".symbols" beside the cache; the ones under build/images/rootfs have it, and
# SYMSTACK_CACHE lets this point there instead of copying gigabytes into sysimage/
KNOWN = os.environ.get('SYMSTACK_KNOWN') or os.path.join(BASE, 'a2s.json')
CACHE_LOW, CACHE_HIGH = 0x180000000, 0x280000000        # unslid addresses of the shared cache

known = json.load(open(KNOWN)) if os.path.exists(KNOWN) else {}


def name(address, slide):
    unslid = address - slide
    if not CACHE_LOW <= unslid < CACHE_HIGH:
        return f'{address:#x} (not in the shared cache)'
    key = f'{unslid:#x}'
    if key not in known or known.get(key) == '?':
        tool = os.environ.get('IPSW', 'ipsw')
        try:
            out = subprocess.run([tool, 'dyld', 'a2s', CACHE, key], capture_output=True, text=True).stdout
        except FileNotFoundError:
            return f'{address:#x} (ipsw not found; set IPSW=)'
        lines = [line for line in out.splitlines() if line and not line.startswith('   •')]
        known[key] = re.sub(r'^\S+:\s*', '', lines[-1]).strip() if lines else '?'
        json.dump(known, open(KNOWN, 'w'), indent=0)
    return known[key]


def main():
    log = sys.argv[1]
    only = sys.argv[2] if len(sys.argv) > 2 else None
    slide, show = 0, True
    thrown = None                       # the addresses after "*** First throw call stack:", until the abort
    for line in open(log, errors='replace'):
        line = line.rstrip('\n')
        if re.fullmatch(r'\((0x[0-9a-f]+ ?)+\)', line):
            thrown = line[1:-1].split()
            continue
        m = re.search(r'userabort: (\S+)\[(\d+)\] aborts, shared cache slide (0x[0-9a-f]+); called from: (.*)', line)
        if m:
            if only is None or m.group(1) == only:
                print(f'userabort: {m.group(1)}[{m.group(2)}]')
                for text in m.group(4).split():
                    print(f'    {name(int(text, 16), int(m.group(3), 16))}')
                if thrown:
                    print('  the exception was thrown from')
                    for text in thrown:
                        print(f'    {name(int(text, 16), int(m.group(3), 16))}')
            thrown = None
            continue
        m = re.search(r'(ustacks|usertrap): (\S+)\[(\d+)\].*shared cache slide (0x[0-9a-f]+)', line)
        if m:
            slide = int(m.group(4), 16)
            show = only is None or m.group(2) == only
            if show:
                print(line[line.index(m.group(1)):])
            continue
        if not show:
            continue
        m = re.search(r'ustacks:  thread (0x[0-9a-f]+)([^:]*): (.*)', line)
        if m:
            print(f'  thread {m.group(1)}{m.group(2)}')
            for depth, text in enumerate(m.group(3).split()):
                if depth == 1:
                    continue            # the link register: usually stale, the frame chain has the caller
                print(f'    {name(int(text, 16), slide)}')
            continue
        m = re.search(r'usertrap:   called from (0x[0-9a-f]+)', line)
        if m:
            print(f'    {name(int(m.group(1), 16), slide)}')
        elif 'usertrap:   x' in line:
            print('  ' + line[line.index('usertrap:') + 10:])


if __name__ == '__main__':
    main()
