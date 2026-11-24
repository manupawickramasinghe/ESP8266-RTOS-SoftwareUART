#
# Component makefile for the legacy GNU Make build system.
#
COMPONENT_ADD_INCLUDEDIRS := include

CFLAGS += -Wall -Wextra
# CI builds with SOFTUART_WERROR=1 so new warnings fail the build
ifdef SOFTUART_WERROR
CFLAGS += -Werror
endif
