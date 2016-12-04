# run with gnuplot -e filename=\'glx_hook_frametimes-ctx1.csv\' script.gnuplot

set yrange [0:50]
set ylabel 'time [milliseconds]'
set xlabel 'frame number'

set style line 1 lt 1 lw 1 lc rgb "#109010"
set style line 2 lt 1 lw 1 lc rgb "black"
set style line 3 lt 1 lw 3 lc rgb "red"
set style line 4 lt 1 lw 1 lc rgb "blue"

set terminal png size 1600,1000
outfile=sprintf("%s.png",filename)
set output outfile

plot \
	filename using 1:($2/1000000) w l ls 1 title 'draw (CPU)', \
	filename using 1:($3/1000000) w l ls 2 title 'draw (GPU)', \
	filename using 1:($6/1000000) w l ls 3 title 'whole frame (GPU)', \
	filename using 1:($7/1000000) w l ls 4 title 'latency'
