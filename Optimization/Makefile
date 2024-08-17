CC = gcc
CFLAGS = -Wall -g
LDFLAGS = -libverbs

# source files (.c files)
SRC = bw_template.c

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


# Sam's note: For someone unfamiliar with the Makefile syntax:
# OBJ = $(SRC: .c=.o) means OBJ will be "ibv_api.o ibv_pingpong.o"
# this is just string manipulation, to create a string based on SRC
# Where are these .o files? They are not generated during "OBJ = $(SRC: .c=.o)"
# When the make program sees that one of the .o files are missing/outdated, it will execute the "pattern rule" %.o: %.c
