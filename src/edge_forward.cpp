/*
  edge_forward.cpp
  Edge side of the edge-to-server variant of the live path: the
  acquisition and the LED stay at the edge and the analysis runs on a
  remote server.

  The board and the LED stay at the edge; the analysis moves to the
  server. This program reads the three sensors over the Win32 serial API,
  exactly as live.cpp does, but instead of analysing the samples itself it
  forwards each one across an ssh pipe to live_stream on the remote
  machine, which runs the same analyze_window() and returns one result
  line per window. Those results come back over the same pipe and drive
  the RGB LED on the board, so the loop is closed across the network:

      board -> edge -> ssh -> server analysis -> ssh -> edge -> LED

  The work is split over four threads, each with one job:

      serial  reads the serial port into a buffer, so no sample is lost
              while another thread is busy
      main    takes the buffered samples, stamps each one's reception
              time, records it and puts each sample line on a bounded queue
      writer  takes lines off the queue and writes them to the ssh pipe
      reader  reads the server's result lines from the ssh pipe, drives
              the LED and writes the per-window CSV

  The queue decouples acquisition from the network. The sensor produces
  samples at a fixed rate and cannot be paused; the network can. Writing
  to the network on the main thread would let a slow link stall the serial
  read and overflow the receive buffer, so the write runs on the writer
  thread and the samples wait on the queue meanwhile. The queue is bounded
  by a fixed number of samples, so a stalled link cannot grow the process
  without limit; a full queue drops the sample and the gap shows on the
  server as a loss by sample id.

  usage: edge_forward <COM port> <seconds> <ssh-dest> [remote-cmd] [-plot]
    e.g. edge_forward COM5 60 i.gulino@ampere-1.unipi.it

  The samples and the per-window results are written to CSV files in
  data/live/ and results/live/, in the same formats as the local path, so
  the run can be plotted live with -plot or re-analysed the same way. The
  only difference from the local path is that the analysis and the
  processing time recorded for each window come from the server.
*/
#include "analysis.hpp"   /* State, Sample */
#include "live_win.hpp"   /* open_port, send_command, send_led, timestamp, start_plot */
#include "sample_io.hpp"  /* parse_sample */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

using std::string;
using clockt = std::chrono::steady_clock;

/* the queue holds at most this many sample lines; at 500 Hz it is about
   ten seconds of acquisition, far more than a healthy link ever needs */
static const size_t MAX_QUEUE = 5000;

/*
  Window-to-state latency. The main thread stamps the wall-clock time at
  which it receives each sample from the serial reader (acq, keyed by
  sample id). The server returns, with each window's result, the id of
  that window's last sample. When the result comes back the reader
  subtracts the two, recovering the path for that window: reception at the
  edge of the last sample -> forwarding -> server analysis -> returned
  state. The mutex guards acq, which the main thread writes and the reader
  thread reads.
*/
struct E2E {
  std::mutex m;
  std::map<uint32_t, clockt::time_point> acq;
  std::vector<double> ms;
};

/*
  The queue between the main thread (producer) and the writer thread
  (consumer): a classic bounded producer-consumer queue.
*/
struct Forwarder {
  std::mutex              m;
  std::condition_variable cv;
  std::deque<string>      queue;
  bool stop    = false;   /* main asks the writer to flush and finish */
  long dropped = 0;       /* samples dropped because the queue was full */
};

/* the ssh child and the two pipe ends the parent keeps */
struct Link {
  HANDLE to_server   = nullptr;   /* parent writes samples here (child stdin)  */
  HANDLE from_server = nullptr;   /* parent reads results here  (child stdout) */
  PROCESS_INFORMATION pi{};
};

/*
  Writer thread: take sample lines off the queue and write them to the ssh
  pipe. Only this thread writes to the pipe. It sleeps on the condition
  variable until there is work or until stop is set, then drains what is
  there. On stop it finishes once the queue is empty.
*/
void writer_run(Forwarder& f, HANDLE pipe) {
  while (true) {
    std::unique_lock<std::mutex> lock(f.m);
    f.cv.wait(lock, [&] { return f.stop || !f.queue.empty(); });

    if (f.stop && f.queue.empty())
      break;

    string line = f.queue.front();
    f.queue.pop_front();
    lock.unlock();

    DWORD written = 0;
    WriteFile(pipe, line.c_str(), (DWORD)line.size(), &written, nullptr);
  }
}

