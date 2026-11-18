"""Exercise the hardware runner protocol and restoration without a camera."""
import json
import os
import signal
import time
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
FAKE = '''#!/usr/bin/env python3
import json, pathlib, sys
settings = dict(Width='64', Height='32', OffsetX='4', OffsetY='4', PixelFormat='Mono10', ExposureTime='2300', TriggerMode='On', AcquisitionMode='Continuous', AcquisitionFrameRateEnable='1')
for line in sys.stdin:
    words = line.split()
    if not words: continue
    command = words[0]
    if command == 'quit': break
    if command == 'get':
        name = words[1]
        print(name + ' = ' + settings.get(name, 'FAKE123'), flush=True)
    if command == 'set':
        name, value = words[1:3]
        if value == '999':
            print(name + ': rejected', end='', flush=True)
        else:
            settings[name] = value
            print(name + ': ok', flush=True)
    if command == 'bench':
        if '--no-json' in words: continue
        if settings['ExposureTime'] == '777':
            import time
            time.sleep(0.5)
        mode = words[words.index('--mode')+1]
        path = words[words.index('--json')+1]
        row = dict(format=settings['PixelFormat'], width=int(settings['Width']), height=int(settings['Height']), exposure=settings['ExposureTime'], mode=mode, frames=100, delivered_fps=500.0, source_fps=500.0, dropped=0, disorder=0, timeouts=0, payload_bytes=512, grab_MBps=.256, copy_MBps=1234, queued_min=7, error='')
        pathlib.Path(path).write_text(json.dumps([row]))
pathlib.Path(__file__ + '.final').write_text(json.dumps(settings))
'''


class SweepTests(unittest.TestCase):
    def exercise(self, exposure, formats=('Mono8',)):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            binary = root / 'fake_camera'
            binary.write_text(FAKE)
            binary.chmod(0o755)
            out = root / 'run'
            run = subprocess.run([sys.executable, str(ROOT/'scripts/sweep_fps.py'), '--binary', str(binary), '--out', str(out), '--widths', '32', '--heights', '16', '--exposures', str(exposure), '--formats', *formats, '--precondition-seconds', '0.1'], capture_output=True, text=True, timeout=10)
            initial = json.loads((out/'initial.json').read_text())
            final = json.loads(pathlib.Path(str(binary)+'.final').read_text())
            self.assertEqual(initial, final, run.stderr)
            status = json.loads((out/'status.json').read_text())
            self.assertTrue(all(v == 'ok' for v in status['restoration'].values()))
            if exposure == 999:
                self.assertNotEqual(run.returncode, 0)
                self.assertTrue(status['errors'])
            else:
                self.assertEqual(run.returncode, 0, run.stderr)
                rows = json.loads((out/'results.json').read_text())
                self.assertEqual([r['mode'] for r in rows], ['discard', 'copy'] * len(formats))
                self.assertEqual([r['format'] for r in rows], [fmt for fmt in formats for _ in range(2)])
                self.assertEqual(len({r['report'] for r in rows}), 2 * len(formats))
                subprocess.run([sys.executable, str(ROOT/'scripts/report_fps.py'), str(out)], check=True, capture_output=True, timeout=10)
                self.assertIn('500.00', (out/'report.md').read_text())
                self.assertEqual(len((out/'results.csv').read_text().splitlines()), 1 + 2 * len(formats))
                for fmt in formats:
                    self.assertIn(f'## {fmt},', (out/'report.md').read_text())

    def test_sigterm_during_benchmark_restores_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            binary = root / 'fake_camera'
            binary.write_text(FAKE)
            binary.chmod(0o755)
            out = root / 'run'
            proc = subprocess.Popen([sys.executable, str(ROOT/'scripts/sweep_fps.py'), '--binary', str(binary), '--out', str(out), '--widths', '32', '--heights', '16', '--exposures', '777'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertIn('discard', proc.stdout.readline())
            time.sleep(.1)
            os.kill(proc.pid, signal.SIGTERM)
            _, stderr = proc.communicate(timeout=10)
            self.assertNotEqual(proc.returncode, 0)
            initial = json.loads((out/'initial.json').read_text())
            final = json.loads(pathlib.Path(str(binary)+'.final').read_text())
            self.assertEqual(initial, final, stderr)
            self.assertTrue(all(v == 'ok' for v in json.loads((out/'status.json').read_text())['restoration'].values()))

    def test_success_restores_settings_and_reports(self):
        self.exercise(27)

    def test_formats_have_separate_measurements_and_tables(self):
        self.exercise(27, formats=('Mono8', 'Mono10'))

    def test_rejected_setting_restores_settings(self):
        self.exercise(999)


if __name__ == '__main__':
    unittest.main()
