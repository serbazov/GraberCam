#!/usr/bin/env python3
"""Hardware ROI/exposure sweep using one grabercam shell; no frame files written."""
import argparse
import datetime
import json
import pathlib
import queue
import signal
import subprocess
import threading
import time


class Camera:
    def __init__(self, binary, log):
        self.log = log
        self.lines = queue.Queue()
        self.proc = subprocess.Popen(
            ['stdbuf', '-oL', '-eL', str(binary), 'shell'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        def reader():
            for line in self.proc.stdout:
                log.write(line)
                log.flush()
                self.lines.put(line.rstrip())
            self.lines.put(None)
        threading.Thread(target=reader, daemon=True).start()
        try:
            self.command('info')
        except BaseException:
            self.close()
            raise

    def command(self, command, timeout=30):
        # Readback delimiter also confirms the shell has finished the command.
        self.proc.stdin.write(command + '\nget DeviceSerialNumber\n')
        self.proc.stdin.flush()
        result = []
        deadline = time.monotonic() + timeout
        interrupted = False
        while True:
            try:
                line = self.lines.get(timeout=max(.01, deadline-time.monotonic()))
            except KeyboardInterrupt:
                # Finish consuming this command before restoring settings.
                interrupted = True
                continue
            except queue.Empty:
                raise TimeoutError(command)
            if line is None:
                raise RuntimeError(f'Camera process exited: {self.proc.poll()}: {result}')
            marker = line.find('DeviceSerialNumber = ')
            if marker >= 0:
                # SDK exception text sometimes omits its trailing newline.
                if marker:
                    result.append(line[:marker])
                if interrupted:
                    raise KeyboardInterrupt
                return result
            result.append(line)

    def get(self, name):
        prefix = name + ' = '
        for line in self.command('get ' + name):
            if line.startswith(prefix):
                return line[len(prefix):]
        raise RuntimeError('Cannot read ' + name)

    def set(self, name, value):
        lines = self.command(f'set {name} {value}')
        if name + ': ok' not in lines:
            raise RuntimeError(f'Cannot set {name}={value}: {lines}')
        actual = self.get(name)
        try:
            equal = float(actual) == float(value)
        except ValueError:
            equal = actual == str(value)
        if not equal:
            raise RuntimeError(f'{name}: requested {value}, read back {actual}')

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.write('quit\n')
            self.proc.stdin.flush()
            self.proc.wait(timeout=20)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=pathlib.Path, default=pathlib.Path('./grabercam'))
    p.add_argument('--out', type=pathlib.Path, default=pathlib.Path('experiments') / datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    p.add_argument('--widths', nargs='+', type=int, default=[9344, 4672])
    p.add_argument('--heights', nargs='+', type=int, default=[7000, 2500, 1000, 500, 100, 16])
    p.add_argument('--exposures', nargs='+', type=int, default=[5000, 1000, 100, 27])
    p.add_argument('--seconds', type=float, default=3)
    p.add_argument('--repeats', type=int, default=1)
    p.add_argument('--buf', type=int, default=8)
    a = p.parse_args()
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    if min(a.widths + a.heights + a.exposures) <= 0 or a.seconds <= 0 or a.repeats < 1 or a.buf < 2:
        p.error('Dimensions, exposure, duration and repeats must be positive; buf >= 2')
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=False)
    (a.out / 'config.json').write_text(json.dumps({k: str(v) if isinstance(v, pathlib.Path) else v for k, v in vars(a).items()}, indent=2))
    results = []
    errors = []
    initial = {}
    restoration = {}
    snapshot_complete = False
    camera = None
    with (a.out / 'session.log').open('w') as log:
        try:
            camera = Camera(a.binary.resolve(), log)
            for key in ['Width', 'Height', 'OffsetX', 'OffsetY', 'PixelFormat',
                        'ExposureTime', 'TriggerMode', 'AcquisitionMode', 'AcquisitionFrameRateEnable']:
                initial[key] = camera.get(key)
            (a.out / 'initial.json').write_text(json.dumps(initial, indent=2))
            snapshot_complete = True
            camera.set('AcquisitionFrameRateEnable', '0')
            camera.set('TriggerMode', 'Off')
            camera.set('AcquisitionMode', 'Continuous')
            camera.set('OffsetX', 0)
            camera.set('OffsetY', 0)
            camera.set('PixelFormat', 'Mono8')
            for width in a.widths:
                camera.set('Width', width)
                for height in a.heights:
                    camera.set('Height', height)
                    for exposure in a.exposures:
                        camera.set('ExposureTime', exposure)
                        for repeat in range(a.repeats):
                            # Alternate order between repetitions to reduce order bias.
                            for mode in (['discard', 'copy'] if repeat % 2 == 0 else ['copy', 'discard']):
                                name = f'w{width}_h{height}_e{exposure}_{mode}_r{repeat+1}'
                                path = a.out / (name + '.json')
                                print(name, flush=True)
                                camera.command(f'bench --mode {mode} -t {a.seconds} --warmup 16 --buf {a.buf} --json {path}', timeout=a.seconds+30)
                                rows = json.loads(path.read_text())
                                if len(rows) != 1:
                                    raise RuntimeError(f'Unexpected report: {path}')
                                row = rows[0]
                                if row.get('error') or row.get('frames', 0) < 2:
                                    raise RuntimeError(f'Failed benchmark: {row}')
                                if (row['width'], row['height'], float(row['exposure']), row['mode']) != (width, height, exposure, mode):
                                    raise RuntimeError(f'Benchmark configuration mismatch: {row}')
                                row.update(repeat=repeat+1, report=path.name)
                                results.append(row)
                                (a.out / 'results.json').write_text(json.dumps(results, indent=2))
                                print(f"  {row['delivered_fps']:.2f} fps, dropped={row['dropped']}, timeouts={row['timeouts']}", flush=True)
        except BaseException as exc:
            errors.append(f'{type(exc).__name__}: {exc}')
            raise
        finally:
            if camera is not None and snapshot_complete:
                # Restore geometry before offsets, and restore the rate switch last.
                for key in ['OffsetX', 'OffsetY', 'Width', 'Height', 'PixelFormat', 'ExposureTime',
                            'AcquisitionMode', 'TriggerMode', 'AcquisitionFrameRateEnable']:
                    try:
                        camera.set(key, 0 if key in ['OffsetX', 'OffsetY'] else initial[key])
                        restoration[key] = 'ok'
                    except Exception as exc:
                        restoration[key] = str(exc)
                for key in ['OffsetX', 'OffsetY']:
                    try:
                        camera.set(key, initial[key])
                        restoration[key] = 'ok'
                    except Exception as exc:
                        restoration[key] = str(exc)
            (a.out / 'status.json').write_text(json.dumps({'errors': errors, 'restoration': restoration}, indent=2))
            if camera is not None:
                camera.close()
            print('Reports:', a.out, flush=True)
            if any(v != 'ok' for v in restoration.values()):
                raise RuntimeError(f'Settings restoration failed: {restoration}')


if __name__ == '__main__':
    main()
