#!/bin/sh
# Guest thread (Thread-4) CPU share over 10 s: utime+stime ticks vs uptime.
pid=$(pidof com.koldecomp.game)
tid=$(for t in /proc/$pid/task/*; do [ "$(cat $t/comm)" = "Thread-4" ] && basename $t; done | head -1)
read u0 _ < /proc/uptime; t0=$(awk '{print $14+$15}' /proc/$pid/task/$tid/stat)
sleep 10
read u1 _ < /proc/uptime; t1=$(awk '{print $14+$15}' /proc/$pid/task/$tid/stat)
echo "pid=$pid tid=$tid cpu=$(echo "$t0 $t1 $u0 $u1" | awk '{printf "%.1f%%", ($2-$1)/100/($4-$3)*100}')"
