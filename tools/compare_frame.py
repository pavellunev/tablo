#!/usr/bin/env python3
"""Сверка каркаса кадра с эталоном — измеримая замена проверке «на глаз».

Линия-разделитель — это сплошной пробег тёмных пикселей вдоль строки/столбца
длиной от полусотни пикселей и больше; текст такого пробега не даёт (буквы
разрозненны, самый длинный сплошной штрих — от силы 10-15px даже в крупном
кегле). Для каждой строки/столбца ищутся **все** такие пробеги, а не только
самый длинный: на одной и той же колонке в разных диапазонах Y может лежать
две разные линии (например, разделитель CO₂|TVOC в верхнем ряду и разделитель
Почта|Сегодня в нижнем — у обоих в эталоне x=567, но разная высота и разный Y)
— взять только самый длинный пробег значило бы потерять вторую линию молча.

Раз попиксельного совпадения не будет (в эталоне другие данные — курсы,
время), сверяем структуру: где проходят линии, какой они длины и в каком
Y/X-диапазоне, а не что нарисовано между ними.

Использование:
    python3 tools/compare_frame.py <кадр.png> [эталон.png]

Без эталона просто печатает найденные линии (полезно смотреть на сам кадр
после правки). С эталоном — сверяет и печатает совпадения/расхождения.
"""
from __future__ import annotations

import sys
from dataclasses import dataclass

import numpy as np
from PIL import Image

DARK_THRESHOLD = 128    # ниже — считаем пикселем "чёрным"
MIN_RUN_H = 120         # минимальная длина сплошного пробега для горизонтальной линии
MIN_RUN_V = 50          # то же для вертикальной (внутриблочные разделители короче)
MERGE_GAP = 2           # соседние строки/столбцы-кандидаты в пределах зазора — одна линия
OVERLAP_RATIO = 0.5     # доля перекрытия пробегов, чтобы считать их одной линией


@dataclass
class Line:
    kind: str  # "h" | "v"
    start: int  # y для h, x для v
    end: int
    span_lo: int  # x-диапазон пробега для h, y-диапазон для v
    span_hi: int


def load_dark_mask(path: str) -> np.ndarray:
    img = Image.open(path).convert("L")
    arr = np.asarray(img)
    return arr < DARK_THRESHOLD


def all_runs(bits: np.ndarray, min_len: int) -> list[tuple[int, int]]:
    """Все пробеги True длиной >= min_len в 1D-массиве, каждый как (lo, hi)."""
    if not bits.any():
        return []
    padded = np.concatenate(([False], bits, [False]))
    diffs = np.diff(padded.astype(np.int8))
    starts = np.nonzero(diffs == 1)[0]
    ends = np.nonzero(diffs == -1)[0] - 1
    return [(int(s), int(e)) for s, e in zip(starts, ends) if e - s + 1 >= min_len]


def overlaps(a: tuple[int, int], b: tuple[int, int]) -> bool:
    lo = max(a[0], b[0])
    hi = min(a[1], b[1])
    if hi < lo:
        return False
    inter = hi - lo + 1
    shortest = min(a[1] - a[0] + 1, b[1] - b[0] + 1)
    return inter >= shortest * OVERLAP_RATIO


def find_lines(mask: np.ndarray, kind: str, min_run: int) -> list[Line]:
    h, w = mask.shape
    count = h if kind == "h" else w

    # candidates[i] — список пробегов (lo, hi) на строке/столбце i
    candidates: list[tuple[int, tuple[int, int]]] = []
    for i in range(count):
        row = mask[i, :] if kind == "h" else mask[:, i]
        for run in all_runs(row, min_run):
            candidates.append((i, run))

    # Открытые кластеры: каждый — линия в процессе накопления соседних строк/
    # столбцов с перекрывающимся пробегом. Кластер закрывается, если следующая
    # строка/столбец за пределами MERGE_GAP не расширила ни один из них.
    open_clusters: list[dict] = []
    closed: list[Line] = []

    def close(cluster: dict) -> Line:
        return Line(kind, cluster["idx_lo"], cluster["idx_hi"], cluster["span_lo"],
                    cluster["span_hi"])

    idx_groups: dict[int, list[tuple[int, int]]] = {}
    for i, run in candidates:
        idx_groups.setdefault(i, []).append(run)

    for i in sorted(idx_groups):
        runs_here = idx_groups[i]
        still_open = []
        used_runs = set()
        for cluster in open_clusters:
            if i - cluster["idx_hi"] > MERGE_GAP:
                closed.append(close(cluster))
                continue
            matched = None
            for ri, run in enumerate(runs_here):
                if ri in used_runs:
                    continue
                if overlaps(run, (cluster["span_lo"], cluster["span_hi"])):
                    matched = (ri, run)
                    break
            if matched is not None:
                ri, run = matched
                used_runs.add(ri)
                cluster["idx_hi"] = i
                cluster["span_lo"] = min(cluster["span_lo"], run[0])
                cluster["span_hi"] = max(cluster["span_hi"], run[1])
            still_open.append(cluster)
        open_clusters = [c for c in still_open if i - c["idx_hi"] <= MERGE_GAP]
        for ri, run in enumerate(runs_here):
            if ri in used_runs:
                continue
            open_clusters.append({"idx_lo": i, "idx_hi": i, "span_lo": run[0], "span_hi": run[1]})

    for cluster in open_clusters:
        closed.append(close(cluster))

    closed.sort(key=lambda l: (l.start, l.span_lo))
    return closed


