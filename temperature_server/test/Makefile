# Host unit tests for the hardware-free modules (logic.h, history.cpp).
# Usage:  make -C test        (builds and runs)
SRC := ../temperature_server_src

test_logic: test_logic.cpp $(SRC)/history.cpp $(SRC)/logic.h $(SRC)/history.h $(SRC)/node_packet.h $(SRC)/config.h
	g++ -std=c++17 -Wall -Wextra -I$(SRC) -o $@ test_logic.cpp $(SRC)/history.cpp

.PHONY: run clean
run: test_logic
	./test_logic

clean:
	rm -f test_logic

.DEFAULT_GOAL := run
