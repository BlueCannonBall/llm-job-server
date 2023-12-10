CXX = g++
CXXFLAGS = -Wall -std=c++14 -Ofast -pthread
LDLIBS = -lcrypto
HEADERS = $(shell find . -name "*.hpp")
OBJDIR = obj
OBJS = $(OBJDIR)/main.o $(OBJDIR)/polynet.o $(OBJDIR)/polyweb.o $(OBJDIR)/polyweb_string.o
PREFIX = /usr/local
TARGET = server

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(CXXFLAGS) $(LDLIBS) -o $@

$(OBJDIR)/main.o: main.cpp $(HEADERS)
	mkdir -p $(OBJDIR)
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJDIR)/polynet.o: Polyweb/Polynet/polynet.cpp Polyweb/Polynet/polynet.hpp
	mkdir -p $(OBJDIR)
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJDIR)/polyweb.o: Polyweb/polyweb.cpp Polyweb/polyweb.hpp Polyweb/Polynet/polynet.hpp Polyweb/string.hpp Polyweb/threadpool.hpp
	mkdir -p $(OBJDIR)
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJDIR)/polyweb_string.o: Polyweb/string.cpp Polyweb/string.hpp
	mkdir -p $(OBJDIR)
	$(CXX) -c $< $(CXXFLAGS) -o $@

.PHONY: clean install

clean:
	rm -rf $(TARGET) $(OBJDIR)

install:
	cp $(TARGET) $(PREFIX)/bin/
