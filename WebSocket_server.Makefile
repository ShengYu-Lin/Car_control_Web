CC ?= cc

CFLAGS ?= -O2 -Wall -Wextra -Werror -pthread
CPPFLAGS += $(shell pkg-config --cflags libwebsockets json-c)
LDLIBS += $(shell pkg-config --libs libwebsockets json-c)

TARGET = WebSocket_server

all: $(TARGET)

$(TARGET): WebSocket_server.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) \
		-o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
