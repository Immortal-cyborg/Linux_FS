# Makefile для userspace-утилиты MyFS
#
# Использование:
#   make              — собрать myfs_tool
#   make clean        — удалить артефакты
#   make install      — установить в /usr/local/bin (требует root)

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I..

TARGET := myfs_tool
SRC    := myfs_tool.cpp

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC) ../myfs.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
