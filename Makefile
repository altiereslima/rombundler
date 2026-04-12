TARGET := rombundler
VERSION ?= devel
UNAME_S := $(shell uname -s 2>NUL)

ifeq ($(OS),Windows_NT) # win
	TARGET := rombundler.exe
	CFLAGS += -IC:/glfw-3.3.4.bin.WIN64/include -IC:/openal-soft-1.21.0-bin/include
	LDFLAGS += -L./lib -L"C:\glfw-3.3.4.bin.WIN64/lib-mingw-w64" -L"C:\openal-soft-1.21.0-bin/libs/Win64" -static -lglfw3 -lopengl32 -lvulkan-1 -lgdi32 -luser32 -lkernel32 -lshell32 -lwinmm -lOpenAL32 -mwindows -static-libgcc -static-libstdc++
	OS ?= Windows
else ifneq ($(findstring MINGW,$(UNAME_S)),) # win
	TARGET := rombundler.exe
	CFLAGS += -IC:/glfw-3.3.4.bin.WIN64/include -IC:/openal-soft-1.21.0-bin/include
	LDFLAGS += -L./lib -L"C:\glfw-3.3.4.bin.WIN64/lib-mingw-w64" -L"C:\openal-soft-1.21.0-bin/libs/Win64" -static -lglfw3 -lopengl32 -lvulkan-1 -lgdi32 -luser32 -lkernel32 -lshell32 -lwinmm -lOpenAL32 -mwindows -static-libgcc -static-libstdc++
	OS ?= Windows
else ifneq ($(findstring Darwin,$(UNAME_S)),) # osx
	LDFLAGS := -Ldeps/osx_$(shell uname -m)/lib -lglfw3 -framework Cocoa -framework OpenGL -framework IOKit
	LDFLAGS += -framework OpenAL
	OS ?= OSX
else
	LDFLAGS := -ldl
	LDFLAGS += $(shell pkg-config --libs glfw3)
	LDFLAGS += $(shell pkg-config --libs openal)
	LDFLAGS += -lvulkan -pthread
	CFLAGS += -pthread
	OS ?= Linux
endif

CFLAGS += -Wall -O3 -fPIC -flto -I. -Iinclude -Ideps/include

OBJ = main.o glad.o config.o core.o audio.o video.o video_vulkan.o input.o options.o ini.o utils.o srm.o menu.o font.o remap.o lang.o aspect.o

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

.PHONY: all clean

all: $(TARGET)
$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) -flto

bundle: $(TARGET)
	mkdir -p ROMBundler-$(OS)-$(VERSION)
	cp $(TARGET) ROMBundler-$(OS)-$(VERSION)
	cp *.dll ROMBundler-$(OS)-$(VERSION) || :
	cp config.ini ROMBundler-$(OS)-$(VERSION)
	cp README.md ROMBundler-$(OS)-$(VERSION)
	cp COPYING ROMBundler-$(OS)-$(VERSION)
	zip -r ROMBundler-$(OS)-$(VERSION).zip ROMBundler-$(OS)-$(VERSION)

clean:
	rm -rf $(OBJ) $(TARGET) ROMBundler-*
