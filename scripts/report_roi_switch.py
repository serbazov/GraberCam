#!/usr/bin/env python3
"""Summarize roi_switch_bench CSV logs; no third-party packages required."""
import argparse
import csv
import itertools
import pathlib
import statistics


def percentile(values, p):
    values = sorted(values)
    if not values:
        return None
    pos = (len(values)-1)*p/100
    lo = int(pos)
    return values[lo] + (values[min(lo+1, len(values)-1)]-values[lo])*(pos-lo)


def read_rows(path):
    with path.open() as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for k in list(r):
            if k not in ('test', 'mode'):
                r[k] = float(r[k])
    return rows


def summarize(frames, switches):
    keys = sorted({(r['test'], r['mode'], int(r['group'])) for r in frames+switches if r['test'] != 'warmup'})
    result = []
    for key in keys:
        ff = [r for r in frames if (r['test'], r['mode'], int(r['group'])) == key]
        ss = [r for r in switches if (r['test'], r['mode'], int(r['group'])) == key]
        observed = [r for r in ff if r['observed'] >= 0]
        entry = dict(test=key[0], mode=key[1], group=key[2], frames=len(ff), switches=len(ss), unknown=len(ff)-len(observed))
        for label, values in [
            ('write_ms', [(r['after_write_us']-r['after_stop_us'])/1000 for r in ss]),
            ('first_target_ms', [(r['first_target_us']-r['begin_us'])/1000 for r in ss if r['first_target_us'] >= 0]),
            ('frame_gap_ms', [(r['first_target_us']-r['previous_frame_us'])/1000 for r in ss if r['first_target_us'] >= 0 and r['previous_frame_us'] >= 0]),
            ('stop_ms', [(r['after_stop_us']-r['begin_us'])/1000 for r in ss]),
            ('restart_ms', [(r['after_start_us']-r['after_write_us'])/1000 for r in ss]),
        ]:
            entry[label+'_p50'] = percentile(values, 50)
            entry[label+'_p95'] = percentile(values, 95)
            entry[label+'_max'] = max(values, default=None)
        entry['confirmed_switches'] = sum(r['first_target_us'] >= 0 for r in ss)
        entry['stale_frames_p50'] = percentile([r['stale_frames'] for r in ss], 50)
        entry['dropped'] = sum(max(0, b['frame_id']-a['frame_id']-1) for a, b in zip(ff, ff[1:]) if a['block'] == b['block'])
        if key[0] == 'baseline':
            rates = []
            for block in sorted({r['block'] for r in ff}):
                block_frames = [r for r in ff if r['block'] == block]
                if len(block_frames) > 1:
                    rates.append((len(block_frames)-1)*1e6/(block_frames[-1]['ready_us']-block_frames[0]['ready_us']))
            entry['capture_fps'] = statistics.median(rates) if rates else None
            entry['wrong_zone'] = sum(r['observed'] != r['expected'] for r in observed)
        elif key[0] == 'full_frame' and len(ff) > 1:
            span = (ff[-1]['ready_us']-ff[0]['ready_us'])/1e6
            entry['capture_fps'] = (len(ff)-1)/span
            entry['roi_images_s'] = 4*entry['capture_fps']
            copies = [r['copy_us']/1000 for r in ff if r['copy_us'] > 0]
            entry['split_copy_ms_p50'] = percentile(copies,50)
            entry['split_copy_ms_p95'] = percentile(copies,95)
        elif key[0] == 'live_pipeline' and len(ff) > 1:
            span = (ff[-1]['ready_us']-ff[0]['ready_us'])/1e6
            entry['capture_fps'] = (len(ff)-1)/span
            transitions = [(a,b) for a,b in zip(ff,ff[1:]) if a['observed'] >= 0 and b['observed'] >= 0 and a['observed'] != b['observed']]
            entry['window_switches_s'] = len(transitions)/span
            entry['actual_switches_s'] = ((len(transitions)-1)*1e6/(transitions[-1][1]['ready_us']-transitions[0][1]['ready_us'])) if len(transitions)>1 else None
            entry['noncyclic_transitions'] = sum(b['observed'] != (a['observed']+1)%4 for a,b in transitions)
            runs = [(zone,len(list(run))) for zone,run in itertools.groupby(r['observed'] for r in ff)]
            lens = [length for zone,length in runs[1:-1] if zone >= 0]
            entry['zone_run_p50'] = percentile(lens,50)
            entry['zone_run_p95'] = percentile(lens,95)
            entry['zone_run_max'] = max(lens,default=None)
        elif ff and ss:
            span = (ff[-1]['ready_us']-ss[0]['begin_us'])/1e6
            if span > 0:
                entry['useful_fps'] = sum(r['observed'] == r['expected'] for r in ff)/span
                entry['actual_switches_s'] = entry['confirmed_switches']/span
        result.append(entry)
    return result


