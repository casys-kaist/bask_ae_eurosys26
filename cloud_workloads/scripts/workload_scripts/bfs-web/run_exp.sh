cd /home/ubuntu/gapbs
export OMP_NUM_THREADS=4
/usr/bin/time -pao /home/ubuntu/result_app_perf.txt ./bfs -f benchmark/graphs/web.sg -n128 >> /home/ubuntu/result_log.txt 2>> /home/ubuntu/result_err.txt