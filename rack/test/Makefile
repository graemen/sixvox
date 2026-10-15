# Copyright 2026 VOX contributors. MIT license, see vox/LICENSE.
#
# Host test of src/AudioBridge.hpp. Needs the Rack SDK headers and the
# speexdsp sources (Rack's resampler).
#   make -C vox/rack/test RACK_DIR=/path/to/Rack-SDK SPEEXDSP=/path/to/speexdsp

RACK_DIR ?= ../../../../Rack-SDK
SPEEXDSP ?= speexdsp

bridge_test: bridge_test.cpp ../src/AudioBridge.hpp
	$(CC) -O2 -c -DFLOATING_POINT -DEXPORT= -I$(RACK_DIR)/dep/include \
		$(SPEEXDSP)/libspeexdsp/resample.c -o resample.o
	$(CXX) -O2 -std=c++11 -I$(RACK_DIR)/include -I$(RACK_DIR)/dep/include \
		-I../../.. -o $@ bridge_test.cpp resample.o -lm -lpthread

check: bridge_test
	./bridge_test

.PHONY: check
