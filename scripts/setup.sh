#!/usr/bin/env bash

set -e 
set -x

git submodule update --init --recursive
git apply third-party/hostrpc.diff --directory third-party/hostrpc

# Installs
sudo apt-get update -y
sudo apt-get install -y python3-pip make valgrind clang-format libc++-dev \
    numactl libnuma-dev zlib1g-dev cloc pkg-config libhwloc-dev libmemkind-dev
sudo apt install clang-format -y
# sudo apt-get install -y python3-pip g++-10 make valgrind clang-format numactl \
#     libc++-dev libnuma-dev zlib1g-dev libudev-dev libnl-3-dev libxxhash-dev \
#     libnl-route-3-dev libgflags-dev libgflags2.2 libhugetlbfs-dev \
#     libunwind-dev uuid-dev nlohmann-json3-dev \
#     cloc build-essential meson ca-certificates autoconf pkg-config pciutils \
#     python3-dev python3-docutils python3-pyelftools

# Install cmake with pip since apt installs an outdated version 
sudo pip install cmake numpy matplotlib

sudo ./scripts/llvm.sh 17

# update compiler
BASHRC="$HOME/.bashrc"
COMMENT_LINE="# rackobj compiler flag"
EXPORT_LINES="export CXX=clang++-17\nexport CC=clang-17\nexport LD=clang++-17"

# Check if all of the environment variables are already present in ~/.bashrc
if grep -q "^export CXX=clang++-17" "$BASHRC" && \
   grep -q "^export CC=clang-17" "$BASHRC" && \
   grep -q "^export LD=clang++-17" "$BASHRC"; then
    echo "Compiler environment variables are already set in $BASHRC. Skipping..."
else
    # Append the comment and export lines to ~/.bashrc
    echo -e "\n$COMMENT_LINE\n$EXPORT_LINES" >> "$BASHRC"
    echo "Added the following lines to $BASHRC:"
    echo -e "$COMMENT_LINE\n$EXPORT_LINES"
fi

# enable uncore frequency setup
sudo modprobe intel_uncore_frequency

# for vscode: set up compiler flag tracking
SETTINGS_FILE=".vscode/settings.json"
KEY="C_Cpp.default.compileCommands"
VALUE="build/compile_commands.json"
mkdir -p .vscode

if [ ! -f "$SETTINGS_FILE" ]; then
    echo "{ \"$KEY\": \"$VALUE\" }" > "$SETTINGS_FILE"
    echo "Created $SETTINGS_FILE with $KEY."
else
    if jq -e ".\"$KEY\"" "$SETTINGS_FILE" > /dev/null; then
        jq ".\"$KEY\" = \"$VALUE\"" "$SETTINGS_FILE" > tmp.json && mv tmp.json "$SETTINGS_FILE"
        echo "Updated $KEY in $SETTINGS_FILE."
    else
        jq ". + {\"$KEY\": \"$VALUE\"}" "$SETTINGS_FILE" > tmp.json && mv tmp.json "$SETTINGS_FILE"
        echo "Added $KEY to $SETTINGS_FILE."
    fi
fi

rm -f tmp.json

# set up $RACKOBJ_RESULT_DIR
BASHRC="$HOME/.bashrc"
ENV_VAR="RACKOBJ_RESULT_DIR"
EXPORT_LINE="export RACKOBJ_RESULT_DIR=/mydata/jiyu/rackobj-benchmarks/benchmarks/results/"
COMMENT_LINE="# set up rackobj result dir"

# Check if RACKOBJ_RESULT_DIR is already present in ~/.bashrc
if grep -q "^export $ENV_VAR=" "$BASHRC"; then
    echo "$ENV_VAR is already set in $BASHRC. Skipping..."
else
    # Append the comment and export line to ~/.bashrc
    echo -e "\n$COMMENT_LINE\n$EXPORT_LINE" >> "$BASHRC"
    echo "Added the following lines to $BASHRC:"
    echo -e "$COMMENT_LINE\n$EXPORT_LINE"
fi

./scripts//setup_logical_node.sh 0 3 24
