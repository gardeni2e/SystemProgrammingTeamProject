CC := gcc
CFLAGS := -Wall -Wextra -g -pthread -Iinclude
LDFLAGS := -pthread -lncursesw

BINDIR := bin
SRCDIR := src

COMMON_SRC := common.c protocol.c menu.c order.c storage.c layout.c
COMMON_OBJ := $(COMMON_SRC:%.c=$(BINDIR)/%.o)

UI_OBJ := $(BINDIR)/ui_common.o $(BINDIR)/ui_pos.o $(BINDIR)/ui_table.o $(BINDIR)/ui_kitchen.o

POS_OBJ := $(COMMON_OBJ) $(BINDIR)/server.o $(UI_OBJ) $(BINDIR)/pos_server.o
TABLE_OBJ := $(COMMON_OBJ) $(BINDIR)/server.o $(UI_OBJ) $(BINDIR)/table_client.o
KITCHEN_OBJ := $(COMMON_OBJ) $(BINDIR)/server.o $(UI_OBJ) $(BINDIR)/kitchen_client.o

.PHONY: all clean run-server run-table run-kitchen test dirs

all: dirs $(BINDIR)/pos_server $(BINDIR)/table_client $(BINDIR)/kitchen_client

dirs:
	@mkdir -p $(BINDIR)

$(BINDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BINDIR)/pos_server: $(POS_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BINDIR)/table_client: $(TABLE_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BINDIR)/kitchen_client: $(KITCHEN_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BINDIR)/test_menu.o: tests/test_menu.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BINDIR)/test_order.o: tests/test_order.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BINDIR)/test_protocol.o: tests/test_protocol.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BINDIR)/test_menu: $(BINDIR)/test_menu.o $(BINDIR)/menu.o
	$(CC) -o $@ $^ $(LDFLAGS)

$(BINDIR)/test_order: $(BINDIR)/test_order.o $(BINDIR)/order.o $(BINDIR)/menu.o $(BINDIR)/common.o
	$(CC) -o $@ $^ $(LDFLAGS)

$(BINDIR)/test_protocol: $(BINDIR)/test_protocol.o $(BINDIR)/protocol.o $(BINDIR)/common.o
	$(CC) -o $@ $^ $(LDFLAGS)

test: dirs $(BINDIR)/test_menu $(BINDIR)/test_order $(BINDIR)/test_protocol
	./$(BINDIR)/test_menu
	./$(BINDIR)/test_order
	./$(BINDIR)/test_protocol

clean:
	rm -rf $(BINDIR)

HOST ?= 127.0.0.1
PORT ?= 9090
TABLE ?= 1

run-server:
	./$(BINDIR)/pos_server $(PORT)

run-table:
	./$(BINDIR)/table_client $(HOST) $(PORT) $(TABLE)

run-kitchen:
	./$(BINDIR)/kitchen_client $(HOST) $(PORT)
