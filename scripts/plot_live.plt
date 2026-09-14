#
#  plot_live.plt
#  Live view of a serial session, redrawn once a second while it runs.
#  Started by
#
#      live COM9 60 -plot
#
#  or by hand on the two files a live run writes. It only draws those
#  files and computes nothing; the analysis stays in the application.
#  Upper panel: the three FSR channels. Lower panel: the index with the
#  two decision thresholds.
#

if (!exists("raw"))     raw     = 'data/live/session.csv'
if (!exists("windows")) windows = 'results/live/session_windows.csv'
if (!exists("pre"))     pre     = 0.15
if (!exists("alert"))   alert   = 0.30
if (!exists("rate"))    rate    = 500.0
if (!exists("span"))    span    = 10.0

set term qt size 900,720 noraise title "TremorFSR live"
set datafile separator ","
set datafile commentschars "#"

previous = -1
idle = 0

while (1) {
  undefine STATS_records
  undefine STATS_min
  undefine STATS_max
  stats raw using 2 nooutput
  n_raw   = exists("STATS_records") ? STATS_records : 0
  t_first = exists("STATS_min") ? STATS_min : 0
  t_last  = exists("STATS_max") ? STATS_max : 0

  undefine STATS_records
  stats windows using 2 nooutput
  n_win = exists("STATS_records") ? STATS_records : 0

  # show the last `span` seconds of the samples
  tmax  = n_raw > 0 ? (t_last - t_first) / 1000.0 : span
  tmin  = tmax > span ? tmax - span : 0
  first = n_raw > span * rate ? n_raw - span * rate : 0

  set multiplot layout 2,1 title sprintf("TremorFSR live   %.1f s", tmax)
  set xrange [tmin:tmax]
  set grid

  # upper panel: the three sensor channels
  set ylabel "FSR (ADC)"
  set yrange [0:1023]
  set format x ""
  unset xlabel
  if (n_raw > 0) {
    plot raw every ::first using (($2-t_first)/1000.0):3 w l lc rgb "#1f77b4" t "FSR1", \
         ''  every ::first using (($2-t_first)/1000.0):4 w l lc rgb "#d62728" t "FSR2", \
         ''  every ::first using (($2-t_first)/1000.0):5 w l lc rgb "#2ca02c" t "FSR3"
  } else {
    # nothing on disk yet: draw an empty frame (plot NaN would abort)
    plot -1 lc rgb "#ffffff" notitle
  }

  # lower panel: the index with the two thresholds
  set ylabel "index"
  set xlabel "time (s)"
  set format x "%.1f"
  set yrange [0:*]
  if (n_win > 0) {
    plot windows using 2:6 w lp pt 7 ps 0.4 lc rgb "#000000" t "index", \
         alert w l dt 2 lc rgb "#d62728" t "ALERT", \
         pre   w l dt 3 lc rgb "#ff9900" t "PRE\\_ALERT"
  } else {
    plot alert w l dt 2 lc rgb "#d62728" t "ALERT", \
         pre   w l dt 3 lc rgb "#ff9900" t "PRE\\_ALERT"
  }

  unset multiplot

  # stop once the file has stopped growing, so the window can be closed
  idle = (n_raw > 0 && n_raw == previous) ? idle + 1 : 0
  previous = n_raw
  if (idle > 1) { break }
  pause 1.0
}

print "session over: close the window to quit"
pause mouse close