def describe(lines: list[Line]) -> str:
    out = []
    for l in lines:
        mid = (l.start + l.end) // 2
        out.append(f"  {l.kind} @ {l.start}-{l.end} (центр {mid}), охват {l.span_lo}-{l.span_hi} "
                   f"({l.span_hi - l.span_lo + 1}px)")
    return "\n".join(out) if out else "  (нет)"


def match_lines(a: list[Line], b: list[Line], tolerance: int = 4) -> bool:
    """Для каждой линии эталона (b) ищет ближайшую в проверяемом кадре (a) по
    координате центра И перекрытию охвата (иначе две параллельные линии на
    разной высоте в одной колонке спутались бы друг с другом)."""
    used: set[int] = set()
    all_ok = True
    for lb in b:
        mid_b = (lb.start + lb.end) // 2
        best = None
        best_dist = None
        for i, la in enumerate(a):
            if i in used:
                continue
            if not overlaps((la.span_lo, la.span_hi), (lb.span_lo, lb.span_hi)):
                continue
            mid_a = (la.start + la.end) // 2
            dist = abs(mid_a - mid_b)
            if best_dist is None or dist < best_dist:
                best_dist = dist
                best = i
        if best is not None:
            used.add(best)
            la = a[best]
            mid_a = (la.start + la.end) // 2
            ok = best_dist <= tolerance
            all_ok &= ok
            status = "OK" if ok else "СДВИГ"
            print(f"  [{status}] эталон центр={mid_b} охват={lb.span_lo}-{lb.span_hi} "
                  f"<-> кадр центр={mid_a} охват={la.span_lo}-{la.span_hi}  Δ={mid_a - mid_b}")
        else:
            all_ok = False
            print(f"  [НЕТ В КАДРЕ] эталон центр={mid_b} охват={lb.span_lo}-{lb.span_hi}")
    for i, la in enumerate(a):
        if i not in used:
            all_ok = False
            mid_a = (la.start + la.end) // 2
            print(f"  [ЛИШНЕЕ В КАДРЕ] центр={mid_a} охват={la.span_lo}-{la.span_hi}")
    return all_ok


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    frame_path = sys.argv[1]
    mask = load_dark_mask(frame_path)
    hlines = find_lines(mask, "h", MIN_RUN_H)
    vlines = find_lines(mask, "v", MIN_RUN_V)

    print(f"=== {frame_path} ===")
    print("Горизонтальные линии:")
    print(describe(hlines))
    print("Вертикальные линии:")
    print(describe(vlines))

    if len(sys.argv) >= 3:
        ref_path = sys.argv[2]
        ref_mask = load_dark_mask(ref_path)
        ref_h = find_lines(ref_mask, "h", MIN_RUN_H)
        ref_v = find_lines(ref_mask, "v", MIN_RUN_V)
        print(f"\n=== эталон {ref_path} ===")
        print("Горизонтальные линии:")
        print(describe(ref_h))
        print("Вертикальные линии:")
        print(describe(ref_v))

        print("\n=== сверка: горизонтальные ===")
        ok_h = match_lines(hlines, ref_h)
        print("=== сверка: вертикальные ===")
        ok_v = match_lines(vlines, ref_v)
        print(f"\n{'ВСЁ СОШЛОСЬ' if ok_h and ok_v else 'ЕСТЬ РАСХОЖДЕНИЯ'}")
        return 0 if ok_h and ok_v else 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
