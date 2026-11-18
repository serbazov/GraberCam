#!/usr/bin/env python3
"""Fit sensor/exposure timing on training sweeps and assess independent sweeps.

Uses clean discard measurements only for fitting. Copy results are evaluated
separately: memcpy and host stalls are not part of the camera timing model.
No third-party packages required.
"""
import argparse
import csv
import json
import math
import pathlib
import statistics


def clean(row):
    return (not any(row.get(k) for k in ('error', 'dropped', 'disorder', 'timeouts'))
            and row.get('frames', 0) >= 2 and row.get('delivered_fps', 0) > 0)


def linear_fit(points):
    if len({x for x, _ in points}) < 2:
        raise ValueError('At least two distinct coordinates required')
    xm = statistics.mean(x for x, _ in points)
    ym = statistics.mean(y for _, y in points)
    slope = sum((x-xm)*(y-ym) for x, y in points) / sum((x-xm)**2 for x, _ in points)
    return slope, ym - slope*xm


def fit_models(rows):
    models = {}
    for fmt in sorted({r['format'] for r in rows}):
        sample = [r for r in rows if r['format'] == fmt and r['mode'] == 'discard' and clean(r)]
        if not sample:
            raise ValueError(f'No clean discard training measurements for {fmt}')
        shortest = min(float(r['exposure']) for r in sample)
        short = [r for r in sample if float(r['exposure']) == shortest]
        heights = sorted({r['height'] for r in short})
        # Each height has equal weight; median over widths/repeats limits host outliers.
        readout_points = [(h, statistics.median(1e6/r['delivered_fps'] for r in short if r['height'] == h)) for h in heights]
        slope, intercept = linear_fit(readout_points)
        min_height = min(heights)
        readout_floor = slope*min_height + intercept
        long = [r for r in sample if r['height'] == min_height and float(r['exposure']) >= 2*readout_floor]
        exposures = sorted({float(r['exposure']) for r in long})
        exposure_points = [(e, statistics.median(1e6/r['delivered_fps'] for r in long if float(r['exposure']) == e)) for e in exposures]
        scale, offset = linear_fit(exposure_points)
        if min(slope, intercept, scale, offset) <= 0:
            raise ValueError(f'Unphysical fit for {fmt}')
        models[fmt] = dict(row_us=slope, readout_overhead_us=intercept,
                           exposure_scale=scale, exposure_overhead_us=offset,
                           readout_fit_points=readout_points,
                           exposure_fit_points=exposure_points,
                           fit_discard_samples=len(sample))
    return models


def predict(model, height, exposure):
    readout = model['row_us']*height + model['readout_overhead_us']
    exposure_period = model['exposure_scale']*exposure + model['exposure_overhead_us']
    return 1e6/max(readout, exposure_period)


def load_rows(directories):
    rows = []
    for directory in directories:
        json_path = directory/'results.json'
        if json_path.exists():
            source = json.loads(json_path.read_text())
        else:
            with (directory/'results.csv').open() as f:
                source = list(csv.DictReader(f))
            for row in source:
                for key in ('width', 'height', 'frames', 'dropped', 'disorder', 'timeouts', 'repeat'):
                    if row.get(key):
                        row[key] = int(row[key])
                row['delivered_fps'] = float(row['delivered_fps'])
        for row in source:
            rows.append(dict(row, dataset=directory.name))
    return rows


