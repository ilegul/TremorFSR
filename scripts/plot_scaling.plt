# plot_scaling.plt
# Speedup against the number of workers, farm and OpenMP on the same
# axes, with the ideal linear line, from the sweep the benchmark writes.
#
#   awk -F, '/^farm\(/  {print $2","$6}' results/benchmark.csv > farm.dat
#   awk -F, '/^openmp\(/{print $2","$6}' results/benchmark.csv > omp.dat
#   gnuplot -e "farmfile='farm.dat'; ompfile='omp.dat'; \
#               outfile='results/scaling.png'" scripts/plot_scaling.plt

if (!exists("farmfile")) farmfile = 'farm.dat'
if (!exists("ompfile"))  ompfile  = 'omp.dat'
if (!exists("outfile"))  outfile  = 'results/scaling.png'

set terminal pngcairo size 760,600 font "Arial,11" background rgb "#fcfcfb"
set output outfile

set datafile separator ","

# log-log axes: ideal linear speedup (y = x) is a straight 45 line.
# Any loss of efficiency shows as the points falling below it.
set title "Throughput scaling on Ampere (256-core ARM Neoverse-N1)"
set xlabel "workers"
set ylabel "speedup over the sequential baseline"
set logscale x 2
set logscale y 2
set xrange [0.9:320]
set yrange [0.9:320]
set xtics (1,2,4,8,16,32,64,128,256)
set ytics (1,2,4,8,16,32,64,128,256)
set size square
set grid lc rgb "#d9d8d4"
set key top left

ideal(x) = x

plot ideal(x) w l dt 2 lw 1.5 lc rgb "#999999" t "ideal (linear speedup)", \
     farmfile using 1:2 w lp pt 7 ps 1.3 lw 2 lc rgb "#2a78d6" t "FastFlow farm", \
     ompfile  using 1:2 w lp pt 5 ps 1.2 lw 2 lc rgb "#d1603d" t "OpenMP parallel for"
