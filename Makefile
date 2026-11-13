CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
EASYCAP ?= /opt/magewell/libeasycap
EASY_CXXFLAGS := -std=c++17 -O2 -w -DNDEBUG -I$(EASYCAP)/include -I$(EASYCAP)/include_private

SDK_SRC := CEasyCapS.cpp CShm.cpp gentl_capture.cpp load_gentl_cti.cpp load_gentl_cti_ex.cpp
SDK_OBJ := $(addprefix build/,$(SDK_SRC:.cpp=.o))

.PHONY: all clean

all: grabercam graberview

build/%.o: $(EASYCAP)/src/%.cpp
	mkdir -p build
	$(CXX) $(EASY_CXXFLAGS) -c $< -o $@

build/easycap.a: $(SDK_OBJ)
	ar rcs $@ $(SDK_OBJ)

READLINE_LIB ?= -l:libreadline.so.8

grabercam: src/main.cpp src/session.cpp src/session.hpp build/easycap.a
	$(CXX) $(CXXFLAGS) -I$(EASYCAP)/include -o $@ src/main.cpp src/session.cpp build/easycap.a -ldl -lpthread -lrt $(READLINE_LIB)

graberview: src/view.cpp src/session.cpp src/session.hpp build/easycap.a
	$(CXX) $(CXXFLAGS) -I$(EASYCAP)/include -o $@ src/view.cpp src/session.cpp build/easycap.a -ldl -lpthread -lrt

clean:
	rm -rf build grabercam graberview
