# Host-side unit tests for the software UART, run against a simulated ESP8266.
#
#   make -C test/host          # build and run
#   make -C test/host clean
#
# Needs only a C11 compiler; the ESP8266 SDK is not required. The suite is
# built twice: with the library's default configuration and with the
# non-default menuconfig values in VARIANT_CONFIG.

SRC_DIR   := ../../components/softuart
BUILD_DIR := build
CC        ?= cc
CFLAGS    ?= -O1 -g
CFLAGS    += -std=c11 -Wall -Wextra -Werror -Imocks -I. -I$(SRC_DIR)/include
SANITIZE  ?= -fsanitize=address,undefined -fno-sanitize-recover=all
LDLIBS    += -lm

VARIANT_CONFIG := -DCONFIG_SOFTUART_MAX_UARTS=3 \
                  -DCONFIG_SOFTUART_RX_BUFFER_SIZE=16 \
                  -DCONFIG_SOFTUART_TX_EXTRA_IDLE_BITS=5 \
                  -DCONFIG_SOFTUART_TX_CRITICAL_SECTION=1

TESTS := $(BUILD_DIR)/test_softuart $(BUILD_DIR)/test_softuart_variant
SRCS  := test_softuart.c sim.c $(SRC_DIR)/softuart.c
DEPS  := $(SRCS) sim.h $(SRC_DIR)/include/softuart.h $(wildcard mocks/*.h mocks/*/*.h)

.PHONY: all test clean
all: test

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

$(BUILD_DIR)/test_softuart: $(DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $(SRCS) $(LDLIBS)

$(BUILD_DIR)/test_softuart_variant: $(DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(VARIANT_CONFIG) $(SANITIZE) -o $@ $(SRCS) $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
