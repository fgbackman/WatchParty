#!/bin/sh
# dev-loop.sh — a tiny build helper so Claude can rebuild/test from the shared
# folder without a shell on this Mac. It only ever runs the fixed make targets
# below. Start it in Terminal:   sh ~/Developer/WatchParty/dev-loop.sh
# Stop it any time with Ctrl-C.
cd "$(dirname "$0")" || exit 1
echo "WatchParty dev loop running in $(pwd) — Ctrl-C to stop."
while true; do
  if [ -f .request ]; then
    cmd=$(cat .request)
    rm -f .request
    {
      echo "=== $(date) === request: $cmd"
      case "$cmd" in
        info)     sw_vers; xcode-select -p; clang --version; node --version 2>/dev/null; echo "arch: $(uname -m)" ;;
        build)    make ;;
        test)     make test ;;
        selftest) make selftest ;;
        run)      make run ;;
        all)      make test && make && make selftest ;;
        clean)    make clean ;;
        stop)     echo "stopped" ;;
        *)        echo "unknown request: $cmd" ;;
      esac
      echo "=== done (exit $?) ==="
    } > build.log 2>&1
    echo "ran: $cmd  (output in build.log)"
    [ "$cmd" = "stop" ] && exit 0
  fi
  sleep 1
done
