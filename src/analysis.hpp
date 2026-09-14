/*
  analysis.hpp
  Signal analysis: one window and one session of windows.

  Both the live application and the benchmark call these functions, so
  there is a single implementation of the algorithm.
*/
#ifndef ANALYSIS_HPP
#define ANALYSIS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/* ---- configuration ---- */

constexpr double SAMPLING_RATE   = 500.0;   /* Hz                    */
constexpr int    WINDOW_SIZE     = 4096;    /* samples, 8.192 s      */
constexpr int    WINDOW_HOP      = 256;     /* samples, 0.512 s      */
constexpr int    SEGMENT_SIZE    = 2048;    /* Welch segment         */
constexpr int    SEGMENT_HOP     = 1024;    /* 50 % overlap          */
constexpr double BAND_LOW_HZ     = 4.0;
constexpr double BAND_HIGH_HZ    = 10.0;
constexpr double PRE_ALERT_LEVEL = 0.15;
constexpr double ALERT_LEVEL     = 0.30;
constexpr int    ALERT_WINDOWS   = 3;       /* consecutive, for ALERT */

constexpr int N_CHANNELS = 3;

/* ---- data ---- */

struct Sample {
  uint32_t id;
  double   time_s;
  float    fsr[N_CHANNELS];
};

enum class State { NORMAL, PRE_ALERT, ALERT };

const char *state_name(State s);

struct WindowResult {
  size_t window_id;
  double time_end_s;
  double dominant_hz[N_CHANNELS];
  double index;
  State  state;

  /*
    How long the analysis of this window took. The live application
    fills it, to show that the cost of a window stays below the
    interval at which windows arrive.
  */
  double processing_us;
};

struct Session {
  std::string         name;
  std::vector<Sample> samples;
};

struct SessionSummary {
  size_t windows;
  double mean_index;
  double max_index;
  size_t normal_count;
  size_t prealert_count;
  size_t alert_count;
  double dominant_hz;      /* median over the windows, first channel */
};

/*
  The decision carries state: ALERT needs ALERT_WINDOWS consecutive
  windows above the upper level. One instance per session, so that
  sessions analysed concurrently never share it.
*/
class DecisionState {
  int consecutive_high = 0;

public:
  State update(double index);
};

/* ---- Analysis ---- */

/*
  Analyse the window that begins at 'start' in the session. Fill the
  dominant frequencies and the index; the state is assigned by the
  caller, which owns the DecisionState.
*/
WindowResult analyze_window(const Session& session, size_t start,
                            size_t window_id);

/*
  Analyses one session from beginning to end, sequentially. This is the
  unit of work the farm distributes: a whole session per worker.
*/
SessionSummary analyze_session(const Session& session);

/* Reads "sample_id,time_ms,fsr1,fsr2,fsr3", skipping '#' comments. */
bool load_session(const std::string& path, Session& out);

#endif
