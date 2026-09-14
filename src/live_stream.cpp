/*
  live_stream.cpp
  Server side of the edge-to-server variant of the live path.

  The acquisition stays at the edge: the PC with the Arduino reads the
  sensors and streams the sample lines. This program runs on the remote
  machine, performs the same windowed analysis as the local live
  application and reports each window as it completes. It reads the
  samples from standard input, so the transport is not fixed. The edge
  program edge_forward opens the ssh connection, streams the samples to
  this process and reads the results back to drive the LED on the board:

      edge_forward COM5 60 i.gulino@ampere-1.unipi.it

  It is the same analyze_window() as live.cpp and benchmark.cpp, so the
  analysis exists in one place. It uses no Win32 and no serial API, so it
  builds and runs on the Linux/ARM machine.

  For each completed window it prints one CSV line, the same fields the
  local path records:

      window_id,time_end_s,dom1,dom2,dom3,index,state,processing_us,last_id

  last_id is the identifier of the window's last sample. The edge timed
  its reception, so from it the edge recovers the window-to-state latency.
  This program classifies each window into NORMAL, PRE_ALERT or ALERT with
  its own DecisionState, which holds the persistence ALERT requires; the
  edge uses the returned state only to drive the LED.
*/
#include "analysis.hpp"
#include "sample_io.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>

using clockt = std::chrono::steady_clock;

int main() {
  Session       session;
  DecisionState decision;

  size_t   next_start = 0, window_id = 0;
  long     received = 0, lost = 0;
  uint32_t last_id = 0;
  bool     have_last = false, board_reset = false;

  std::string line;

  while (std::getline(std::cin, line)) {
    if (line.empty() || line[0] == '#')
      continue;

    Sample s{};
    unsigned long t_ms = 0;

    if (!parse_sample(line, s, t_ms))
      continue;

    /* the identifier measures what did not arrive over the link; a
       backwards identifier means the board restarted mid-stream */
    if (have_last && s.id > last_id + 1)
      lost += (long)(s.id - last_id - 1);
    else if (have_last && s.id <= last_id)
      board_reset = true;

    last_id   = s.id;
    have_last = true;
    received++;

    session.samples.push_back(s);

    /* a window is complete: analyse it here, on the server */
    while (next_start + WINDOW_SIZE <= session.samples.size()) {
      const auto w0 = clockt::now();

      WindowResult r = analyze_window(session, next_start, window_id);

      r.processing_us =
        std::chrono::duration<double, std::micro>(clockt::now() - w0).count();
      r.state = decision.update(r.index);

      /* the identifier of this window's last sample: the edge timed its
         reception, so from it the edge recovers the window-to-state latency */
      const uint32_t win_last_id =
        session.samples[next_start + WINDOW_SIZE - 1].id;

      /* one CSV line per window, the same fields the local path records:
         window, end time, the three dominant frequencies, index, state,
         server processing time, last sample id */
      printf("%zu,%.3f,%.2f,%.2f,%.2f,%.6f,%s,%.0f,%u\n",
             window_id, r.time_end_s,
             r.dominant_hz[0], r.dominant_hz[1], r.dominant_hz[2],
             r.index, state_name(r.state), r.processing_us,
             (unsigned)win_last_id);
      fflush(stdout);

      next_start += WINDOW_HOP;
      window_id++;
    }
  }

  /* the summary goes to stderr so the sample stream on stdout stays clean */
  fprintf(stderr, "stream ended: %ld samples received, %ld lost, %zu windows\n",
          received, lost, window_id);
  if (board_reset)
    fprintf(stderr, "  the sample identifier went backwards: the board "
                    "restarted during the stream\n");
  return 0;
}
