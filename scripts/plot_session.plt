# plot_session.plt
# Renders a recorded session as a static image over its whole duration: the
# three channels above, the index with the two thresholds below. Static
# counterpart of plot_live.plt, which shows the same two panels while the
# acquisition is running.
#
#   gnuplot -e "raw='data/live/session_X.csv'; \
#               windows='results/live/session_X_windows.csv'; \
#               outfile='results/session.png'" scripts/plot_session.plt

if (!exists("pre"))     pre     = 0.15
if (!exists("alert"))   alert   = 0.30
if (!exists("outfile")) outfile = 'results/session.png'

set terminal pngcairo size 900,560 font "Arial,10" background rgb "#fcfcfb"
set output outfile

set datafile separator ","
set datafile commentschars "#"

# the samples carry the clock of the board, so they are shifted onto
# the origin the results file counts from
stats raw using 2 nooutput
t0 = STATS_min
tend = (STATS_max - t0) / 1000.0

# an excerpt can be requested to restrict the plotted time range
if (!exists("from")) from = 0
if (!exists("to"))   to   = tend

set multiplot layout 2,1

set grid lc rgb "#d9d8d4"
set xrange [from:to]
set key top left horizontal samplen 1.5

set ylabel "FSR (ADC)"
set format x ""
unset xlabel

plot raw every (to - from > 20 ? 5 : 1) using (($2 - t0)/1000.0):3 w l lw 1 \
       lc rgb "#1f77b4" t "FSR1", \
     ''  every (to - from > 20 ? 5 : 1) using (($2 - t0)/1000.0):4 w l lw 1 \
       lc rgb "#d62728" t "FSR2", \
     ''  every (to - from > 20 ? 5 : 1) using (($2 - t0)/1000.0):5 w l lw 1 \
       lc rgb "#2ca02c" t "FSR3"

set ylabel "index"
set xlabel "time (s)"
set format x (to - from > 20 ? "%.0f" : "%.1f")
set yrange [0:*]
set offsets 0, 0, 0.05, 0

plot windows using 2:6 w l lw 2 lc rgb "#000000" t "index", \
     alert w l lw 1.5 dt 2 lc rgb "#d62728" t sprintf('ALERT %.2f', alert), \
     pre   w l lw 1.5 dt 3 lc rgb "#ff9900" t sprintf('PRE-ALERT %.2f', pre)

unset multiplot
