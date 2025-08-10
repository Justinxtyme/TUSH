# Compiler to use
CC = gcc

# Compiler flags:
# -Wall  → enable all common warnings
# -Wextra → enable extra warnings
# -g     → include debug info for gdb
# -MMD   → generate a .d file listing header dependencies
# -MP    → add "dummy" rules so make won't break if a header is deleted
CFLAGS = -Wall -Wextra -g -MMD -MP

# List of source files
SRC = main.c executor.c builtins.c shell.c

# List of object files (same names but .o instead of .c)
OBJ = $(SRC:.c=.o)

# List of dependency files (same names but .d instead of .o)
DEPS = $(OBJ:.o=.d)

# Final program name
TARGET = tush

# Default target (build the program)
all: $(TARGET)

# Link all object files into the final executable
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET)

# Compile each .c into a .o file
# $<  = first dependency (e.g., main.c)
# $@  = target name (e.g., main.o)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Remove compiled files and the executable
clean:
	rm -f $(OBJ) $(DEPS) $(TARGET)

# Include the auto-generated dependency files (.d)
# The '-' at the start means: don't complain if the files don't exist yet
-include $(DEPS)

# These targets aren't actual files, so mark them as phony
.PHONY: all clean
