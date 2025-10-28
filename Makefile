# Makefile for FT600 Speed Test Application with PainterEngine
# Windows (MinGW/MSVC)

# Compilers
CC = gcc
CXX = g++

# Target executables
TARGET_CONSOLE = ft600_speed_test.exe
TARGET_GUI = ft600_display.exe
TARGETS = $(TARGET_CONSOLE) $(TARGET_GUI)

# Source files for console version
SOURCES_CONSOLE = ft600_speed_test.c

# Source files for GUI version
SOURCES_GUI = ft600_display.c platform/windows/px_main.c

# PainterEngine library sources
PE_SOURCES = $(wildcard core/*.c) \
	$(wildcard kernel/*.c) \
	$(wildcard runtime/*.c) \
	$(filter-out %/px_main.c, $(wildcard platform/windows/*.c))

PE_CPP_SOURCES = $(filter-out %/px_main.cpp, $(wildcard platform/windows/*.cpp))

# Object files
OBJECTS_CONSOLE = $(SOURCES_CONSOLE:.c=.o)
OBJECTS_GUI = $(SOURCES_GUI:.c=.o) $(PE_SOURCES:.c=.o) $(PE_CPP_SOURCES:.cpp=.o)
OBJECTS_PE = $(PE_SOURCES:.c=.o) $(PE_CPP_SOURCES:.cpp=.o)

# Include directories
INCLUDES = -I. -I./platform

# Library directories
LIBDIRS = -L.

# Libraries to link
LIBS = -lFTD3XXWU -lgdi32 -luser32 -lwinmm -lopengl32 -lws2_32 -lcomdlg32 -ldsound -limm32 -ld2d1

# Compiler flags
CFLAGS = -Wall -O2 $(INCLUDES)
CXXFLAGS = -Wall -O2 $(INCLUDES)

# Linker flags
LDFLAGS = $(LIBDIRS) $(LIBS)

# Default target
all: $(TARGETS)

# Link console executable
$(TARGET_CONSOLE): $(OBJECTS_CONSOLE)
	@echo Linking $(TARGET_CONSOLE)...
	$(CC) -Wl,--subsystem,console -o $(TARGET_CONSOLE) $(OBJECTS_CONSOLE) $(LDFLAGS)
	@echo Build complete: $(TARGET_CONSOLE)

# Link GUI executable
$(TARGET_GUI): $(OBJECTS_GUI)
	@echo Linking $(TARGET_GUI)...
	$(CXX) -mwindows -o $(TARGET_GUI) $(OBJECTS_GUI) -Wl,--allow-multiple-definition $(LDFLAGS)
	@echo Build complete: $(TARGET_GUI)

# Compile C source files
%.o: %.c
	@echo Compiling $<...
	$(CC) $(CFLAGS) -c $< -o $@

# Compile C++ source files
%.o: %.cpp
	@echo Compiling $<...
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo Cleaning build artifacts...
	del /Q $(subst /,\,$(OBJECTS_CONSOLE)) $(subst /,\,$(OBJECTS_PE)) $(TARGETS) 2>nul || echo No files to clean

# Rebuild
rebuild: clean all

# Run the GUI application
run: $(TARGET_GUI)
	@echo Running $(TARGET_GUI)...
	.\$(TARGET_GUI)

# Run the console application
run-console: $(TARGET_CONSOLE)
	@echo Running $(TARGET_CONSOLE)...
	.\$(TARGET_CONSOLE)

# Install (copy DLL to executable directory if needed)
install: $(TARGET)
	@echo Checking for FTD3XXWU.dll...
	@if not exist FTD3XXWU.dll echo Warning: FTD3XXWU.dll not found in current directory

# Help target
help:
	@echo FT600 Speed Test Application - Makefile
	@echo.
	@echo Available targets:
	@echo   all      - Build the application (default)
	@echo   clean    - Remove build artifacts
	@echo   rebuild  - Clean and build
	@echo   run      - Build and run the application
	@echo   install  - Check for required DLL
	@echo   help     - Show this help message
	@echo.
	@echo Requirements:
	@echo   - FTD3XX.h (header file)
	@echo   - FTD3XXWU.lib (import library)
	@echo   - FTD3XXWU.dll (runtime library)
	@echo   - MinGW GCC or compatible compiler
	@echo   - PainterEngine library files

.PHONY: all clean rebuild run install help
