/*
  benchmark.cpp
  Measures the throughput a FastFlow farm adds when independent session
  analyses are run in parallel.

  A task is a whole session, not a single window: a window is small
  enough that a FastFlow handoff (tens of nanoseconds) could stream it,
  but the decision carries state along the ordered windows of a session,
  so distributing single windows would need reordering in front of the
  stateful stage while whole sessions, being independent, need none. The
  farm is applied to the stage the profiling shows to dominate.
*/
#include "analysis.hpp"
#include "utimer.hpp"

#include <ff/ff.hpp>
#include <ff/farm.hpp>
#include <ff/pipeline.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace ff;
using std::string;
using std::vector;

/*
  One unit of work: a read-only input and the place its result goes.
  The nodes exchange the pointer, so nothing is copied and nothing is
  allocated per result; the identifier keeps the task recognisable
  whatever order the workers finish in.
*/
struct Job {
  size_t         id;
  const Session *input;
  SessionSummary result;
};

/* ---------------- the three nodes ---------------- */

struct SourceNode : ff_node_t<Job> {
  vector<Job>& jobs;

  explicit SourceNode(vector<Job>& j) : jobs(j) {}

  Job *svc(Job *) override {
    for (Job& j : jobs)
      ff_send_out(&j);

    return EOS;
  }
};

struct ComputeNode : ff_node_t<Job> {
  Job *svc(Job *j) override {
    j->result = analyze_session(*j->input);
    return j;
  }
};

struct DrainNode : ff_node_t<Job> {
  size_t received = 0;
  size_t windows  = 0;

  Job *svc(Job *j) override {
    received++;
    windows += j->result.windows;
    return GO_ON;
  }
};

/* ---------------- the three ways of running them ---------------- */

/*
  Each of the four ways of running the jobs only performs the work and
  reports whether it succeeded; the elapsed time is taken by the caller,
  with a utimer wrapped around the call in measure(). This keeps a single
  timing primitive for every configuration.
*/

/* the baseline: no threads, no runtime, just the loop */
bool run_sequential(vector<Job>& jobs) {
  for (Job& j : jobs)
    j.result = analyze_session(*j.input);

  return true;
}

/* Source -> Compute -> Drain, one Compute: the stream version */
bool run_pipeline(vector<Job>& jobs, bool stats) {
  SourceNode  source(jobs);
  ComputeNode compute;
  DrainNode   drain;

  ff_Pipe<Job> pipe(source, compute, drain);

  if (pipe.run_and_wait_end() < 0)
    return false;

  if (stats) {
    printf("\nffTime: %.1f ms\n", pipe.ffTime());
    pipe.ffStats(std::cout);
  }

  return true;
}

/* Source -> farm(Compute) -> Drain */
bool run_farm(vector<Job>& jobs, int nworkers) {
  SourceNode source(jobs);
  DrainNode  drain;

  vector<std::unique_ptr<ff_node>> workers;

  for (int i = 0; i < nworkers; i++)
    workers.push_back(std::make_unique<ComputeNode>());

  ff_Farm<Job> farm(std::move(workers));

  ff_Pipe<Job> pipe(source, farm, drain);

  return pipe.run_and_wait_end() >= 0;
}

/*
  The same jobs under OpenMP: one pragma on the sequential loop.
  schedule(static, 1) deals the jobs out cyclically.
*/
bool run_openmp(vector<Job>& jobs, int nthreads) {
  #pragma omp parallel for schedule(static, 1) num_threads(nthreads)
  for (int i = 0; i < (int)jobs.size(); i++)
    jobs[i].result = analyze_session(*jobs[i].input);

  return true;
}

/*
  Every configuration must produce the same summaries; only the order
  in which the workers finish may differ.
*/
bool same(const SessionSummary& a, const SessionSummary& b) {
  return a.windows == b.windows
      && a.normal_count == b.normal_count
      && a.prealert_count == b.prealert_count
      && a.alert_count == b.alert_count
      && fabs(a.mean_index - b.mean_index) < 1e-12
      && fabs(a.max_index - b.max_index) < 1e-12
      && fabs(a.dominant_hz - b.dominant_hz) < 1e-12;
}


