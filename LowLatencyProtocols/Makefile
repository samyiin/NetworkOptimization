CC = gcc
CFLAGS = -Wall -g

# link to library
LDFLAGS = -libverbs

# source files (.c files)
SRC = ib_functions.c main.c kv_api.c

# object files (.o files): For each .c file, their .o file name will be exactly the same except for the extension.
OBJ = $(SRC: .c=.o)

# the name of the compiled executable
TARGET = server
# we will compile another executable with the same code. So we will use symbolic link
CLIENT_LINK = client

.PHONY: all clean

all: $(TARGET)
	@ln -sf $(TARGET) $(CLIENT_LINK)

$(TARGET): $(OBJ)
	$(CC) $^ $(LDFLAGS) -o $@

# Define pattern rule to compile .c files to .o files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@rm -f $(TARGET) $(CLIENT_LINK)
