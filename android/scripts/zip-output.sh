#!/bin/bash

script_dir=$(dirname $(realpath "$0"))
mkdir -p $script_dir/../out/debug/app/src/main/jniLibs

# cp -rf $script_dir/../app/build/intermediates/stripped_native_libs/debug/stripDebugDebugSymbols/out/lib/arm64-v8a $script_dir/../out/debug/app/src/main/jniLibs
# cp -rf $script_dir/../app/build.gradle $script_dir/../out/debug/app
# cp -rf $script_dir/../app/src $script_dir/../out/debug/app
# cd $script_dir/../out/debug
# tar -jcf ttsignal.tar.bz2 app

mkdir -p $script_dir/../out/debug/app/src/main/jniLibs/arm64-v8a
mkdir -p $script_dir/../out/debug/app/src/main/jniLibs/armeabi-v7a
cp -rf $script_dir/../build/arm64-v8a/libsignal.so $script_dir/../out/debug/app/src/main/jniLibs/arm64-v8a
cp -rf $script_dir/../build/armeabi-v7a/libsignal.so $script_dir/../out/debug/app/src/main/jniLibs/armeabi-v7a
cp -rf $script_dir/../app/src $script_dir/../out/debug/app
cd $script_dir/../out/debug
tar -jcf ttsignal.tar.bz2 app