def number(x):
    return '—' if x is None else f'{x:.2f}'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=pathlib.Path)
    a = p.parse_args()
    ff = read_rows(a.directory/'frames.csv')
    ss = read_rows(a.directory/'switches.csv')
    summary = summarize(ff,ss)
    fields = sorted({key for row in summary for key in row})
    with (a.directory/'summary.csv').open('w') as f:
        writer = csv.DictWriter(f, ['test','mode','group']+[key for key in fields if key not in ('test','mode','group')])
        writer.writeheader()
        writer.writerows(summary)
    status = (a.directory/'status.log').read_text()
    lines = ['# Переключение четырёх ROI', '',
             'MV-AX5F57MP050E + Magewell Flex I/O Quad CXP-12 Enhanced; Mono8, '
             'ROI 7008×1750 (в тесте полной высоты — 7008×7000), выдержка 300 мкс; OffsetX=0, OffsetY=0/1750/3500/5250.', '',
             'Размер 7000 камера округляет до 7008 (шаг ширины 16). '
             'Положение кадра определяется сравнением изображения с четырьмя профилями полного кадра. '
             'Корреляция должна быть ≥0,90, отрыв от второго кандидата ≥0,002; иначе зона неизвестна. '
             'Это проверка в текущей статичной сцене, не универсальный алгоритм маркировки ROI.', '',
             'В `copy` memcpy выполняется для всего кадра в переиспользуемый RAM-буфер. '
             'Покадрового опроса stream info нет. Метаданные OffsetY не используются для подтверждения зоны.', '']
    lines += ['Параметры и результат восстановления:', '', '```text', status.rstrip(), '```', '',
              '## Базовый поток и проверка распознавания зон', '',
              '| Режим | Кадров/с | Кадров | Неизвестная зона | Неверная зона |',
              '|---|---:|---:|---:|---:|']
    for r in summary:
        if r['test']=='baseline':
            lines.append(f"| {r['mode']} | {number(r.get('capture_fps'))} | {r['frames']} | {r['unknown']} | {r['wrong_zone']} |")
    full = [r for r in summary if r['test']=='full_frame']
    if full:
        lines += ['', '## Полная высота и разделение в RAM', '',
                  'Кадр 7008×7000; split_copy упаковывает четыре изображения 7000×1750, обрезая 8 столбцов. '
                  'Внутри одного полного кадра зоны относятся к одной экспозиции.', '',
                  '| Режим | Полных кадров/с | ROI-изображений/с | Кадров | Пропуски | Разделение p50, мс | p95, мс |',
                  '|---|---:|---:|---:|---:|---:|---:|']
        for r in full:
            lines.append(f"| {r['mode']} | {number(r['capture_fps'])} | {number(r['roi_images_s'])} | {r['frames']} | {int(r['dropped'])} | {number(r['split_copy_ms_p50'])} | {number(r['split_copy_ms_p95'])} |")
    lines += ['', '## Время команды и первого кадра новой зоны', '',
              'Время первой новой зоны отсчитывается от начала процедуры переключения, '
              'включая паузу и перезапуск, если они есть. API-время записи не включает паузу. '
              'Для pipeline незавершённые команды в конце окна не входят в распределение задержки.', '',
              '| Метод | Режим | N кадров | Запись p50, мс | Новая зона p50, мс | p95, мс | Промежуточных кадров p50 |',
              '|---|---|---:|---:|---:|---:|---:|']
    for r in summary:
        if r['test'] in ('baseline','full_frame'): continue
        lines.append(f"| {r['test']} | {r['mode']} | {r['group']} | {number(r['write_ms_p50'])} | {number(r['first_target_ms_p50'])} | {number(r['first_target_ms_p95'])} | {number(r['stale_frames_p50'])} |")
    lines += ['', '## Практическая частота', '',
              '`live_confirmed` ждёт кадра нужной зоны и получает N подтверждённых кадров, '
              'затем переключает окно. `live_pipeline` выдаёт команды из отдельного потока '
              'по счётчику принятых кадров; возможны задержки очереди команд. '
              'Фактические переключения pipeline подсчитаны по изменению изображения, '
              'а не по числу успешных записей координаты. Частота pipeline вычислена между первым и последним наблюдаемыми переходами, чтобы исключить неполные серии на краях окна. Цикл четырёх зон = число переключений/с ÷ 4 '
              'только при правильном порядке переходов.', '',
              '| Метод | Режим | N | Входящих кадров/с | Полезных кадров/с | Переключений/с | Пропуски frame_id | Неизвестная зона |',
              '|---|---|---:|---:|---:|---:|---:|---:|']
    for r in summary:
        if r['test'] in ('baseline','write_only','full_frame'): continue
        lines.append(f"| {r['test']} | {r['mode']} | {r['group']} | {number(r.get('capture_fps'))} | {number(r.get('useful_fps'))} | {number(r.get('actual_switches_s'))} | {int(r['dropped'])} | {r['unknown']} |")
    lines += ['', '## Точность покадрового порядка pipeline', '',
              'Длина серии — число подряд пришедших кадров одной фактической зоны; '
              'первая и последняя неполные серии исключены. Непоследовательный переход — '
              'изменение зоны не в порядке 0→1→2→3→0.', '',
              '| Режим | N | Длина серии p50 | p95 | max | Непоследовательные переходы |',
              '|---|---:|---:|---:|---:|---:|']
    for r in summary:
        if r['test']=='live_pipeline':
            lines.append(f"| {r['mode']} | {r['group']} | {number(r.get('zone_run_p50'))} | {number(r.get('zone_run_p95'))} | {number(r.get('zone_run_max'))} | {r['noncyclic_transitions']} |")
    lines += ['', '## Методы', '',
              '- `write_only`: запись OffsetY при остановленном захвате.',
              '- `pause`: scap_pause → запись координаты → scap_restart. DMA-буферы сохраняются.',
              '- `reopen`: scap_stop → запись координаты → scap_start; линк и устройство остаются открытыми.',
              '- `live_confirmed`, `live_pipeline`: поток не останавливается, локальная блокировка TLParamsLocked снята; размер кадра не меняется.', '',
              'Полные данные: `frames.csv`, `switches.csv`, `summary.csv`; исходные настройки: `initial.csv`. '
              'Измерения относятся к текущей камере, прошивке, SDK, ОС и сцене. '
              'Перезапуски могут сбрасывать frame_id; пропуски считаются внутри непрерывных блоков.', '']
    (a.directory/'report.md').write_text('\n'.join(lines))
    print(a.directory/'report.md')


if __name__ == '__main__':
    main()
