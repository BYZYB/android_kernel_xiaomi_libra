prefix = /usr
nosy-dump-version = 0.4

CC = gcc

all : nosy-dump

nosy-dump : CFLAGS = -O3 -g0
nosy-dump : CPPFLAGS = -DVERSION=\"$(nosy-dump-version)\" -I../../drivers/firewire
nosy-dump : LDFLAGS = -g0
nosy-dump : LDLIBS = -lpopt

nosy-dump : nosy-dump.o decode-fcp.o

clean :
	rm -rf *.o nosy-dump

install :
	install nosy-dump $(prefix)/bin/nosy-dump