int mismatches(const vector<Job>& jobs, const vector<SessionSummary>& ref) {
  int bad = 0;

  for (size_t i = 0; i < jobs.size(); i++)
    if (!same(jobs[i].result, ref[i]))
      bad++;

  return bad;
}


/* ---------------- measurement ---------------- */

/*
  Seven executions per configuration: the fastest and the slowest are
  discarded and the five in the middle are averaged, so that a single
  perturbed run does not decide the figure. The extremes are kept and
  reported, because they show how much the runs varied.
*/
struct Measure {
  double mean, min, max;
};

Measure measure(const string& label, const std::function<bool()>& run,
                const vector<Job>& jobs, const vector<SessionSummary>& reference,
                int& bad_total, int runs) {
  vector<double> t;

  /* every configuration is timed by the same primitive: a utimer around
     the call, which stores the elapsed microseconds it also prints. The
     result is checked against the reference after each run, outside the
     timed region, so a wrong result in any run is caught and the
     verification is not counted in the measured time */
  for (int i = 0; i < runs; i++) {
    long usec = 0;
    {
      utimer u(label, &usec);
      run();
    }
    t.push_back((double)usec / 1e6);
    if (!reference.empty())
      bad_total += mismatches(jobs, reference);
  }

  vector<double> sorted = t;
  std::sort(sorted.begin(), sorted.end());

  /*
    Drop the fastest and the slowest and average the rest; with fewer
    than three runs there is nothing to trim, so average them all.
  */
  size_t first = (sorted.size() >= 3) ? 1 : 0;
  size_t last  = (sorted.size() >= 3) ? sorted.size() - 1 : sorted.size();

  double sum = 0.0;
  for (size_t i = first; i < last; i++)
    sum += sorted[i];

  const double trimmed = sum / (double)(last - first);

  printf("   runs:");

  for (double x : t)
    printf(" %7.3f", x);

  printf("\n");

  /* the reported minimum and maximum are those of the runs kept after the
     extremes are trimmed, so they describe the same set the mean is taken
     over; with fewer than three runs nothing is trimmed */
  const bool   trim = sorted.size() >= 3;
  const double lo   = trim ? sorted[1] : sorted.front();
  const double hi   = trim ? sorted[sorted.size() - 2] : sorted.back();

  return { trimmed, lo, hi };
}

