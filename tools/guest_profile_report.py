#!/usr/bin/env python3
"""Aggregate an LP32_PROFILE_GUEST sample file against the game image's symbols.

usage: guest_profile_report.py <profile.txt> <game.image> [top]
"""
import bisect, collections, subprocess, sys

profile, image = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
syms = []
for line in subprocess.run(['nm', '-n', image], capture_output=True, text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) >= 3 and parts[1] in 'Tt':
        syms.append((int(parts[0], 16), parts[2]))
syms.sort()
keys = [s[0] for s in syms]
demangled = {}
def name_of(addr):
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return '<below image>'
    base, name = syms[i]
    if addr - base > 0x100000:
        return '<far from %s>' % name
    return name
guest = collections.Counter(); host = collections.Counter(); images = collections.Counter(); callers = collections.defaultdict(collections.Counter); total = 0; nguest = 0; nhost = 0; pads = 0; unknown_guest = 0
for line in open(profile):
    parts = line.split(maxsplit=2)
    if len(parts) < 2:
        continue
    total += 1
    addr = int(parts[1], 16)
    if parts[0] == 'g':
        nguest += 1
        chain = []
        for token in line.split()[2:]:
            try:
                chain.append(int(token, 16))
            except ValueError:
                break  # the trailing @timestamp, or a watch-mode arg0=/arg1=
        if chain:
            fn = name_of(addr) if 0x1000 <= addr < keys[-1] + 0x200000 else '?'
            callers[fn][' <- '.join(name_of(c) for c in chain[:3])] += 1
        if addr >= 0x7f000000:
            pads += 1; guest['<bridge pad 0x7fxxxxxx>'] += 1
        elif addr < 0x1000 or addr >= keys[-1] + 0x200000:
            unknown_guest += 1; guest['<guest outside image 0x%08x>' % (addr & ~0xfff)] += 1
        else:
            guest[name_of(addr)] += 1
    else:
        nhost += 1
        sym = parts[2].strip() if len(parts) > 2 else '?'
        sym = sym.split('+0x')[0]
        host[sym] += 1
        images[sym.split('!')[0] if '!' in sym else '<unknown image (Rosetta runtime / JIT)>'] += 1
if total == 0:
    sys.exit('no samples')
print('samples=%d guest=%d (%.1f%%) host=%d (%.1f%%) bridge-pads=%d' % (total, nguest, 100.0 * nguest / total, nhost, 100.0 * nhost / total, pads))
def demangle(names):
    try:
        out = subprocess.run(['c++filt'] + [n.lstrip('_') if n.startswith('__Z') else n for n in names], capture_output=True, text=True).stdout.splitlines()
        return dict(zip(names, out))
    except Exception:
        return {}
gnames = [n for n, _ in guest.most_common(top)]
dm = demangle(gnames)
print('\n--- top guest functions (%% of all samples) ---')
for n, c in guest.most_common(top):
    d = dm.get(n, n)
    if len(d) > 110: d = d[:107] + '...'
    print('%6.2f%%  %7d  %s' % (100.0 * c / total, c, d))
print('\n--- callers of the top guest functions ---')
for n, c in guest.most_common(6):
    if n not in callers: continue
    print('%s (%d samples):' % (dm.get(n, n)[:90], c))
    cn = [k for k, _ in callers[n].most_common(4)]
    cdm = demangle([x for k in cn for x in k.split(' <- ')])
    for k, kc in callers[n].most_common(4):
        print('   %6.1f%%  %s' % (100.0 * kc / c, ' <- '.join(cdm.get(x, x)[:60] for x in k.split(' <- '))))
print('\n--- host samples by image ---')
for n, c in images.most_common(12):
    print('%6.2f%%  %7d  %s' % (100.0 * c / total, c, n))
print('\n--- top host symbols (%% of all samples) ---')
for n, c in host.most_common(25):
    print('%6.2f%%  %7d  %s' % (100.0 * c / total, c, n))
