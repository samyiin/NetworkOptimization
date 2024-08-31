CC = gcc
CFLAGS = -libverbs
TARGET = server
CLIENT_LINK = client
HEADER_FILE = utils.h

.PHONY: all clean

all: $(TARGET)
	ln -s $(TARGET) $(CLIENT_LINK)

$(TARGET): kv_store_final.c $(HEADER_FILE)
	$(CC) $< $(HEADER_FILE) $(CFLAGS) -o $@

clean:
	rm -f $(TARGET) $(CLIENT_LINK)