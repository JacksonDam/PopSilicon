#!/usr/bin/env python3
import subprocess,pathlib,os,sys
root=pathlib.Path(__file__).resolve().parent.parent
log=root/'diagnostics/run.log'
with log.open('w') as f:
 p=subprocess.Popen([str(root/'build/PeggleSilicon.app/Contents/MacOS/PeggleSilicon')],stdout=f,stderr=subprocess.STDOUT,env=os.environ)
 try:p.wait(timeout=int(sys.argv[1]) if len(sys.argv)>1 else 12)
 except subprocess.TimeoutExpired:p.terminate();p.wait();print('Probe time limit reached')
print('\n'.join(log.read_text(errors='replace').splitlines()[-22:]))
