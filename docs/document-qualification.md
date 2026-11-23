# Bounded document viewing: measured profile {#document-qualification}

[Русская версия](ru/document-qualification.md) · [DocumentView](document-view.md) · [Reproduce the tools](document-probe-reference.md)

This page records bounded viewing and a separate End optimization profile.
Results apply to the stated workloads rather than arbitrary files.

## Faster End on a file-sized line {#document-end-optimization}

The updated runtime batches bounded text-tail preparation, avoids constructing
invisible printable ASCII rows, and reuses a native file handle within each
worker job. No client-code change is required. The following profile uses one
generated SQL line occupying the entire 64 MiB or 1 GiB file, AUTO coloring,
one/four visible viewers sharing a source and a 128 MiB credit root, and a total
120×40 viewport. Windows and WSL/ext4 use three independent alternating
before/after pairs. WSL/NTFS is a separate `n=1` storage control.

| Platform / storage | MiB | Views | Before, s | After, s | Speedup | After peak RSS, MiB |
|---|---:|---:|---:|---:|---:|---:|
| Windows / NTFS | 64 | 1 | 7.927 | 0.433 | 18.3× | 12.91 |
| Windows / NTFS | 64 | 4 | 7.925 | 0.582 | 13.6× | 13.52 |
| Windows / NTFS | 1024 | 1 | 126.268 | 6.230 | 20.3× | 12.84 |
| Windows / NTFS | 1024 | 4 | 126.177 | 8.470 | 14.9× | 13.38 |
| WSL Ubuntu / ext4 | 64 | 1 | 5.265 | 0.234 | 22.5× | 26.90 |
| WSL Ubuntu / ext4 | 64 | 4 | 5.295 | 0.395 | 13.4× | 26.90 |
| WSL Ubuntu / ext4 | 1024 | 1 | 83.417 | 3.316 | 25.2× | 26.90 |
| WSL Ubuntu / ext4 | 1024 | 4 | 83.548 | 5.106 | 16.4× | 26.90 |
| WSL Ubuntu / NTFS | 1024 | 1 | — | 36.844 | — | 25.80 |
| WSL Ubuntu / NTFS | 1024 | 4 | — | 99.301 | — | 25.80 |

Times are medians where `n=3`; memory is the largest process peak. The
[complete reports](measurements/document-end-qualification.json) retain individual
ranges, private commit where available, source-credit peaks, CPU, phases, source
hashes and completion/cleanup results. The source credit limit and byte quantum
were not raised. The process does not retain the entire file; kernel file-cache
memory is outside these process counters. Linux's reported high-water mark can
already be present at the pre-Application baseline; the individual reports retain
current and highest sampled RSS separately. End still scans relevant source data,
and admitted bytes are not physical disk traffic. Unicode fallback, storage
latency and exact-color recovery can have different costs. These headless
observations do not measure physical Windows Terminal presentation.

### Original qualification baseline

The tables below retain the earlier production qualification and its failed
stress observation. They are historical results, not reruns of the updated SDK.

## Workload and results

Release MSVC 19.51 and GCC 14, Ryzen 9 5950X, 128 GiB RAM, 22 November 2026.
Linux runs use Ubuntu 24.04 in WSL and read the same NTFS files; these environments
are not a native-storage comparison. OS caches were not flushed, and fixture
generation and hash checks could warm them.

There are 192 independent processes: SQL/Markdown, 2/8 GiB, raw/gzip,
plain/highlight, one/four visible views, two platforms, three repetitions in
alternating order. Four views share one source and one 128 MiB credit root in
four quadrants of a total 120×40 viewport. Gzip sizes are decompressed extents.
The finite navigation plan includes scrolling, paging, Home, End and positions
on both sides of 4 GiB. Fixtures contain Unicode and MiB-specific markers.

The table shows four highlighted views. Time values are medians of three
processes; memory values are the largest process peaks/sampled reservations.
The headless sink discards output. First text is timer-observed readiness, not
physical terminal presentation; End is observed navigation completion.

| Source | GiB | OS | First text, ms | End, ms | Peak resident, MiB | Sampled budget, MiB |
|---|---:|---|---:|---:|---:|---:|
| MARKDOWN gzip | 2 | linux | 105.4 | 5356.3 | 15.75 | 9.28 |
| MARKDOWN raw | 2 | linux | 106.0 | 166.8 | 15.88 | 8.77 |
| MARKDOWN gzip | 2 | windows | 81.8 | 1084.8 | 13.64 | 9.28 |
| MARKDOWN raw | 2 | windows | 82.0 | 93.2 | 13.04 | 8.77 |
| MARKDOWN gzip | 8 | linux | 108.1 | 21084.3 | 14.54 | 9.28 |
| MARKDOWN raw | 8 | linux | 105.0 | 165.8 | 14.38 | 8.77 |
| MARKDOWN gzip | 8 | windows | 80.3 | 3860.3 | 13.72 | 9.28 |
| MARKDOWN raw | 8 | windows | 83.5 | 93.2 | 13.17 | 8.77 |
| SQL gzip | 2 | linux | 105.9 | 5367.5 | 16.93 | 9.28 |
| SQL raw | 2 | linux | 107.3 | 166.0 | 16.66 | 8.77 |
| SQL gzip | 2 | windows | 82.8 | 1112.6 | 14.00 | 9.28 |
| SQL raw | 2 | windows | 81.5 | 92.4 | 13.57 | 8.77 |
| SQL gzip | 8 | linux | 105.5 | 21097.1 | 15.89 | 9.28 |
| SQL raw | 8 | linux | 107.2 | 206.1 | 14.84 | 8.77 |
| SQL gzip | 8 | windows | 83.6 | 3849.4 | 14.33 | 9.28 |
| SQL raw | 8 | windows | 82.3 | 92.4 | 13.59 | 8.77 |

