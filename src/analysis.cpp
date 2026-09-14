/*
  analysis.cpp
  Mean removal, Welch spectral estimate over a radix-2 FFT, the band
  index and the decision.
*/
#include "analysis.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <fstream>
#include <sstream>

using std::complex;
using std::vector;

namespace {

constexpr double PI = 3.14159265358979323846;

/*
  Iterative radix-2 Cooley-Tukey FFT, in place. The recursive form
  splits even and odd indices; doing that repeatedly sends every
  element to the position given by reversing the bits of its index, so
  the iterative form permutes once and then combines bottom-up in
  log2(n) stages of butterflies.

  Standard algorithm (Cooley & Tukey, "An algorithm for the machine
  calculation of complex Fourier series", Math. Comp. 19, 1965). This
  in-place iterative form follows the implementation at
  CP-Algorithms: https://cp-algorithms.com/algebra/fft.html
*/
void fft(vector<complex<double>>& a) {
  const size_t n = a.size();

  for (size_t i = 1, j = 0; i < n; i++) {
    size_t bit = n >> 1;

    for (; j & bit; bit >>= 1)
      j ^= bit;

    j ^= bit;

    if (i < j)
      std::swap(a[i], a[j]);
  }

  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * PI / (double)len;
    const complex<double> wl(cos(ang), sin(ang));

    for (size_t i = 0; i < n; i += len) {
      complex<double> w(1.0, 0.0);

      for (size_t k = 0; k < len / 2; k++) {
        const complex<double> u = a[i + k];
        const complex<double> v = a[i + k + len / 2] * w;

        a[i + k]             = u + v;
        a[i + k + len / 2]   = u - v;
        w *= wl;
      }
    }
  }
}

/*
  The Hann coefficients depend only on the segment length, so they are
  built once. thread_local because sessions are analysed concurrently.
  Hann window, w[i] = 0.5 (1 - cos(2*pi*i/(N-1))); see the "Hann and
  Hamming windows" section of the window-function article on Wikipedia,
  https://en.wikipedia.org/wiki/Window_function
*/
const vector<double>& hann() {
  static thread_local vector<double> w;

  if (w.empty()) {
    w.resize(SEGMENT_SIZE);

    for (int i = 0; i < SEGMENT_SIZE; i++)
      w[i] = 0.5 * (1.0 - cos(2.0 * PI * i / (SEGMENT_SIZE - 1)));
  }

  return w;
}

/* Scratch buffers, one set per thread, reused across windows. */
struct Workspace {
  vector<complex<double>> seg;
  vector<double>          psd;
  vector<double>          x;

  Workspace()
    : seg(SEGMENT_SIZE), psd(SEGMENT_SIZE / 2 + 1), x(WINDOW_SIZE) {}
};

Workspace& workspace() {
  static thread_local Workspace w;
  return w;
}

/*
  A bin is counted when its centre lies inside the stated band, so the
  lower edge rounds up and the upper rounds down. At 500 Hz with
  segments of 2048 the band 4-10 Hz is bins 17 to 40.
*/
int band_low_bin() {
  return (int)ceil(BAND_LOW_HZ * SEGMENT_SIZE / SAMPLING_RATE);
}

int band_high_bin() {
  return (int)floor(BAND_HIGH_HZ * SEGMENT_SIZE / SAMPLING_RATE);
}

/*
  Welch: the window is split into overlapping segments, each is tapered
  and transformed and the periodograms are averaged. Averaging
  stabilises the estimate so a threshold can be applied to it.

  Welch's method (P. D. Welch, "The use of fast Fourier transform for
  the estimation of power spectra: a method based on time averaging over
  short, modified periodograms", IEEE Trans. Audio Electroacoust. 15(2),
  1967, pp. 70-73); overview: https://en.wikipedia.org/wiki/Welch%27s_method
*/
void welch(const vector<double>& x, vector<double>& psd, Workspace& ws) {
  const int bins  = SEGMENT_SIZE / 2 + 1;
  const int nseg  = (WINDOW_SIZE - SEGMENT_SIZE) / SEGMENT_HOP + 1;
  const vector<double>& taper = hann();

  std::fill(psd.begin(), psd.begin() + bins, 0.0);

  for (int s = 0; s < nseg; s++) {
    const int off = s * SEGMENT_HOP;

    for (int i = 0; i < SEGMENT_SIZE; i++)
      ws.seg[i] = complex<double>(x[off + i] * taper[i], 0.0);

    fft(ws.seg);

    for (int k = 0; k < bins; k++) {
      const double re = ws.seg[k].real();
      const double im = ws.seg[k].imag();
      psd[k] += (re * re + im * im) / SEGMENT_SIZE;
    }
  }

  for (int k = 0; k < bins; k++)
    psd[k] /= nseg;
}

}  /* namespace */

