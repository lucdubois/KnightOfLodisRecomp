#!/bin/sh
# Sends gbarecomp and this repo (with generated/) to the Steam Deck (ssh host "steamdeck",
# ~/kol) and builds the dev binary there inside the freedesktop SDK (~/kol/sdk.sh).
# The Deck side (sdk.sh, run.sh, play.sh, data/) is described in docs/DEVLOG.md "Steam Deck".
cd "$(dirname "$0")/../.." || exit 1
rsync -a --delete --exclude '/build*/' --exclude '/.git/' --exclude 'hang_*' --exclude '.DS_Store' gbarecomp/ steamdeck:kol/gbarecomp/ || exit 1
rsync -a --delete --exclude '/build/' --exclude '/build-deck/' --exclude '/build-android/' --exclude '/android/' --exclude '/states/' --exclude '/shots/' --exclude '/recomp_cache/' --exclude '/.git/' --exclude '/diagnostics_*/' --exclude '*.bmp' --exclude '*.state*' --exclude 'hang_*' --exclude '*.gba' --exclude '*.sav' --exclude '.DS_Store' --exclude '__pycache__/' KnightOfLodisRecomp/ steamdeck:kol/KnightOfLodisRecomp/ || exit 1
ssh steamdeck 'cd ~/kol && ./sdk.sh "cmake --build build-deck --target KnightOfLodisRecomp -- -k 0 > build-deck/build.log 2>&1; echo BUILD_EXIT \$? >> build-deck/build.log"; L=~/kol/KnightOfLodisRecomp/build-deck/build.log; tail -1 $L; grep -E "error:|undefined reference" $L | sed -E "s/^.*kol\///" | sort | uniq -c | sort -rn | head -25; ls -la ~/kol/KnightOfLodisRecomp/build-deck/KnightOfLodisRecomp 2>/dev/null'
