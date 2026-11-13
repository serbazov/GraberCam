#!/usr/bin/env python3
"""Generate CSV and a Markdown comparison from sweep_fps.py results (stdlib only)."""
import argparse
import csv
import json
import pathlib
import statistics


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=pathlib.Path)
    a = p.parse_args()
    rows = json.loads((a.directory / 'results.json').read_text())
    if not rows:
        p.error('No measurements')
    fields = ['width', 'height', 'exposure', 'mode', 'repeat', 'delivered_fps',
              'source_fps', 'frames', 'dropped', 'disorder', 'timeouts', 'payload_bytes',
              'grab_MBps', 'copy_MBps', 'queued_min', 'report']
    with (a.directory / 'results.csv').open('w') as f:
        writer = csv.DictWriter(f, fields, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(rows)
    widths = sorted({r['width'] for r in rows}, reverse=True)
    heights = sorted({r['height'] for r in rows}, reverse=True)
    exposures = sorted({float(r['exposure']) for r in rows}, reverse=True)
    def group(w, h, e, m):
        return [r for r in rows if (r['width'], r['height'], float(r['exposure']), r['mode']) == (w, h, e, m)]
    def rate(g):
        return statistics.median(r['delivered_fps'] for r in g)
    text = ['# Эксперимент: ROI, выдержка и частота кадров', '',
            'Камера MV-AX5F57MP050E, Magewell Flex I/O Quad CXP-12 Enhanced, CXP12_X4, Mono8.', '',
            'Значения — принятые кадры/с; при повторах показана медиана. '
            'Звёздочка означает пропуски frame_id, скачки назад, таймауты или ошибку хотя бы в одном повторе. '
            'Отсутствие пропусков frame_id само по себе не исключает торможение камеры при нехватке DMA-буферов.', '',
            '`discard` возвращает DMA-буфер без копирования; `copy` делает memcpy каждого кадра '
            'в один переиспользуемый RAM-буфер. Весь видеопоток в памяти не сохраняется. '
            'Дисковая запись кадров и GUI в измерении не участвуют.', '']
    config_path = a.directory / 'config.json'
    if config_path.exists():
        config = json.loads(config_path.read_text())
        text += [f"Исполняемый файл: `{config['binary']}`. Окно прогона: {config['seconds']:g} с; DMA-буферов: {config['buf']}; повторов: {config['repeats']}.", '']
    for w in widths:
        for m in ['discard', 'copy']:
            text += [f'## Ширина {w}, {m}', '',
                     '| Высота | ' + ' | '.join(f'{e:g} мкс' for e in exposures) + ' |',
                     '|---:|' + '---:|' * len(exposures)]
            for h in heights:
                cells = []
                for e in exposures:
                    g = group(w, h, e, m)
                    bad = any(r.get('dropped') or r.get('disorder') or r.get('timeouts') or r.get('error') for r in g)
                    cells.append((f'{rate(g):.2f}' + ('*' if bad else '')) if g else '—')
                text.append(f'| {h} | ' + ' | '.join(cells) + ' |')
            text.append('')
    text += ['## Потери и скорость копирования', '',
             '| Ширина×высота | Выдержка, мкс | Повтор | Принято, кадр/с | Пропуски | Таймауты | memcpy, МБ/с |',
             '|---|---:|---:|---:|---:|---:|---:|']
    for r in rows:
        if r['mode'] == 'copy':
            text.append(f"| {r['width']}×{r['height']} | {r['exposure']} | {r['repeat']} | {r['delivered_fps']:.2f} | {r['dropped']} | {r['timeouts']} | {r['copy_MBps']:.0f} |")
    if len(widths) > 1:
        text += ['', '## Влияние ширины', '',
                 '| Высота | Выдержка, мкс | Режим | Полная ширина, кадр/с | Меньшая ширина, кадр/с | Отношение |',
                 '|---:|---:|---|---:|---:|---:|']
        for h in heights:
            for e in exposures:
                for m in ['discard', 'copy']:
                    full, narrow = group(widths[0], h, e, m), group(widths[-1], h, e, m)
                    if full and narrow:
                        text.append(f'| {h} | {e:g} | {m} | {rate(full):.2f} | {rate(narrow):.2f} | {rate(narrow)/rate(full):.3f}× |')
    status_path = a.directory / 'status.json'
    if status_path.exists():
        status = json.loads(status_path.read_text())
        text += ['', '## Завершение', '', 'Ошибки: ' + (str(status['errors']) if status['errors'] else 'нет') + '.',
                 'Восстановление настроек: ' + json.dumps(status['restoration'], ensure_ascii=False) + '.']
    text += ['', 'Сырые отчёты: отдельные JSON для каждого прогона; полный журнал — `session.log`; '
             'исходные настройки — `initial.json`; все численные результаты — `results.csv`.', '']
    (a.directory / 'report.md').write_text('\n'.join(text))
    print(a.directory / 'report.md')


if __name__ == '__main__':
    main()
