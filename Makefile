# TremorFSR
#
#   make benchmark        the FastFlow study         (Linux / WSL2)
#   make benchmark_trace  the same, with ffStats     (Linux / WSL2)
#   make live             the local acquisition      (Windows, MinGW)
#   make edge_forward     the edge side, edge->server (Windows, MinGW)
#   make live_stream      the server side, edge->server (Linux / ARM)
#   make clean
#
# FastFlow is header only, so it needs only an include path.

CXX      = g++
FF_PATH ?= /usr/local/fastflow
CXXFLAGS = -std=c++20 -O3 -Wall -pthread -fopenmp -I$(FF_PATH)

benchmark: src/analysis.cpp src/benchmark.cpp src/analysis.hpp src/utimer.hpp
	$(CXX) $(CXXFLAGS) src/analysis.cpp src/benchmark.cpp -o $@

# ffStats reports nothing unless the runtime is built with tracing.
# Tracing costs time, so the timings and the profile are two binaries.
benchmark_trace: src/analysis.cpp src/benchmark.cpp src/analysis.hpp src/utimer.hpp
	$(CXX) $(CXXFLAGS) -DTRACE_FASTFLOW \
	    src/analysis.cpp src/benchmark.cpp -o $@

# Same benchmark with FastFlow's default thread pinning disabled
# (-DNO_DEFAULT_MAPPING): the OS is left free to place and migrate the
# threads. Used to compare mapping strategies on the many-core machine -
# default pinning vs OS-scheduled vs core 0 excluded (via taskset).
benchmark_nomap: src/analysis.cpp src/benchmark.cpp src/analysis.hpp src/utimer.hpp
	$(CXX) $(CXXFLAGS) -DNO_DEFAULT_MAPPING \
	    src/analysis.cpp src/benchmark.cpp -o $@

# The acquisition uses the Win32 serial API, so it is built on Windows.
# It needs no FastFlow: it is a sequential loop.
live: src/analysis.cpp src/live.cpp src/analysis.hpp src/live_win.hpp src/sample_io.hpp
	$(CXX) -std=c++20 -O3 -Wall -static \
	    src/analysis.cpp src/live.cpp -o $@

# The edge side of the edge->server live variant: it reads the sensors
# over the Win32 serial API, forwards the samples to the server across an
# ssh pipe and drives the LED from the states the server sends back. Built
# on Windows like live; it runs no analysis itself, so it links neither
# analysis.cpp nor FastFlow. It forwards on a separate thread, so it needs
# the threading runtime (-pthread), statically linked here.
edge_forward: src/edge_forward.cpp src/analysis.hpp src/live_win.hpp src/sample_io.hpp
	$(CXX) -std=c++20 -O3 -Wall -static -pthread \
	    src/edge_forward.cpp -o $@

# The server side of the edge->server live variant. Reads samples from
# stdin (an ssh pipe from the edge), so it needs neither the serial API
# nor FastFlow. It builds on the remote Linux/ARM machine.
live_stream: src/analysis.cpp src/live_stream.cpp src/analysis.hpp src/sample_io.hpp
	$(CXX) -std=c++20 -O3 -Wall -pthread \
	    src/analysis.cpp src/live_stream.cpp -o $@

clean:
	rm -f benchmark benchmark_trace benchmark_nomap live edge_forward \
	    live_stream *.exe

.PHONY: clean
