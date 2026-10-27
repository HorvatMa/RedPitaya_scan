# RedPitaya_SiPM server2
#   make          build/rpsipm_server_fake (simulated FPGA, runs anywhere) and
#                 build/rpsipm_server (real FPGA via UIO; run on the board with the overlay applied)
#   make test     host-side unit tests (reconstruction core)
#   make bench    reconstruction throughput benchmark
#   make clean

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Wshadow -Wpointer-arith
LDLIBS  := -lm -lpthread

# on the Red Pitaya (Zynq-7010, Cortex-A9): ~2x faster reconstruction than plain -O2
ifeq ($(shell uname -m),armv7l)
CFLAGS  += -O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
endif

BUILD   := build
CORE    := src/recon/scan.c src/recon/recon.c
SERVER  := src/main.c src/app.c src/config.c src/util/ring.c src/net/proto.c src/net/server.c
FAKE    := src/hw/hw_fake.c src/hw/sim.c
UIO     := src/hw/hw_uio.c
HEADERS := $(wildcard src/*.h src/*/*.h)

.PHONY: all test bench clean
all: $(BUILD)/rpsipm_server_fake $(BUILD)/rpsipm_server

$(BUILD)/rpsipm_server: $(SERVER) $(CORE) $(UIO) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SERVER) $(CORE) $(UIO) $(LDLIBS)

$(BUILD)/rpsipm_server_fake: $(SERVER) $(CORE) $(FAKE) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SERVER) $(CORE) $(FAKE) $(LDLIBS)

$(BUILD)/test_main: $(CORE) src/hw/sim.c tests/test_main.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(CORE) src/hw/sim.c tests/test_main.c $(LDLIBS)

$(BUILD)/bench: $(CORE) src/hw/sim.c tests/bench.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(CORE) src/hw/sim.c tests/bench.c $(LDLIBS)

test: $(BUILD)/test_main
	cd $(BUILD) && ./test_main

bench: $(BUILD)/bench
	./$(BUILD)/bench

clean:
	rm -rf $(BUILD)
