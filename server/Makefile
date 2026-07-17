# Compiler and flags setup
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -g -Wall -Werror
LDFLAGS ?= 
TARGET = aesdsocket

# Required targets
all: $(TARGET)

default: $(TARGET)

# Compile the executable
$(TARGET): aesdsocket.o
	$(CC) $(CFLAGS) -o $(TARGET) aesdsocket.o $(LDFLAGS)

# Compile the object file
aesdsocket.o: aesdsocket.c
	$(CC) $(CFLAGS) -c aesdsocket.c

# Cleanup
clean:
	rm -f $(TARGET) *.o
