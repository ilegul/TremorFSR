/*
  live.cpp
  Three force sensors on an Arduino, analysed while they are being
  acquired, with the state shown on the terminal and on the RGB LED.

  It is a sequential loop. One window costs less than the interval at
  which windows arrive, so no parallel runtime is used here; the farm
  is in benchmark.cpp, on a workload with the granularity for it.

  The serial port is opened through the Win32 API, so this program is
  built on Windows. A session it records is an ordinary CSV file and
  can be re-analysed anywhere.
*/
#include "analysis.hpp"
#include "live_win.hpp"
#include "sample_io.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include <windows.h>

using std::string;
using clockt = std::chrono::steady_clock;

int main(int argc, char *argv[]) {
  if (argc < 3) {
    printf("usage: live <COM port> <seconds> [-plot]\n\n");
    printf("  reads the three sensors for the given time, analysing each\n");
    printf("  window as it completes and writing two files:\n");
    printf("    data/live/<session>.csv               the samples\n");
    printf("    results/live/<session>_windows.csv    the results\n\n");
    printf("  -plot opens a gnuplot window on those two files\n");
    return 0;
  }

  const string port = argv[1];
  const double seconds = atof(argv[2]);
  if (seconds <= 0.0) {
    fprintf(stderr, "the duration must be a positive number of seconds "
                    "(got \"%s\")\n", argv[2]);
    return 1;
  }
  bool plot = false;

  for (int i = 3; i < argc; i++)
    if (string(argv[i]) == "-plot" || string(argv[i]) == "--plot")
      plot = true;

  const string base = timestamp();
  const string raw_path = "data/live/" + base + ".csv";
  const string win_path = "results/live/" + base + "_windows.csv";

  HANDLE port_handle = open_port(port, 250000);

  if (!port_handle)
    return 1;

  std::ofstream raw(raw_path);
  std::ofstream win(win_path);

  if (!raw || !win) {
    fprintf(stderr, "cannot write the session files "
                    "(do data/live and results/live exist?)\n");
    return 1;
  }

  raw << "sample_id,time_ms,fsr1,fsr2,fsr3\n";
  win << "window_id,time_end_s,dominant_f1_hz,dominant_f2_hz,"
         "dominant_f3_hz,index,state,processing_us\n";

  printf("port %s at 250000 baud, %.0f s\n", port.c_str(), seconds);
  printf("samples  -> %s\n", raw_path.c_str());
  printf("results  -> %s\n", win_path.c_str());
  printf("first window after %.3f s, then one result every %.3f s\n\n",
         WINDOW_SIZE / SAMPLING_RATE, WINDOW_HOP / SAMPLING_RATE);

  /* flush the headers so the files exist on disk before gnuplot opens
     them: on a second back-to-back run the plot could otherwise read an
     empty file and fail to start */
  raw.flush();
  win.flush();

  if (plot)
    start_plot(raw_path, win_path);

  /*
    The samples accumulate into a Session, which is what the
    benchmark analyses: the live path calls the same analyze_window on
    the same structure, so the analysis exists in one place only.
  */
  Session session;
  session.name = base;

  DecisionState decision;
  State last_state = State::NORMAL;

  /*
    The board leaves the LED dark at reset. A command is sent only
    when the state changes, so the initial state is sent once here.
    Without it a session that stays NORMAL would never light the LED.
  */
  send_led(port_handle, State::NORMAL);

  /* clear whatever arrived during the slow startup (the board reset and
     the plot launch) so the acquisition starts from a clean buffer */
  PurgeComm(port_handle, PURGE_RXCLEAR);

  size_t next_start = 0, window_id = 0;
  long received = 0, lost = 0;
  uint32_t last_id = 0;
  bool have_last = false, board_reset = false;

  string pending;
  char buffer[4096];

  const auto t0 = clockt::now();

  while (std::chrono::duration<double>(clockt::now() - t0).count() < seconds) {
    DWORD got = 0;

    if (!ReadFile(port_handle, buffer, sizeof(buffer), &got, nullptr))
      break;

    pending.append(buffer, got);

    size_t nl;

    while ((nl = pending.find('\n')) != string::npos) {
      const string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);

      Sample s{};
      unsigned long t_ms = 0;

      if (!parse_sample(line, s, t_ms))
        continue;

      /*
        The board numbers the samples it produces, so a gap in the
        identifier counts exactly how many did not arrive. An
        identifier that fails to advance indicates the board restarted
        mid-session. The recording then spans two runs of the sketch -
        a session to discard, like one with losses.
      */
      if (have_last && s.id > last_id + 1)
        lost += (long)(s.id - last_id - 1);
      else if (have_last && s.id <= last_id)
        board_reset = true;

      last_id = s.id;
      have_last = true;
      received++;

      session.samples.push_back(s);

      raw << s.id << "," << t_ms << "," << (int)s.fsr[0] << ","
          << (int)s.fsr[1] << "," << (int)s.fsr[2] << "\n";

      if (received % 250 == 0)
        raw.flush();

      /* ---- a window is complete: analyse it now ---- */
      while (next_start + WINDOW_SIZE <= session.samples.size()) {
        const auto w0 = clockt::now();

        WindowResult r = analyze_window(session, next_start, window_id);

        r.processing_us =
          std::chrono::duration<double, std::micro>(
            clockt::now() - w0).count();

        r.state = decision.update(r.index);

        win << r.window_id << "," << r.time_end_s << ","
            << r.dominant_hz[0] << "," << r.dominant_hz[1] << ","
            << r.dominant_hz[2] << "," << r.index << ","
            << state_name(r.state) << "," << r.processing_us << "\n";
        win.flush();

        printf("win %4zu  t=%6.1fs  dom=[%5.1f %5.1f %5.1f] Hz  "
               "index=%.3f  %-9s  %6.0f us\n",
               r.window_id, r.time_end_s,
               r.dominant_hz[0], r.dominant_hz[1], r.dominant_hz[2],
               r.index, state_name(r.state), r.processing_us);

        if (r.state != last_state) {
          send_led(port_handle, r.state);

          if (r.state != State::NORMAL)
            printf("\n  *** %s ***\n\n", state_name(r.state));

          last_state = r.state;
        }

        next_start += WINDOW_HOP;
        window_id++;
      }
    }
  }

  /* the session is over: the LED should not keep reporting a state */
  send_command(port_handle, "LED,OFF\n");
  Sleep(100);   /* let the board receive and act on the command before
                   the port is closed, so the LED goes dark */
  CloseHandle(port_handle);

  raw.flush();
  win.flush();

  printf("\nsamples received %ld, lost %ld (%.2f %%)\n",
         received, lost,
         received + lost > 0 ? 100.0 * lost / (received + lost) : 0.0);
  printf("windows analysed %zu\n", window_id);

  /*
    A lost sample is not visible to the analysis: the samples that did
    arrive are packed together, so the window that spans the gap covers
    more time than it appears to and its spectrum is distorted.
    Interpolation is not used. A session is used as a result only when
    nothing was lost; otherwise it is repeated.
  */
  if (lost > 0)
    printf("\n  this session lost samples: the analysis assumes a "
           "uniform 500 Hz,\n  so repeat it before using it as a "
           "result\n");

  if (board_reset)
    printf("\n  the sample identifier went backwards: the board "
           "restarted during\n  this session, so repeat it before "
           "using it as a result\n");
  printf("the recording can be re-analysed from %s\n", raw_path.c_str());

  return 0;
}
