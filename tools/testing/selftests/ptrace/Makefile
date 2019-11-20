CFLAGS += -iquote../../../../include/uapi
peeksiginfo: peeksiginfo.c

all: peeksiginfo

clean:
	rm -f peeksiginfo

run_tests: all
	@./peeksiginfo || echo "peeksiginfo selftests: [FAIL]"