def score_rows(models, rows, split):
    scored = []
    for row in rows:
        model = models[row['format']]
        predicted = predict(model, row['height'], float(row['exposure']))
        actual = row['delivered_fps']
        scored.append(dict(split=split, dataset=row['dataset'], format=row['format'],
                           width=row['width'], height=row['height'], exposure=row['exposure'],
                           mode=row['mode'], repeat=row.get('repeat', 1), observed_fps=actual,
                           predicted_fps=predicted, error_pct=100*(actual/predicted-1),
                           abs_error_pct=100*abs(actual/predicted-1), clean=clean(row),
                           dropped=row.get('dropped', 0), disorder=row.get('disorder', 0),
                           timeouts=row.get('timeouts', 0), report=row.get('report', '')))
    return scored


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--train', nargs='+', type=pathlib.Path)
    p.add_argument('--validate', nargs='+', type=pathlib.Path, default=[])
    p.add_argument('--out', type=pathlib.Path)
    p.add_argument('--model', type=pathlib.Path)
    p.add_argument('--predict', nargs=4, metavar=('FORMAT', 'WIDTH', 'HEIGHT', 'EXPOSURE_US'))
    args = p.parse_args()
    if args.predict:
        if not args.model:
            p.error('--predict needs --model')
        fmt, width, height, exposure = args.predict
        width, height, exposure = int(width), int(height), float(exposure)
        if not math.isfinite(exposure) or min(width, height, exposure) <= 0:
            p.error('Prediction inputs must be finite and positive')
        artifact = json.loads(args.model.read_text())
        if fmt not in artifact['models']:
            p.error('Format not fitted')
        fps = predict(artifact['models'][fmt], height, exposure)
        print(json.dumps(dict(format=fmt, width=width, height=height, exposure_us=exposure,
                              camera_fps=fps, host_payload_bytes=width*height*(1 if fmt == 'Mono8' else 2),
                              host_MBps=fps*width*height*(1 if fmt == 'Mono8' else 2)/1e6,
                              note='Width affects payload, not fitted sensor timing; outside measured bounds this is extrapolation. Host/copy losses are not predicted.'), indent=2))
        return
    if not args.train or not args.out:
        p.error('Fitting needs --train and --out')
    if {d.resolve() for d in args.train} & {d.resolve() for d in args.validate}:
        p.error('Training and validation directories must be disjoint')
    training = load_rows(args.train)
    validation = load_rows(args.validate)
    models = fit_models(training)
    args.out.mkdir(parents=True, exist_ok=True)
    artifact = dict(models=models, training=[str(d) for d in args.train], validation=[str(d) for d in args.validate],
                    equation='1e6/max(row_us*height+readout_overhead_us, exposure_scale*exposure_us+exposure_overhead_us)',
                    width_term='No width coefficient: validated separately against unseen widths',
                    measured_bounds={k: [min(float(r[k]) for r in training+validation), max(float(r[k]) for r in training+validation)] for k in ('width','height','exposure')})
    (args.out/'model.json').write_text(json.dumps(artifact, indent=2))
    scored = score_rows(models, training, 'train') + score_rows(models, validation, 'validation')
    with (args.out/'predictions.csv').open('w') as f:
        writer = csv.DictWriter(f, fieldnames=list(scored[0]))
        writer.writeheader()
        writer.writerows(scored)
    text = ['# Эмпирическая модель FPS', '',
            '`FPS = 1e6 / max(a × Height + b, c × ExposureTime + d)`; времена в мкс.', '',
            'Коэффициенты подогнаны только по чистым discard-прогонам обучающих наборов. '
            'Для считывания используются минимальная выдержка и медианы периодов по ширинам '
            'на каждой высоте; для выдержки — минимальная высота и длинные выдержки. '
            'Проверочные наборы не участвуют в подгонке.', '',
            '| Формат | a, мкс/строку | b, мкс | c | d, мкс |', '|---|---:|---:|---:|---:|']
    for fmt, m in models.items():
        text.append(f"| {fmt} | {m['row_us']:.8f} | {m['readout_overhead_us']:.5f} | {m['exposure_scale']:.8f} | {m['exposure_overhead_us']:.5f} |")
    text += ['', '## Проверка точности', '',
             '| Набор | Формат | Режим | Всего | Чистых | Медиана абсолютной ошибки чистых, % | Максимум, % |',
             '|---|---|---|---:|---:|---:|---:|']
    for split in ('train', 'validation'):
        for fmt in models:
            for mode in ('discard', 'copy'):
                group = [r for r in scored if (r['split'],r['format'],r['mode']) == (split,fmt,mode)]
                ok = [r['abs_error_pct'] for r in group if r['clean']]
                if group:
                    text.append(f"| {split} | {fmt} | {mode} | {len(group)} | {len(ok)} | {statistics.median(ok):.5f} | {max(ok):.5f} |" if ok else f'| {split} | {fmt} | {mode} | {len(group)} | 0 | — | — |')
    text += ['', '## Проверка по ширинам (независимые discard-прогоны)', '',
             '| Формат | Ширина | Чистых | Медиана абсолютной ошибки, % | Максимум, % |',
             '|---|---:|---:|---:|---:|']
    for fmt in models:
        for width in sorted({r['width'] for r in validation}, reverse=True):
            ok = [r['abs_error_pct'] for r in scored if r['split']=='validation' and r['format']==fmt and r['width']==width and r['mode']=='discard' and r['clean']]
            if ok:
                text.append(f'| {fmt} | {width} | {len(ok)} | {statistics.median(ok):.5f} | {max(ok):.5f} |')
    text += ['', 'Модель описывает частоту камеры. Чистый frame_id не исключает замедления потока '
             'хостом; поэтому даже чистые copy-прогоны могут отклоняться от модели. '
             'Неполные/ошибочные прогоны и потери не исключаются из predictions.csv, '
             'но помечены clean=false и не участвуют в подгонке и статистике чистых прогонов.', '']
    (args.out/'fit-report.md').write_text('\n'.join(text))
    print(args.out/'fit-report.md')


if __name__ == '__main__':
    main()
