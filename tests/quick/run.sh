#!/usr/bin/env bash

set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
. "$HERE/../../scripts/env.sh"

"$ROOT/examples/hello-cpp/run.sh"

echo "quick tests: OK"
