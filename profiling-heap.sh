#!/bin/bash
set -o errexit
set -o nounset
set -o pipefail
PROFDIR=$(dirname "$0")

cd "$PROFDIR"
which actix-web-server || cargo install --path actix-web-server
which actix-web-server || ( echo "Please add ~/.cargo/bin to your PATH" ; exit 1 )
which wrk || ( echo "wrk not found: Compile the wrk binary from https://github.com/kinvolk/wrk2/ and move it to your PATH" ; exit 1 )
ls libmemory_profiler.so memory-profiler-cli || ( curl -L -O https://github.com/nokia/memory-profiler/releases/download/0.3.0/memory-profiler-x86_64-unknown-linux-gnu.tgz ; tar xf memory-profiler-x86_64-unknown-linux-gnu.tgz ; rm memory-profiler-x86_64-unknown-linux-gnu.tgz )

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
  curl --data-ascii '' http://127.0.0.1:9901/heapprofiler?enable=y
  if [ "$MODE" = "TCP" ]; then
    iperf -t 6 -p "$PROXY_PORT" -c localhost | tee "envoy_heap_$NAME.txt"
  else
    wrk -L -s wrk-report.lua -R 4500 -H 'Host: transparency.test.svc.cluster.local' "http://127.0.0.1:$PROXY_PORT/" | tee "envoy_heap_$NAME.txt"
  fi
  # quit envoy
  curl --data-ascii '' http://127.0.0.1:9901/quitquitquit
  mkdir /tmp/$NAME
  mv /tmp/envoy.prof* /tmp/$NAME
  # kill server
  kill $SPID
  ) &
  "$HOME/envoy-build/envoy/source/exe/envoy" -c envoy-conf.yaml
}

MODE=TCP NAME=tcpinbound PROXY_PORT=4144 SERVER_PORT=8081 single_profiling_run
MODE=HTTP NAME=inbound PROXY_PORT=4143 single_profiling_run
ls /tmp/*/envoy*
echo "Run pprof to analyze the above files."
