#!/usr/bin/env bash

set -e
set -x

python -m gprof2dot --format=callgrind --output=$1.dot callgrind.out.$1
dot -Tpng $1.dot -o $1.png
rm $1.dot

echo $1.png