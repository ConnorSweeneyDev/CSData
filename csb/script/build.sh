#!/bin/bash

./csb/script/internal/bootstrap.sh || exit $?
./build/csb build "$@"
