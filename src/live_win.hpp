/*
  live_win.hpp
  The Windows glue shared by the two edge programs: the serial port, the
  RGB LED commands and the session timestamp. Both
  live.cpp (the self-contained local path) and edge_forward.cpp (the edge
  side of the edge->server path) open the same board at the same speed and
  drive the same LED with the same commands, so keeping this in one place
  prevents the baud rate and the LED protocol from drifting between
  the two programs. The analysis is shared through analysis.hpp for the
  same reason.

  Windows only: it is built into the two programs that talk to the board,
  never into the server, which has no serial port.
*/
#ifndef LIVE_WIN_HPP
#define LIVE_WIN_HPP

#include "analysis.hpp"   /* State, for send_led */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <windows.h>

/* The board is opened at the speed the sketch uses. */
inline HANDLE open_port(const std::string& port, int baud) {
  const std::string name = "\\\\.\\" + port;

  HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, 0, nullptr);

  if (h == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "cannot open %s "
                    "(wrong port, or the Serial Monitor is open?)\n",
            port.c_str());
    return nullptr;
  }

  /* a large receive buffer: the program briefly stops reading the port
     while it analyses a window or flushes the CSV, and the samples that
     arrive meanwhile must not overflow the driver buffer and be lost */
  SetupComm(h, 1 << 20, 4096);

  DCB dcb{};
  dcb.DCBlength = sizeof(dcb);
  GetCommState(h, &dcb);
  dcb.BaudRate = baud;
  dcb.ByteSize = 8;
  dcb.Parity   = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  SetCommState(h, &dcb);

  COMMTIMEOUTS t{};
  t.ReadIntervalTimeout = 50;
  t.ReadTotalTimeoutConstant = 200;
  SetCommTimeouts(h, &t);

  /* the board resets when the port is opened; wait for it to restart,
     then drop the bytes that arrived during the reset */
  Sleep(2000);
  PurgeComm(h, PURGE_RXCLEAR);

  return h;
}

inline void send_command(HANDLE h, const char *cmd) {
  DWORD written = 0;
  WriteFile(h, cmd, (DWORD)strlen(cmd), &written, nullptr);

  /*
    WriteFile returns once the bytes are queued in the driver, not once
    they have left the wire. Closing the port can drop whatever is
    still queued. This affects the LED,OFF sent just before the port
    is closed at the end of a session: without waiting for it to drain,
    the command never reaches the board and the LED keeps its last
    colour. FlushFileBuffers blocks until the transmit buffer is empty.
  */
  FlushFileBuffers(h);
}

inline void send_led(HANDLE h, State s) {
  const char *cmd = "LED,GREEN\n";

  if (s == State::PRE_ALERT) cmd = "LED,YELLOW\n";
  if (s == State::ALERT)     cmd = "LED,RED\n";

  send_command(h, cmd);
}

/*
  A session is identified by the moment it was acquired, so that two
  recordings never collide and the name does not encode the
  experiment's outcome.
*/
inline std::string timestamp() {
  const time_t now = time(nullptr);
  struct tm local;
  localtime_s(&local, &now);

  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);

  return std::string("session_") + stamp;
}

/*
  Opens a gnuplot window on the two files a live run writes, the samples
  and the per-window results, redrawn once a second. gnuplot draws only
  what has already been written and computes nothing; the thresholds are
  passed in so the plot and the analysis keep one definition. Used by both
  live paths, which write the same two file formats.
*/
inline void start_plot(const std::string& raw_path,
                       const std::string& win_path) {
  char cmd[600];

  snprintf(cmd, sizeof(cmd),
           "start \"TremorFSR live\" gnuplot -e \"raw='%s'; windows='%s'; "
           "pre=%.4f; alert=%.4f; rate=%.1f; wsize=%d; span=10.0\" "
           "scripts/plot_live.plt",
           raw_path.c_str(), win_path.c_str(),
           PRE_ALERT_LEVEL, ALERT_LEVEL, SAMPLING_RATE, WINDOW_SIZE);

  system(cmd);
}

#endif /* LIVE_WIN_HPP */
