#!/bin/sh
# fps of the game's surface: 6 samples of the last 120 presents, 3 s apart.
L=$(dumpsys SurfaceFlinger --list | grep -i koldecomp | grep -i blast | head -1)
for r in 1 2 3 4 5 6; do
  dumpsys SurfaceFlinger --latency "$L" | awk 'NF==3 && $2>0 && $2<9e18 {print $2}' | tail -120 |
    awk 'NR==1{f=$1} {if(NR>1){d=$1-p; if(d>25e6) j++} p=$1; l=$1} END{printf "fps=%.1f long=%d\n", (NR-1)/((l-f)/1e9), j}'
  sleep 3
done