/*
  Handle every complete line the server sent. A result line is a CSV line

      window_id,time_end_s,index,state,processing_us,last_id

  from which the reader drives the LED on a state change, recovers the
  window-to-state latency of the window and writes its result to the CSV in the
  same schema the local path uses. Any line that is not a result line (an
  ssh message, or the server's own summary) is echoed to the terminal.
*/
void process_server_lines(string& pending, std::ofstream& win,
                          State& last, HANDLE port, E2E& e2e) {
  size_t nl;
  while ((nl = pending.find('\n')) != string::npos) {
    string line = pending.substr(0, nl);
    pending.erase(0, nl + 1);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    size_t        window_id = 0;
    double        time_end = 0, d1 = 0, d2 = 0, d3 = 0;
    double        index = 0, processing_us = 0;
    char          state[16] = {0};
    unsigned long last_id = 0;

    if (sscanf(line.c_str(), "%zu,%lf,%lf,%lf,%lf,%lf,%15[^,],%lf,%lu",
               &window_id, &time_end, &d1, &d2, &d3, &index, state,
               &processing_us, &last_id) == 9) {
      /* drive the LED on a state change */
      State st = State::NORMAL;
      if      (string(state) == "PRE_ALERT") st = State::PRE_ALERT;
      else if (string(state) == "ALERT")     st = State::ALERT;
      if (st != last) {
        send_led(port, st);
        last = st;
      }

      /* the state has just arrived: recover the window-to-state latency of
         this window from the reception time of its last sample */
      double e2e_ms = -1.0;
      {
        std::lock_guard<std::mutex> lk(e2e.m);
        auto it = e2e.acq.find((uint32_t)last_id);
        if (it != e2e.acq.end()) {
          e2e_ms = std::chrono::duration<double, std::milli>(
                     clockt::now() - it->second).count();
          e2e.ms.push_back(e2e_ms);
          /* windows advance, so earlier samples are no longer needed */
          e2e.acq.erase(e2e.acq.begin(),
                        e2e.acq.upper_bound((uint32_t)last_id));
        }
      }

      win << window_id << "," << time_end << "," << d1 << "," << d2 << ","
          << d3 << "," << index << "," << state << "," << processing_us << ",";
      if (e2e_ms >= 0.0) win << e2e_ms;
      win << "\n";
      win.flush();

      printf("win %4zu  index=%.3f  %-9s  server_proc=%.0f us",
             window_id, index, state, processing_us);
      if (e2e_ms >= 0.0)
        printf("  w2s=%.0f ms", e2e_ms);
      printf("\n");
    } else {
      printf("[server] %s\n", line.c_str());
    }
  }
  fflush(stdout);
}

/*
  Reader thread: read the server's output from the ssh pipe until it
  closes, handling each complete line. When the server finishes it closes
  its output, ReadFile then returns end of file and the thread stops.
*/
void reader_run(HANDLE from_server, std::ofstream& win, HANDLE port,
                E2E& e2e) {
  State  last = State::NORMAL;
  string pending;
  char   buffer[4096];

  while (true) {
    DWORD got = 0;
    if (!ReadFile(from_server, buffer, sizeof(buffer), &got, nullptr) ||
        got == 0)
      break;
    pending.append(buffer, got);
    process_server_lines(pending, win, last, port, e2e);
  }
}

/*
  Start ssh as a child process with its standard input and output
  redirected to two pipes. Samples are written to to_server; the results
  the server prints come back on from_server. The child's stderr is sent
  to the same output pipe, so an ssh message is seen in the same stream.
*/
bool spawn_ssh(const string& dest, const string& remote_cmd, Link& link) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE in_rd = nullptr, in_wr = nullptr;    /* child stdin : child reads in_rd  */
  HANDLE out_rd = nullptr, out_wr = nullptr;  /* child stdout: child writes out_wr */

  if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) {
    fprintf(stderr, "cannot create the pipe to ssh\n");
    return false;
  }
  if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) {
    fprintf(stderr, "cannot create the pipe from ssh\n");
    CloseHandle(in_rd);
    CloseHandle(in_wr);
    return false;
  }

  /* the ends the parent keeps must not be inherited by the child */
  SetHandleInformation(in_wr,  HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags    = STARTF_USESTDHANDLES;
  si.hStdInput  = in_rd;
  si.hStdOutput = out_wr;
  si.hStdError  = out_wr;

  /* CreateProcessA needs a writable command-line buffer */
  string cmd = "ssh " + dest + " \"" + remote_cmd + "\"";
  std::vector<char> buf(cmd.begin(), cmd.end());
  buf.push_back('\0');

  BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                           0, nullptr, nullptr, &si, &link.pi);

  /* the child owns its ends now; the parent closes its copies so that
     end of file propagates in both directions when either side finishes */
  CloseHandle(in_rd);
  CloseHandle(out_wr);

  if (!ok) {
    fprintf(stderr, "cannot start ssh (is OpenSSH on PATH?)\n");
    CloseHandle(in_wr);
    CloseHandle(out_rd);
    return false;
  }

  link.to_server   = in_wr;
  link.from_server = out_rd;
  return true;
}

