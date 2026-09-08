#!/usr/bin/env python3
"""Find the stalls in an LP32_PROFILE_GUEST sample file.

A whole session's aggregate hides a brief freeze: a 200 ms hitch inside three
minutes of play is a tenth of a percent of the samples.  The sampler stamps
every sample with the time it was taken (CLOCK_UPTIME_RAW nanoseconds, the same
clock LP32_SLOW_IMPORT_MS prints), so the stalls can be found directly: the
profiled thread is the one running the game's frame loop, and a stall is a run
of consecutive samples that stays in one function instead of cycling through
the loop.

usage: guest_profile_stalls.py <profile> <game.image> [min-ms] [top]
       guest_profile_stalls.py <profile> <game.image> --window <from-s> <to-s>
"""

import bisect
import collections
import subprocess
import sys

profile, image = sys.argv[1], sys.argv[2]

syms = []
for line in subprocess.run(['nm', '-n', image], capture_output=True,
                           text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) >= 3 and parts[1] in 'Tt':
        syms.append((int(parts[0], 16), parts[2]))
syms.sort()
keys = [s[0] for s in syms]


def name_of(addr):
    if addr >= 0x7f000000:
        return '<bridge thunk>'
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0 or addr - syms[i][0] > 0x100000:
        return '<0x%08x>' % addr
    return syms[i][1]


def demangle(names):
    names = list(names)
    if not names:
        return {}
    stripped = [n.lstrip('_') if n.startswith('__Z') else n for n in names]
    out = subprocess.run(['c++filt'] + stripped, capture_output=True,
                         text=True).stdout.splitlines()
    return dict(zip(names, out))


samples = []   # (seconds, function, 'g'|'h', caller chain)
for line in open(profile, errors='replace'):
    parts = line.split()
    if len(parts) < 2 or parts[0] not in ('g', 'h') or not parts[-1].startswith('@'):
        continue
    when = int(parts[-1][1:]) / 1e9
    if parts[0] == 'g':
        addr = int(parts[1], 16)
        chain = []
        for token in parts[2:-1]:
            try:
                chain.append(int(token, 16))
            except ValueError:
                break
        samples.append((when, name_of(addr), 'g', chain))
    else:
        samples.append((when, parts[2].split('+0x')[0] if len(parts) > 2 else '?',
                        'h', []))
if not samples:
    sys.exit('no timestamped samples (was the profile written by a build that '
             'stamps them?)')
samples.sort()
start, end = samples[0][0], samples[-1][0]
print('%d samples over %.1f s (%.0f/s)' % (len(samples), end - start,
                                           len(samples) / max(end - start, 1e-9)))


def report(window, title):
    if not window:
        return
    counts = collections.Counter(s[1] for s in window)
    names = demangle(n for n, _ in counts.most_common(8))
    print('\n%s' % title)
    for name, hits in counts.most_common(8):
        print('   %5.1f%% %5d  %s' % (100.0 * hits / len(window), hits,
                                      names.get(name, name)))
    chains = collections.Counter(
        ' <- '.join(name_of(c) for c in s[3][:3]) for s in window if s[3])
    for chain, hits in chains.most_common(3):
        print('        via %s  (%d)' % (chain, hits))


if len(sys.argv) > 3 and sys.argv[3] == '--window':
    lo, hi = float(sys.argv[4]), float(sys.argv[5])
    report([s for s in samples if lo <= s[0] <= hi],
           'samples in %.3f-%.3f s' % (lo, hi))
    sys.exit(0)

minimum_ms = float(sys.argv[3]) if len(sys.argv) > 3 else 40.0
top = int(sys.argv[4]) if len(sys.argv) > 4 else 6

# A stall is a run of consecutive samples that never leaves one function.
runs = []
begin = 0
for index in range(1, len(samples) + 1):
    if index == len(samples) or samples[index][1] != samples[begin][1]:
        span = (samples[index - 1][0] - samples[begin][0]) * 1000.0
        if span >= minimum_ms:
            runs.append((span, begin, index))
        begin = index
runs.sort(reverse=True)
print('\nruns of >= %.0f ms spent inside a single function: %d' %
      (minimum_ms, len(runs)))
for span, begin, index in runs[:top]:
    name = samples[begin][1]
    print('\n  %8.1f ms  t=%.3f-%.3f  %s  (%d samples)' %
          (span, samples[begin][0], samples[index - 1][0],
           demangle([name]).get(name, name), index - begin))
    chains = collections.Counter(
        ' <- '.join(name_of(c) for c in s[3][:3]) for s in samples[begin:index] if s[3])
    for chain, hits in chains.most_common(2):
        print('        via %s  (%d)' % (chain, hits))