const char *state_name(State s) {
  switch (s) {
    case State::PRE_ALERT: return "PRE_ALERT";
    case State::ALERT:     return "ALERT";
    default:               return "NORMAL";
  }
}

State DecisionState::update(double index) {
  if (index >= ALERT_LEVEL)
    consecutive_high++;
  else
    consecutive_high = 0;

  if (consecutive_high >= ALERT_WINDOWS)
    return State::ALERT;

  if (index >= PRE_ALERT_LEVEL)
    return State::PRE_ALERT;

  return State::NORMAL;
}

WindowResult analyze_window(const Session& session, size_t start,
                            size_t window_id) {
  Workspace& ws = workspace();
  const int bins = SEGMENT_SIZE / 2 + 1;
  const int k_lo = band_low_bin();
  const int k_hi = band_high_bin();

  WindowResult r{};
  r.window_id  = window_id;
  r.time_end_s = (double)(start + WINDOW_SIZE) / SAMPLING_RATE;
  r.state      = State::NORMAL;

  double index_sum = 0.0;

  for (int c = 0; c < N_CHANNELS; c++) {
    /*
      The static part of the applied force exceeds the oscillation by
      orders of magnitude and would dominate bin 0, so the mean of the
      window is removed before anything else.
    */
    double mean = 0.0;

    for (int i = 0; i < WINDOW_SIZE; i++)
      mean += session.samples[start + i].fsr[c];

    mean /= WINDOW_SIZE;

    for (int i = 0; i < WINDOW_SIZE; i++)
      ws.x[i] = session.samples[start + i].fsr[c] - mean;

    welch(ws.x, ws.psd, ws);

    double total = 0.0, band = 0.0, peak = 0.0;
    int    k_peak = 1;

    /* bin 0 is the leftover constant component and is skipped */
    for (int k = 1; k < bins; k++) {
      const double p = ws.psd[k];

      total += p;

      if (k >= k_lo && k <= k_hi)
        band += p;

      if (p > peak) {
        peak = p;
        k_peak = k;
      }
    }

    r.dominant_hz[c] = k_peak * SAMPLING_RATE / SEGMENT_SIZE;

    /*
      Normalising by the total power reduces the dependence on the
      overall amplitude of the signal: on an ideal sensor a change of
      scale would cancel out entirely, while a real FSR is not exactly
      linear and its noise and contact change with the force applied.
    */
    if (total > 0.0)
      index_sum += band / total;
  }

  r.index = index_sum / N_CHANNELS;
  return r;
}

SessionSummary analyze_session(const Session& session) {
  SessionSummary sum{};
  DecisionState decision;

  const size_t n = session.samples.size();

  if (n < (size_t)WINDOW_SIZE)
    return sum;

  double index_total = 0.0;
  vector<double> dominants;

  for (size_t start = 0, id = 0;
       start + WINDOW_SIZE <= n;
       start += WINDOW_HOP, id++) {

    WindowResult r = analyze_window(session, start, id);
    r.state = decision.update(r.index);

    index_total += r.index;
    dominants.push_back(r.dominant_hz[0]);

    if (r.index > sum.max_index)
      sum.max_index = r.index;

    switch (r.state) {
      case State::ALERT:     sum.alert_count++;    break;
      case State::PRE_ALERT: sum.prealert_count++; break;
      default:               sum.normal_count++;   break;
    }

    sum.windows++;
  }

  if (sum.windows > 0) {
    sum.mean_index = index_total / sum.windows;
    std::sort(dominants.begin(), dominants.end());
    sum.dominant_hz = dominants[dominants.size() / 2];
  }

  return sum;
}

bool load_session(const std::string& path, Session& out) {
  std::ifstream file(path);

  if (!file) {
    fprintf(stderr, "cannot read %s\n", path.c_str());
    return false;
  }

  out.name = path;
  out.samples.clear();

  std::string line;

  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;

    Sample s{};
    unsigned long id = 0, t_ms = 0;
    double a = 0, b = 0, c = 0;

    if (sscanf(line.c_str(), "%lu,%lu,%lf,%lf,%lf",
               &id, &t_ms, &a, &b, &c) != 5)
      continue;

    s.id = (uint32_t)id;
    s.time_s = t_ms / 1000.0;
    s.fsr[0] = (float)a;
    s.fsr[1] = (float)b;
    s.fsr[2] = (float)c;

    out.samples.push_back(s);
  }

  return !out.samples.empty();
}
