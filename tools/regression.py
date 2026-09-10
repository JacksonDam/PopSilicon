#!/usr/bin/env python3
"""Boot every built game at once and report whether each one still starts.

Serially this takes as long as the titles cost put together, which is why it
kept getting run with a timeout too short to see anything.  The games are
independent processes, so they are started together and watched together; the
whole sweep costs one settle period rather than seven.

What it can and cannot tell you: a title that traps an import, dies, or stops
producing output early is caught here.  A title that draws a modal error into
its own window is *not* -- that yields zero traps and a live process -- so a
run that looks clean still has to be confirmed by eye.  The startup log size
is the cheap proxy for "got as far as it used to": it is stable per title, and
the Bejeweled 3 properties failure showed up as 1655 bytes against a healthy
8100.  Baselines live in regression-baseline.json next to this script.

usage: regression.py [--seconds N] [--update-baseline] [--only NAME ...]
"""

import argparse
import json
import pathlib
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parent.parent
baseline_path = pathlib.Path(__file__).resolve().parent / 'regression-baseline.json'

parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=float, default=20.0,
                    help='how long to let the games run before sampling')
parser.add_argument('--update-baseline', action='store_true',
                    help='record this run as the reference instead of checking it')
parser.add_argument('--only', nargs='+', metavar='NAME',
                    help='limit the sweep to these bundle names')
parser.add_argument('--env', nargs='+', default=[], metavar='K=V',
                    help='extra environment for every game')
arguments = parser.parse_args()

bundles = sorted(b for b in (root / 'build').glob('*.app')
                 if (b / 'Contents/MacOS/PeggleSilicon').is_file())
if arguments.only:
    wanted = {name.lower() for name in arguments.only}
    bundles = [b for b in bundles if b.stem.lower() in wanted]
if not bundles:
    sys.exit('no built games in build/ -- run tools/build.py first')

logs = root / 'build' / 'regression-logs'
logs.mkdir(exist_ok=True)
environment = dict(**{k: v for k, v in
                      (pair.split('=', 1) for pair in arguments.env)})

runs = []
for bundle in bundles:
    handle = (logs / (bundle.stem + '.log')).open('wb')
    process = subprocess.Popen(
        ['arch', '-x86_64', str(bundle / 'Contents/MacOS/PeggleSilicon')],
        stdout=handle, stderr=subprocess.STDOUT, cwd=root,
        env={**__import__('os').environ, **environment})
    runs.append((bundle.stem, process, handle))
print('started %d games; settling for %.0fs' % (len(runs), arguments.seconds))
time.sleep(arguments.seconds)

# Sample every process before killing any, so the last title is not measured
# after the others have already released the GPU.
sampled = []
for name, process, handle in runs:
    alive = process.poll() is None
    cpu = ''
    if alive:
        cpu = subprocess.run(['ps', '-o', '%cpu=', '-p', str(process.pid)],
                             capture_output=True, text=True).stdout.strip()
    sampled.append((name, process, handle, alive, cpu))

for _, process, _, alive, _ in sampled:
    if alive:
        process.terminate()
for _, process, handle, _, _ in sampled:
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
    handle.close()

baseline = {}
if baseline_path.exists():
    baseline = json.loads(baseline_path.read_text())

print()
print('%-18s %-6s %-8s %-7s %-9s %s' %
      ('title', 'alive', 'cpu', 'traps', 'bytes', 'vs baseline'))
failures, recorded = [], {}
for name, _, _, alive, cpu in sampled:
    text = (logs / (name + '.log')).read_bytes()
    traps = text.count(b'trapped import')
    size = len(text)
    recorded[name] = size
    reference = baseline.get(name)
    if reference is None:
        verdict = 'no baseline'
    else:
        # Startup output is deterministic enough that a real stall shows up as
        # a fraction of the expected size, while ordinary jitter is a few
        # percent.  Only a large shortfall is worth failing on.
        ratio = size / reference if reference else 1.0
        verdict = ('%+.0f%%' % ((ratio - 1) * 100)) if ratio >= 0.6 else \
                  'SHORT (%.0f%% of %d)' % (ratio * 100, reference)
    if not alive or traps or verdict.startswith('SHORT'):
        failures.append(name)
    print('%-18s %-6s %-8s %-7d %-9d %s' %
          (name, 'yes' if alive else 'NO', cpu or '-', traps, size, verdict))

if arguments.update_baseline:
    baseline_path.write_text(json.dumps(recorded, indent=2, sort_keys=True) + '\n')
    print('\nbaseline written to %s' % baseline_path.name)
    sys.exit(0)

print('\nlogs in %s' % logs)
if failures:
    print('FAILED: %s' % ', '.join(failures))
    sys.exit(1)
print('all %d titles started' % len(sampled))
print('note: a modal error drawn by the game itself looks clean here -- '
      'confirm by eye before shipping')
