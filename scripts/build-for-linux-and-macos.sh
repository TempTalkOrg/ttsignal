#!/bin/bash

script_dir=$(dirname $(realpath $0))

cd $script_dir/../
bash build/linux-x64-release/build
cd $script_dir/../
bash build/linux-arm64-release/build
cd $script_dir/../
bash build/macos-arm64-release/build
cd $script_dir/../
bash build/macos-x64-release/build