#!/usr/bin/env python3
"""Build an experimental benchmark without per-frame stream diagnostics.

Leaves application sources and existing binaries untouched. Use sweep_fps.py
--binary PATH for an A/B experiment, not as the production application.
"""
import argparse
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=pathlib.Path, required=True)
    a = p.parse_args()
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=False)
    source = (ROOT/'src/main.cpp').read_text()
    needle = 'if (scap_get_data_stream_info(session.cap, &stream)) {'
    if source.count(needle) != 1:
        raise RuntimeError('Source changed: expected exactly one per-frame diagnostic query')
    source = source.replace(needle, 'if (false && scap_get_data_stream_info(session.cap, &stream)) {')
    cpp = a.out/'main-no-stream-stats.cpp'
    cpp.write_text(source)
    command = ['g++', '-std=c++17', '-O2', '-Wall', '-Wextra',
               '-I'+str(ROOT/'src'), '-I/opt/magewell/libeasycap/include',
               '-o', str(a.out/'grabercam-probe'), str(cpp),
               str(ROOT/'src/session.cpp'), str(ROOT/'build/easycap.a'),
               '-ldl', '-lpthread', '-lrt', '-l:libreadline.so.8']
    subprocess.run(command, check=True)
    print(a.out/'grabercam-probe')


if __name__ == '__main__':
    main()
