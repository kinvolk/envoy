#!/bin/bash
set -o errexit
set -o nounset
set -o pipefail
PROFDIR=$(dirname "$0")

typeset -i PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid)
if [ "$PARANOID" -ne "-1" ]; then
  echo "To capture kernel events please run: echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid"
  exit 1
fi

cd "$PROFDIR"
which inferno-collapse-perf inferno-flamegraph || cargo install inferno
which actix-web-server || cargo install --path actix-web-server
which inferno-collapse-perf inferno-flamegraph actix-web-server || ( echo "Please add ~/.cargo/bin to your PATH" ; exit 1 )
which wrk || ( echo "wrk not found: Compile the wrk binary from https://github.com/kinvolk/wrk2/ and move it to your PATH" ; exit 1 )

trap '{ killall iperf actix-web-server >& /dev/null; }' EXIT

single_profiling_run () {
  (
  SERVER=actix-web-server
  if [ "$MODE" = "TCP" ]; then
    SERVER="iperf -s -p $SERVER_PORT"
  fi
  $SERVER &
  SPID=$!
  # wait for proxy to start
  until ss -tan | grep "LISTEN.*:$PROXY_PORT"
  do
    sleep 1
  done
  if [ "$MODE" = "TCP" ]; then
    iperf -t 6 -p "$PROXY_PORT" -c localhost | tee "envoy_$NAME.txt"
  else
    wrk -L -s wrk-report.lua -R 4500 -H 'Host: transparency.test.svc.cluster.local' "http://127.0.0.1:$PROXY_PORT/" | tee "envoy_$NAME.txt"
  fi
  killall envoy
  # kill server
  kill $SPID
  ) &
  rm ./perf.data* || true
  perf record --call-graph dwarf "$HOME/envoy-build/envoy/source/exe/envoy" -c envoy-conf.yaml || true
  perf script | inferno-collapse-perf > "envoy_out_$NAME.folded"  # separate step to be able to rerun flamegraph with another width
  inferno-flamegraph --width 4000 "envoy_out_$NAME.folded" > "envoy_flamegraph_$NAME.svg"  # or: flamegraph.pl instead of inferno-flamegraph
}

MODE=TCP NAME=tcpinbound PROXY_PORT=4144 SERVER_PORT=8081 single_profiling_run
MODE=HTTP NAME=inbound PROXY_PORT=4143 single_profiling_run
