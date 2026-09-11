#!/bin/bash
gcc -O2 -Wall -Wno-cpp -o sip_headless sip_headless.c     $(pkg-config --cflags --libs libpjproject) -lpthread
