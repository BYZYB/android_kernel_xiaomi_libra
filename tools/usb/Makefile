# Makefile for USB tools

CC = $(CROSS_COMPILE)gcc
PTHREAD_LIBS = -lpthread
CFLAGS = $(WARNINGS) -g0 $(PTHREAD_LIBS) -I../include

all: testusb ffs-test
%: %.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	$(RM) testusb ffs-test