[Complete one/four-view results and individual process metrics](measurements/document-qualification.json)
also retain native per-process phase percentiles and observation gaps. A maximum
of per-run p95 values is not a pooled p95. Empty/overflowed series are unavailable.

## What these measurements establish

The loaded document is not retained in full by these workloads. Process resident
memory and source reservations are separate measurements; Windows private commit
is additionally recorded, while Linux private-commit fields remain unavailable.
Kernel file-cache memory is outside these process counters. Budget peaks are
sampled; the shared root enforces its capacity separately.

Provider counters record admitted bytes, not physical disk traffic. Source I/O
on the UI thread and outstanding page/work/consumer credits are checked separately
from source-root destruction. No authoritative cache hit-rate counter is exposed;
the data does not infer one from repeated reads.

Gzip End can decode a large prefix before its extent is known. A bounded memory
footprint does not make this operation constant-time. AUTO text readiness also
does not mean exact syntax: distant windows may retain provisional colors after
the automatic refinement budget is exhausted. Explicit EXACT can reuse a qualified
checkpoint or scan a long prefix on a worker. The separate source-window oracle
checks actual text, source maps and exact roles at sampled anchors, rather than
declaring every byte of an unseen document verified.

The generated corpus is a controlled workload, not an exhaustive parser oracle.
CRLF/decoder boundaries, source replacement and cancellation additionally have
deterministic small regressions. Gigabyte fixtures are explicit local workloads;
the ordinary CTest uses small finite fixtures only.

## Very long lines and the slow End case {#document-qualification-long-lines}

These are separate single observations (`n=1`), with 262,144 sample slots and a
600-second whole-run deadline. The failed four-view WSL run remains a timeout;
it is not part of the 192 passing ordinary processes and is not a passing End test.

| OS | File, GiB | First line, GiB | Views | End, s | Peak resident, MiB |
|---|---:|---:|---:|---:|---:|
| linux | 1 | 1 | 1 | 334.007 | 15.30 |
| linux | 1 | 1 | 4 | timeout600 | 15.00 |
| linux | 8 | 1 | 1 | 0.083 | 15.50 |
| linux | 8 | 1 | 4 | 0.167 | 15.69 |
| windows | 1 | 1 | 1 | 126.568 | 12.87 |
| windows | 1 | 1 | 4 | 126.365 | 13.46 |
| windows | 8 | 1 | 1 | 0.092 | 12.70 |
| windows | 8 | 1 | 4 | 0.093 | 13.54 |

All these runs returned source credits and destroyed their roots, including the
timeout. First text remained available in roughly 64–107 ms. A 1 GiB first line
inside an 8 GiB file did not make its final short-record page slow. The troublesome
case ends inside that long line: reverse line-boundary search and forward layout
admit about 2 GiB through 8,193 worker turns for one End.

On the same Windows corpus and geometry, the separate worker oracle completed End
in 10.895 seconds; the Application observation took 126.568 seconds. An additional
instrumented Application run spent 127.850 of 128.407 seconds inside 8,264 existing
deadline reads. Worker work overlaps those waits, so the read sum is not a direct
measure of avoidable delay. It nevertheless exposes integration cost that UI paint
percentiles alone do not show. No timer-resolution setting or SDK code was changed.

Storage also matters. With the same frozen GCC binaries and identical generated
bytes, the worker End was 322.126 seconds on WSL/NTFS and 3.377 seconds on WSL ext4;
the ext4 Application End was still 84.019 seconds. These `n=1` controls separate
storage-path cost from host cadence; they are not a native Linux/Windows ranking.
The native terminal uses the same synchronous processing path, but its physical
latency and input wakeups were not measured by these headless observations.

The slow long-line End above is the original baseline; the updated profile is recorded earlier on this page. Bounded memory
and live UI timers do not imply fast completion of every navigation request.

## Exact role checks

Worker oracles checked generated 2/8 GiB SQL/Markdown, anchors around 4 GiB,
retained view states and sampled original-source roles. Explicit EXACT at 512 MiB
inside a 1 GiB SQL string verified 4,800 source maps with zero role mismatches:
34.756 seconds on Windows, 110.823 seconds on WSL/NTFS. The one-line Markdown
case also completed its exact role check. These are sampled lexical checks,
not SQL validation or proof of every document byte. Retained states are prepared
serially; the Application matrix separately exercises jointly active views.

The explicit 500 ms cancellation of an 8 GiB logical gzip source with four views
also released all source credits and destroyed the root. This intentional
cancellation is distinct from the failed 600-second long-line End profile.

## Larger existing SQL inputs

Two existing inputs were read in place on Windows, without copies or saved
contents. File identity, size and modification time matched before/after. These
are `n=1` observations with AUTO, not color-oracle checks for private documents.

| Source | GiB | Views | First text, ms | End, s | Peak resident, MiB |
|---|---:|---:|---:|---:|---:|
| SQL A | 7.42 | 1 | 83.0 | 4.645 | 13.00 |
| SQL A | 7.42 | 4 | 82.3 | 2.564 | 13.87 |
| SQL B | 38.70 | 1 | 84.1 | 4.839 | 13.17 |
| SQL B | 38.70 | 4 | 84.1 | 2.648 | 13.92 |

The generator's 8 GiB limit is a workload bound, not a Viewer file-size ceiling.
These observations establish opening/navigation for the tested files; they do
not promise constant-time seeking or complete syntax analysis of arbitrary data.
