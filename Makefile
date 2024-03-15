CC = gcc
CFLAGS = -Wall -Wextra -g
SRC_DIR = src
BIN_DIR = bin

all: $(BIN_DIR)/shell $(BIN_DIR)/proc_parse
shell: $(BIN_DIR)/shell
proc: $(BIN_DIR)/proc_parse

$(BIN_DIR)/shell: $(SRC_DIR)/shell.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< -lm

$(BIN_DIR)/proc_parse: $(SRC_DIR)/proc_parse.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< -lm

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

.PHONY: clean
clean:
	rm -rf $(BIN_DIR)
