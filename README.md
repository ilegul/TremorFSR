# TremorFSR

Three force-sensitive resistors on an Arduino Due, analysed on a host:
the applied force is examined for a rhythmic component and the result
is reported while the acquisition is running.

The project has two halves that share one implementation of the
analysis.

```text
live       sensors -> serial -> analysis -> index -> state -> LED
benchmark  independent recorded sessions -> FastFlow farm -> throughput
```

`live` shows the chain end to end and that a session is analysed at the
rate the stream imposes. `benchmark` asks a different question: how much
throughput a farm adds when several independent sessions have to be
analysed. Both call the same `analyze_window()`.

## Build

The benchmark is built on Linux, under WSL2 or in a container, where
FastFlow is available:

```bash
make benchmark
make benchmark_trace
```

`benchmark_trace` is the same program with FastFlow's tracing enabled,
which is what makes `ffStats` report anything; it is separate because
tracing costs time and the timings are taken without it. Both take the
FastFlow include path from `FF_PATH`.

The acquisition is built on Windows, since the serial port is opened
through the Win32 API:

```powershell
g++ -std=c++20 -O3 -Wall -static src/analysis.cpp src/live.cpp -o live.exe
```

## Running a session

```powershell
.\live.exe COM9 60 -plot
```

Reads the sensors for sixty seconds. The first window needs 4096/500 =
8.192 s of signal; from then on a result appears every 256/500 =
0.512 s. Two files are written during the same execution:

```text
data/live/session_<timestamp>.csv            the samples received
results/live/session_<timestamp>_windows.csv the results computed online
```

The second is written as the windows complete, not reconstructed
afterwards; it records the time each analysis took alongside its
result. `-plot` opens a gnuplot window on those two files: the three
channels over the last ten seconds above, the index with its two
thresholds below. The state appears on the terminal and on the LED.

At the end the program reports how many samples arrived and how many
were lost, counted from the gaps in the identifier the board assigns.
The analysis assumes a uniform 500 Hz, so a session that lost samples
is repeated rather than corrected: only a session with none is used as
a result.

## Running the benchmark

```bash
./benchmark [datadir] [n_jobs] [workers_csv] [runs]
```

Loads the three synthetic recordings, builds `n_jobs` independent
analysis jobs from them and times the sequential baseline, the
pipeline with one Compute node, the farm and an OpenMP `parallel for`
for each worker count, checking the summaries against the sequential
reference after every run. With no arguments it runs the small default
campaign (24 jobs, 2 and 4 workers, 7 runs); the reported campaign was

```bash
./benchmark data/synthetic 256 1,2,4,8,16,32,64,128,256 7
```

on a 256-core ARM machine. It writes `results/benchmark.csv`.

```bash
./benchmark_trace ...
```

The same, with `ffStats` at the end: it shows where the work of the
pipeline actually is, which is what selects the node to replicate.

`make benchmark_nomap` builds the benchmark with FastFlow's default
thread pinning disabled (`-DNO_DEFAULT_MAPPING`). A fifth argument
`farm` times only the farm sweep, so the thread-mapping strategies can
be compared without re-measuring the baseline they do not affect:

```bash
./benchmark        data/synthetic 256 64,128,256 7 farm   # default pinning
./benchmark_nomap  data/synthetic 256 64,128,256 7 farm   # OS-scheduled
taskset -c 1-255 ./benchmark_nomap data/synthetic 256 64,128,256 7 farm  # core 0 excluded
```

The live path comes in two forms: the analysis runs either beside the
acquisition, as above, or on a remote server. In the edge-to-server
form the samples are streamed to the analysis on the remote machine, so
the effect of moving the analysis across the network can be compared.
The server side is built on the remote machine; the edge program opens
the ssh connection itself, so it takes the serial port, a duration and
the ssh destination (with an optional remote command and `-plot`):

```bash
make live_stream                                    # on the server
make edge_forward                                   # on the PC
edge_forward.exe COM9 60 <user>@<host> [remote-cmd] [-plot]
```

## Figures

```bash
gnuplot scripts/plot_scaling.plt   # speedup vs workers, farm and OpenMP
```

## Layout

```text
src/analysis.{hpp,cpp}   the analysis: windows, Welch, FFT, index, decision
src/live.cpp             the acquisition and the online loop
src/benchmark.cpp        the baseline, the pipeline, the farm, the OpenMP for
src/live_stream.cpp      edge-to-server form: analysis on the remote machine
arduino/TremorFSR/       the sketch: samples three channels, drives the LED
data/synthetic/          generated recordings, with a known answer
data/live/               recorded sessions
results/                 what the two programs write
```
