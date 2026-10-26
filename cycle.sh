#!/bin/bash
# All labelled variants in ONE USB session (the dongle needs a replug between sessions).
exec /c/Users/heniu/miniconda3/python.exe -u /c/dev/drong-display/drongcast.py --source test --duration 12 \
  --variants "B:x264:60:au,G:x264:30:au,H:x264hq:60:au,C:x264:60:nal,D:amf:60:au,F:amf:60:nal" 2>&1 | grep --line-buffered -vE 'raw\[|data\['
