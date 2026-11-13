#!/usr/bin/env python3
"""Build and run the four-zone ROI hardware experiment, then generate its report."""
import argparse
import datetime
import json
import pathlib
import signal
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=pathlib.Path, default=ROOT/'experiments'/('roi-switch-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')))
    p.add_argument('--cycles', type=int, default=8)
    p.add_argument('--write-node', choices=['OffsetY','OffsetYReg','port'], default='OffsetY')
    p.add_argument('--pipeline2', action='store_true', help='Long check of switching every 2 frames: 4096 frames per mode')
    p.add_argument('--pipeline24', action='store_true', help='Long check of switching every 2 and 4 frames')
    p.add_argument('--fullsplit', action='store_true', help='Capture 7008x7000 and split to four packed 7000x1750 images in RAM')
    a = p.parse_args()
    if not 1 <= a.cycles <= 100:
        p.error('cycles must be between 1 and 100')
    out = a.out.resolve()
    if out.exists():
        p.error('Output directory already exists')
    out.parent.mkdir(parents=True,exist_ok=True)
    logpath = out.with_name(out.name+'.log')
    if logpath.exists():
        p.error('Log already exists')
    subprocess.run(['make','build/roi_switch_bench'],cwd=ROOT,check=True)
    command = [str(ROOT/'build/roi_switch_bench'),str(out),str(a.cycles),a.write_node]
    if sum([a.pipeline2,a.pipeline24,a.fullsplit]) > 1:
        p.error('Select only one experiment variant')
    if a.fullsplit:
        command.append('fullsplit')
    elif a.pipeline24:
        command.append('pipeline24')
    elif a.pipeline2:
        command.append('pipeline2')
    with logpath.open('x') as log:
        proc = subprocess.Popen(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        def stop(signum, frame):
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        signal.signal(signal.SIGTERM,stop)
        try:
            code = proc.wait()
        except KeyboardInterrupt:
            stop(signal.SIGINT,None)
            code = proc.wait(timeout=30)
    if out.exists():
        (out/'config.json').write_text(json.dumps({'command':command,'cycles':a.cycles,'write_node':a.write_node,'pipeline2':a.pipeline2,'pipeline24':a.pipeline24,'fullsplit':a.fullsplit,'returncode':code},indent=2))
    if (out/'frames.csv').exists():
        subprocess.run([sys.executable,str(ROOT/'scripts/report_roi_switch.py'),str(out)],check=True)
    print('Log:',logpath)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
