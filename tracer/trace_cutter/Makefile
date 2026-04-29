# Makefile for trace_cutter
#
# Honours ZSTD_HOME (same convention as champsim/tracer/make_tracer.sh).
# Falls back to system zstd if ZSTD_HOME is not set.

ZSTD_HOME ?=

CXX      ?= g++
CXXFLAGS ?= -O3 -march=native -flto -DNDEBUG -std=c++17 -Wall -Wextra
LDFLAGS  ?=

ifneq ($(strip $(ZSTD_HOME)),)
  CXXFLAGS += -I$(ZSTD_HOME)/include
  LDFLAGS  += -L$(ZSTD_HOME)/lib -Wl,-rpath,$(ZSTD_HOME)/lib
endif

LDLIBS   := -lzstd -lpthread

BIN := trace_cutter
SRC := trace_cutter.cpp

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(BIN)

.PHONY: all clean
