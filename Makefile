CXX ?= g++
CPPFLAGS ?=
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -pthread

PROJECT_DIRS := $(patsubst %/,%,$(wildcard */))
MAKE_PROJECTS := $(foreach dir,$(PROJECT_DIRS),$(if $(wildcard $(dir)/Makefile),$(dir)))
STANDALONE_SOURCES := $(foreach dir,$(filter-out $(MAKE_PROJECTS),$(PROJECT_DIRS)),$(wildcard $(dir)/*.cpp))
STANDALONE_BINS := $(patsubst %.cpp,build/%,$(STANDALONE_SOURCES))

.PHONY: all clean $(MAKE_PROJECTS)

all: $(STANDALONE_BINS) $(MAKE_PROJECTS)

$(MAKE_PROJECTS):
	$(MAKE) -C "$@" CXX="$(CXX)" all

build/%: %.cpp
	mkdir -p "$(@D)"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) "$<" $(LDFLAGS) $(LDLIBS) -o "$@"

clean:
	@set -e; for dir in $(MAKE_PROJECTS); do \
		$(MAKE) -C "$$dir" clean; \
	done
	rm -rf build
