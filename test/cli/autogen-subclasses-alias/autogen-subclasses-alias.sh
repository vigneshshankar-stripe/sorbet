#!/bin/bash
set -eu

echo "--- autogen-subclasses-alias ---"
main/sorbet --silence-dev-message --stop-after=namer -p autogen-subclasses \
  --autogen-subclasses-parent=Opus::Mixin \
  test/cli/autogen-subclasses-alias/a.rb
