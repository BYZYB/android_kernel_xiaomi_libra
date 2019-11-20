# Makefile for cgroup tools

CC = $(CROSS_COMPILE)gcc

all: cgroup_event_listener
%: %.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	$(RM) cgroup_event_listener
