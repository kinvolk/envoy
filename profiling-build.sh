#!/bin/sh
ENVOY_DOCKER_BUILD_DIR="$HOME/envoy-build" ./ci/run_envoy_docker.sh './ci/do_ci.sh bazel.debug.server_only'