int main(int argc, char *argv[]) {
  /* usage: benchmark [datadir] [n_jobs] [workers_csv] [runs] [mode]
     with no arguments it runs a small default sweep; on a many-core
     machine pass e.g.  benchmark data/synthetic 256 1,2,4,8,16,32,64,128 7
     mode = "farm" or "openmp" times only that sweep, after one unmeasured
     warm-up run and with no baseline, pipeline or CSV: the form used
     to compare thread-mapping strategies ("farm": default pinning vs
     -DNO_DEFAULT_MAPPING vs core 0 excluded with taskset) and to re-measure
     a single configuration warm. The sequential baseline is not re-measured under
     each mapping, since mapping does not change a
     single-threaded loop. Correctness is verified by the full run. */
  const string dir    = (argc > 1) ? argv[1] : "data/synthetic";
  const int    n_jobs = (argc > 2) ? std::atoi(argv[2]) : 24;
  const int    runs   = (argc > 4) ? std::atoi(argv[4]) : 7;
  const bool   farm_only   = (argc > 5 && string(argv[5]) == "farm");
  const bool   openmp_only = (argc > 5 && string(argv[5]) == "openmp");

  vector<int> W;
  if (argc > 3) {
    const string s = argv[3];
    size_t p = 0;
    while (p <= s.size()) {
      const size_t c = s.find(',', p);
      const string tok = s.substr(p, (c == string::npos) ? string::npos : c - p);
      if (!tok.empty()) W.push_back(std::atoi(tok.c_str()));
      if (c == string::npos) break;
      p = c + 1;
    }
  }
  if (W.empty()) W = { 2, 4 };

  /* the three known inputs, loaded once, before anything is timed */
  const char *names[3] = { "calm", "mild_5hz", "tremor_5hz" };
  vector<Session> sessions(3);

  for (int i = 0; i < 3; i++) {
    if (!load_session(dir + "/" + string(names[i]) + ".csv", sessions[i]))
      return 1;

    printf("loaded %-11s %zu samples\n",
           names[i], sessions[i].samples.size());
  }

  /* independent jobs, cycling over the three recordings */
  vector<Job> jobs(n_jobs);

  for (size_t i = 0; i < jobs.size(); i++) {
    jobs[i].id = i;
    jobs[i].input = &sessions[i % 3];
  }

  printf("\n--- correctness: one session of each input ---\n");
  printf("%-11s %8s %11s %10s %11s %8s %8s %8s\n",
         "input", "windows", "mean index", "max index", "dominant",
         "NORMAL", "PRE_AL", "ALERT");

  for (int i = 0; i < 3; i++) {
    const SessionSummary s = analyze_session(sessions[i]);
    printf("%-11s %8zu %11.4f %10.4f %8.2f Hz %8zu %8zu %8zu\n",
           names[i], s.windows, s.mean_index, s.max_index, s.dominant_hz,
           s.normal_count, s.prealert_count, s.alert_count);
  }

  /*
    The two granularities, measured directly: what one window costs and
    what one whole session costs. The farm distributes a session and not
    a window because of this ratio.
  */
  {
    const int trials = 200;

    START(w0);
    for (int i = 0; i < trials; i++)
      analyze_window(sessions[2], (size_t)i * WINDOW_HOP, i);
    STOP(w0, win_total_us);
    const double win_us = (double)win_total_us / trials;

    START(s0);
    const SessionSummary one = analyze_session(sessions[2]);
    STOP(s0, sess_total_us);
    const double sess_s = (double)sess_total_us / 1e6;

    printf("\n--- granularity ---\n");
    printf("  one window   %8.1f us\n", win_us);
    printf("  one session  %8.3f s   (%zu windows)\n", sess_s, one.windows);
    printf("  ratio        %8.0f windows per task\n", sess_s * 1e6 / win_us);
  }

  printf("\n%d independent session-analysis jobs, %d runs each\n\n",
         n_jobs, runs);

  /* the mapping experiments below only time the farm; they pass an empty
     reference so measure() skips the correctness check that the full
     campaign performs */
  vector<SessionSummary> no_reference;
  int mapping_bad = 0;

  /*
    Mapping experiment: time only the farm, so the sequential baseline
    (unaffected by thread mapping) is not re-measured under each mapping.
    One unmeasured farm run warms the caches; speedup is taken later
    against the baseline the full run measured.
  */
  if (farm_only) {
    printf("--- farm-only (thread-mapping experiment) ---\n");
    run_farm(jobs, W.back());
    for (int nw : W) {
      printf("farm(%d)\n", nw);
      const Measure m = measure("farm(" + std::to_string(nw) + ")",
                                [&] { return run_farm(jobs, nw); },
                                jobs, no_reference, mapping_bad, runs);
      printf("farm(%d)  mean=%.4f  min=%.4f  max=%.4f\n",
             nw, m.mean, m.min, m.max);
    }
    return 0;
  }

  /* Warm re-measurement of the OpenMP loop only: one unmeasured warm-up
     run of the widest configuration, then the timed runs. Used to check
     that a full-occupancy OpenMP figure is not an artefact of cold runs. */
  if (openmp_only) {
    printf("--- openmp-only (warm re-measurement) ---\n");
    run_openmp(jobs, W.back());
    for (int nw : W) {
      printf("openmp(%d)\n", nw);
      const Measure m = measure("openmp(" + std::to_string(nw) + ")",
                                [&] { return run_openmp(jobs, nw); },
                                jobs, no_reference, mapping_bad, runs);
      printf("openmp(%d)  mean=%.4f  min=%.4f  max=%.4f\n",
             nw, m.mean, m.min, m.max);
    }
    return 0;
  }

  /* one unmeasured pass, so every measured run starts warm */
  run_sequential(jobs);

  vector<SessionSummary> reference;

  for (const Job& j : jobs)
    reference.push_back(j.result);

  int bad_total = 0;

  /*
    The summaries are compared against the reference after every run,
    outside the timed region, so a wrong result in any of the seven
    executions is caught, not only one the last run leaves.
  */
  struct Row { string name; int workers; Measure m; };
  vector<Row> rows;

  printf("sequential\n");
  rows.push_back({ "sequential", 0,
                   measure("sequential",
                           [&] { return run_sequential(jobs); },
                           jobs, reference, bad_total, runs) });

  printf("pipeline, one Compute\n");
  rows.push_back({ "pipeline", 1,
                   measure("pipeline",
                           [&] { return run_pipeline(jobs, false); },
                           jobs, reference, bad_total, runs) });

  /* the farm, one row per worker count in the sweep */
  for (int nw : W) {
    printf("farm(%d)\n", nw);
    rows.push_back({ "farm", nw,
                     measure("farm(" + std::to_string(nw) + ")",
                             [&] { return run_farm(jobs, nw); },
                             jobs, reference, bad_total, runs) });
  }

  /* the one-pragma comparison, on the same jobs and the same checks */
  for (int nt : W) {
    printf("openmp(%d)\n", nt);
    rows.push_back({ "openmp", nt,
                     measure("openmp(" + std::to_string(nt) + ")",
                             [&] { return run_openmp(jobs, nt); },
                             jobs, reference, bad_total, runs) });
  }

  const double t_seq = rows[0].m.mean;

  printf("\n%-22s %9s %8s %8s %9s %12s\n",
         "configuration", "time(s)", "min", "max", "speedup", "sessions/s");

  for (const Row& r : rows) {
    string label = r.name;

    if (r.name == "farm" || r.name == "openmp")
      label = r.name + "(" + std::to_string(r.workers) + ")";

    printf("%-22s %9.3f %8.3f %8.3f %9.2f %12.2f\n",
           label.c_str(), r.m.mean, r.m.min, r.m.max,
           t_seq / r.m.mean, n_jobs / r.m.mean);
  }

  /*
    The table the speedup figure is drawn from. The traced build does
    not write it: tracing costs time, so the timings must come from the
    build that runs without it.
  */
#ifndef TRACE_FASTFLOW
  {
    std::ofstream f("results/benchmark.csv");

    if (f) {
      f << "configuration,workers,time_s,min_s,max_s,speedup,sessions_s\n";

      for (const Row& r : rows) {
        string label = r.name;

        if (r.name == "farm" || r.name == "openmp")
          label = r.name + "(" + std::to_string(r.workers) + ")";

        f << label << "," << r.workers << "," << r.m.mean << ","
          << r.m.min << "," << r.m.max << ","
          << t_seq / r.m.mean << "," << n_jobs / r.m.mean << "\n";
      }

      printf("\nsaved: results/benchmark.csv\n");
    }
  }
#endif

  printf("\nresults identical across all configurations: %s\n",
         bad_total == 0 ? "yes" : "NO");

  /* the profiling that identifies the stage that is replicated; only the
     traced build runs it, so the normal benchmark does not run an extra
     pipeline pass that produces no output */
#ifdef TRACE_FASTFLOW
  printf("\n=== ffStats, Source -> Compute -> Drain ===\n");
  run_pipeline(jobs, true);
#endif

  return 0;
}
