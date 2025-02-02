# Compiler and flags
CC = clang
CFLAGS = -std=c23 -O3 -Wextra -Wall -Wno-unused-parameter -Wpedantic
INCLUDES = -I/home/can/Downloads/raylib/include
LDFLAGS = -L/home/can/Downloads/raylib/lib -lm -lraylib -Wl,-rpath=/home/can/Downloads/raylib/lib

# Dependencies (using pkg-config)
DEPS = $(shell pkg-config --cflags --libs dbus-1 libpipewire-0.3 gstreamer-1.0)

# Use below to overwrite screen size if the app can't auto detect
CFLAGS += -DSCREEN_WIDTH=5210 -DSCREEN_HEIGHT=2880

# Use below to add additional padding to account for the GNOME top bar
CFLAGS += -DGNOME_TOP_BAR=60

# Source file
SRC = record_area.c

# Output executable
TARGET = record_area

# Default target
all: $(TARGET)

# Rule to compile and link directly
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPS) $(SRC) -o $(TARGET) $(LDFLAGS)

# Clean target to remove the executable
clean:
	rm -f $(TARGET)

# Phony targets
.PHONY: all clean