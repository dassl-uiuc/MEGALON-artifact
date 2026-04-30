#!/bin/bash

sudo apt update
sudo apt install -y libnuma-dev libhwloc-dev liburcu-dev pkg-config libclang-dev

#install rust nightly
yes "" | curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source $HOME/.cargo/env
rustup toolchain install nightly
rustup default nightly