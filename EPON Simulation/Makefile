CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
LDFLAGS  ?=

# Prefer a stable SDK when the default macOS SDK TBD files are too new for the host linker.
SDKROOT  ?= $(shell ls -d /Library/Developer/CommandLineTools/SDKs/MacOSX15*.sdk 2>/dev/null | tail -n 1)
ifneq ($(SDKROOT),)
  export SDKROOT
endif

SRC_DIR  := src
OBJ_DIR  := build
BIN      := epon_sim

SRCS := \
	$(SRC_DIR)/main.cpp \
	$(SRC_DIR)/metrics.cpp \
	$(SRC_DIR)/signalling.cpp \
	$(SRC_DIR)/polling_manager.cpp \
	$(SRC_DIR)/simulation.cpp

OBJS := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

.PHONY: all clean run dirs

all: dirs $(BIN)

dirs:
	@mkdir -p $(OBJ_DIR) results

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

run: all
	./$(BIN)

clean:
	rm -rf $(OBJ_DIR) $(BIN)
