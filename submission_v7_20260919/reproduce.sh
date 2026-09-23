#!/bin/sh
set -eu
cd "$(dirname "$0")"
sh build.sh
python3 validate_submission.py
python3 package_submission.py
