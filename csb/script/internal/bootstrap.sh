#!/bin/bash

mkdir -p build

need_compile=false
if [ ! -f "build/csb" ]; then
  need_compile=true
elif [ -f "csb/csb.cpp" ] && [ "csb/csb.cpp" -nt "build/csb" ]; then
  need_compile=true
elif [ -f "csb/csb.hpp" ] && [ "csb/csb.hpp" -nt "build/csb" ]; then
  need_compile=true
fi
if [ "$need_compile" = true ]; then
  g++ -std=c++20 -O3 -Icsb/ -o build/csb csb/csb.cpp
  if [ $? -ne 0 ]; then exit $?; fi
fi
