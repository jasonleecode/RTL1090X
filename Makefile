CXX ?= c++
CXXFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local
rtl1090x: rtl1090x.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 $< $(LDFLAGS) -lX11 -ljson-c -o $@
install: rtl1090x
	install -Dm755 rtl1090x $(DESTDIR)$(PREFIX)/bin/rtl1090x
	install -Dm644 rtl1090x.desktop $(DESTDIR)$(PREFIX)/share/applications/rtl1090x.desktop
.PHONY: install
