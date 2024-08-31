#CC = gcc
#CFLAGS = -libverbs
#
#all: server client
#
#server: bw_template.c utils.h
#	$(CC) $(CFLAGS) bw_template.c utils.h -o server
#
#client: bw_template.c utils.h
#	$(CC) $(CFLAGS) bw_template.c utils.h -o client
#
#clean:
#	rm -f server client
CC = gcc
CFLAGS = -libverbs
TARGET = server
CLIENT_LINK = client
HEADER_FILE = utils.h

.PHONY: all clean

all: $(TARGET)
	ln -s $(TARGET) $(CLIENT_LINK)

$(TARGET): bw_template.c $(HEADER_FILE)
	$(CC) $< $(HEADER_FILE) $(CFLAGS) -o $@

clean:
	rm -f $(TARGET) $(CLIENT_LINK)