/*
  The serial reader. It does nothing but read the port into a buffer, so
  the incoming samples are never lost while the main thread is busy
  parsing, writing the CSV or waiting on the disk that the live plot is
  also reading. The samples arrive at a fixed rate and the port cannot be
  paused, so, like the network write, the read runs on its own thread and
  the rest of the program cannot starve it.
*/
struct Serial {
  std::mutex  m;
  std::string data;
  bool        stop = false;
};

void serial_read(HANDLE port, Serial& ser) {
  char buf[4096];
  while (true) {
    {
      std::lock_guard<std::mutex> lk(ser.m);
      if (ser.stop) break;
    }
    DWORD got = 0;
    if (!ReadFile(port, buf, sizeof(buf), &got, nullptr))
      break;
    if (got > 0) {
      std::lock_guard<std::mutex> lk(ser.m);
      ser.data.append(buf, (size_t)got);
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc < 4) {
    printf("usage: edge_forward <COM port> <seconds> <ssh-dest> "
           "[remote-cmd] [-plot]\n\n");
    printf("  reads the three sensors and forwards each sample over an "
           "ssh pipe\n");
    printf("  to the server, which runs the same window analysis; the "
           "results it\n");
    printf("  returns drive the RGB LED on the board.\n\n");
    printf("  ssh-dest    e.g. i.gulino@ampere-1.unipi.it\n");
    printf("  remote-cmd  default ~/TremorFSR/live_stream\n");
    printf("  -plot       open a gnuplot window on the two files\n\n");
    printf("  it writes the same two files as the local path:\n");
    printf("    data/live/<session>_edge.csv             the samples\n");
    printf("    results/live/<session>_edge_windows.csv  the results\n");
    return 0;
  }

  const string port    = argv[1];
  const double seconds = atof(argv[2]);
  const string dest    = argv[3];

  if (seconds <= 0.0) {
    fprintf(stderr, "the duration must be a positive number of seconds "
                    "(got \"%s\")\n", argv[2]);
    return 1;
  }

  /* the optional 4th argument is the remote command; -plot may appear
     anywhere after the destination */
  string remote = "~/TremorFSR/live_stream";
  bool   plot   = false;
  for (int i = 4; i < argc; i++) {
    if (string(argv[i]) == "-plot" || string(argv[i]) == "--plot") plot = true;
    else                             remote = argv[i];
  }

  const string base     = timestamp();
  const string raw_path = "data/live/" + base + "_edge.csv";
  const string win_path = "results/live/" + base + "_edge_windows.csv";

  HANDLE port_handle = open_port(port, 250000);
  if (!port_handle)
    return 1;

  Link link;
  if (!spawn_ssh(dest, remote, link)) {
    send_command(port_handle, "LED,OFF\n");
    CloseHandle(port_handle);
    return 1;
  }

  std::ofstream raw(raw_path);
  std::ofstream win(win_path);
  if (!raw || !win) {
    fprintf(stderr, "cannot write the session files "
                    "(do data/live and results/live exist?)\n");
    send_command(port_handle, "LED,OFF\n");
    CloseHandle(port_handle);
    return 1;
  }
  raw << "sample_id,time_ms,fsr1,fsr2,fsr3\n";
  win << "window_id,time_end_s,dominant_f1_hz,dominant_f2_hz,"
         "dominant_f3_hz,index,state,processing_us,e2e_ms\n";

  printf("port %s at 250000 baud, %.0f s\n", port.c_str(), seconds);
  printf("server   -> ssh %s \"%s\"\n", dest.c_str(), remote.c_str());
  printf("samples  -> %s\n", raw_path.c_str());
  printf("results  -> %s\n\n", win_path.c_str());

  /* flush the headers so the files exist on disk before gnuplot opens
     them: on a second back-to-back run the plot could otherwise read an
     empty file and fail to start */
  raw.flush();
  win.flush();

  if (plot)
    start_plot(raw_path, win_path);

  E2E       e2e;
  Forwarder fwd;

  /* the board leaves the LED dark at reset; send the initial state once,
     as live.cpp does, so a run that stays NORMAL still lights the LED */
  send_led(port_handle, State::NORMAL);

  /* clear whatever arrived during the slow startup (the board reset, the
     ssh connection, the plot launch): the driver buffer may have filled
     and overflowed meanwhile, so purging here makes the acquisition start
     from a clean buffer with no gap in the first samples */
  PurgeComm(port_handle, PURGE_RXCLEAR);

  /* the serial reader fills a buffer; the writer sends the queue to the
     server; the reader brings the results back and drives the LED and the
     CSV */
  Serial ser;
  std::thread serial(serial_read, port_handle, std::ref(ser));
  std::thread writer(writer_run, std::ref(fwd), link.to_server);
  std::thread reader(reader_run, link.from_server, std::ref(win),
                     port_handle, std::ref(e2e));

  string ser_pending;   /* serial bytes waiting to become whole lines */
  long   recorded  = 0; /* samples acquired and written locally */
  long   forwarded = 0; /* samples put on the queue for the server */

  const auto t0 = clockt::now();

  /* ---- main thread: take what the serial reader has buffered, record it
     and enqueue it for the server ---- */
  while (std::chrono::duration<double>(clockt::now() - t0).count() < seconds) {
    string chunk;
    {
      std::lock_guard<std::mutex> lk(ser.m);
      chunk.swap(ser.data);
    }
    if (chunk.empty()) {
      Sleep(2);
      continue;
    }

    /* the moment the samples were taken from the reader: the reception
       time used later for the window-to-state latency */
    const auto rx_now = clockt::now();
    ser_pending += chunk;

    size_t nl;
    while ((nl = ser_pending.find('\n')) != string::npos) {
      string line = ser_pending.substr(0, nl);
      ser_pending.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      /* accept the line only when it parses as a complete sample, that is
         all five fields: this discards a partial first line, a header or
         any noise before the sample is recorded or forwarded */
      Sample        s{};
      unsigned long t_ms = 0;
      if (!parse_sample(line, s, t_ms))
        continue;

      /* stamp this sample's reception time, keyed by its id */
      {
        std::lock_guard<std::mutex> lk(e2e.m);
        e2e.acq[s.id] = rx_now;
      }

      /* record every acquired sample locally */
      raw << line << "\n";
      recorded++;
      if (recorded % 250 == 0)
        raw.flush();

      /* hand the sample to the writer thread through the bounded queue */
      {
        std::lock_guard<std::mutex> lk(fwd.m);
        if (fwd.queue.size() >= MAX_QUEUE) {
          fwd.dropped++;
        } else {
          fwd.queue.push_back(line + "\n");
          forwarded++;
        }
      }
      fwd.cv.notify_one();
    }
  }

  /* ---- shutdown: stop the serial reader and the writer, then close the
     pipe so the server sees end of file and the reader drains its last
     results ---- */
  {
    std::lock_guard<std::mutex> lk(ser.m);
    ser.stop = true;
  }
  serial.join();

  {
    std::lock_guard<std::mutex> lk(fwd.m);
    fwd.stop = true;
  }
  fwd.cv.notify_one();
  writer.join();

  CloseHandle(link.to_server);   /* the server sees end of file on its input */
  link.to_server = nullptr;

  reader.join();                 /* the reader stops when the server closes */

  /* the session is over: the LED should not keep reporting a state */
  send_command(port_handle, "LED,OFF\n");
  Sleep(100);   /* let the board receive and act on the command before
                   the port is closed, so the LED goes dark */
  CloseHandle(port_handle);

  raw.flush();
  win.flush();

  CloseHandle(link.from_server);
  CloseHandle(link.pi.hProcess);
  CloseHandle(link.pi.hThread);

  printf("\nacquired %ld samples, forwarded %ld to the server, "
         "%ld dropped (queue full)\n", recorded, forwarded, fwd.dropped);
  printf("samples saved to %s\n", raw_path.c_str());
  printf("results saved to %s\n", win_path.c_str());

  /* the window-to-state latency of the remote path, per window: the time
     from the reception at the edge of a window's last sample to the arrival
     of its state back at the edge. Reported as a distribution (median and
     percentiles),
     not only a mean, because a network latency is not well summarised by
     its average. The local path cannot make this measurement. */
  if (!e2e.ms.empty()) {
    std::vector<double> v = e2e.ms;
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) {
      return v[(size_t)(p * (double)(v.size() - 1) + 0.5)];
    };
    double sum = 0.0;
    for (double x : v) sum += x;
    printf("\nwindow-to-state latency over %zu windows "
           "(reception of last sample -> returned state), ms:\n"
           "  min %.1f  median %.1f  mean %.1f  p90 %.1f  p99 %.1f  max %.1f\n",
           v.size(), v.front(), pct(0.50), sum / (double)v.size(),
           pct(0.90), pct(0.99), v.back());
  }

  return 0;
}
