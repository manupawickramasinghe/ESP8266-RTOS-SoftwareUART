# Host-side unit tests for the software UART, run against a simulated ESP8266.
#
#   make -C test/host          # build and run
#   make -C test/host clean
#
# Needs only a C11 compiler; the ESP8266 SDK is not required.

SRC_DIR   := ../../main
BUILD_DIR := build
CC        ?= cc
CFLAGS    ?= -O1 -g
CFLAGS    += -std=c11 -Wall -Wextra -Werror -Imocks -I. -I$(SRC_DIR)
SANITIZE  ?= -fsanitize=address,undefined -fno-sanitize-recover=all
LDLIBS    += -lm

TEST := $(BUILD_DIR)/test_softuart
SRCS := test_softuart.c sim.c $(SRC_DIR)/softuart.c

.PHONY: all test clean
all: test

test: $(TEST)
	./$(TEST)

$(TEST): $(SRCS) sim.h $(SRC_DIR)/softuart.h $(wildcard mocks/*/*.h) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $(SRCS) $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
