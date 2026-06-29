# sa-stress — Automatic-1..9 all-time best layouts

Best validated `sa-stress` drawings collected from `results/best/Automatic-N/sa-stress.json`.
Each `Automatic-N.json` is a contest-format layout; all independently re-verified with
`./sakgd --verify` (valid = no vertex-edge overlap, k matches the record).

| Graph | k (max crossings/edge) | total crossings |
|-------|------------------------|-----------------|
| Automatic-1 | 9   | 409    |
| Automatic-2 | 3   | 125    |
| Automatic-3 | 37  | 4508   |
| Automatic-4 | 6   | 649    |
| Automatic-5 | 75  | 45748  |
| Automatic-6 | 737 | 559811 |
| Automatic-7 | 28  | 11324  |
| Automatic-8 | 6   | 11040  |
| Automatic-9 | 10  | 7034   |

Note: these are the best produced *by the sa-stress method specifically*. For a few
graphs another method holds the overall record (e.g. A4 ils=6 ties; dense A6 plain
sa=624 < sa-stress 737 — stress init hurts on A6). Re-verify any file before use:
`./sakgd --verify results/sa-stress-best/Automatic-N.json`
