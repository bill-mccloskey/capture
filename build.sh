#!/bin/bash

clang++ -std=c++14 -O3 -o capture -I ~/decklink-sdk/Mac/include/ Capture.cpp EncodeLib.cpp -framework CoreFoundation

clang++ -std=c++14 Encode.cpp EncodeLib.cpp -o encode -Wall -O3
