INCLUDEDIR = include

CFLAGS = -Wall -Wextra -I$(INCLUDEDIR) -lpthread -DDEBUG

HEADERS = $(INCLUDEDIR)/uffdw.h
TESTS = page_repeat fork

TEST_BINARIES = $(addprefix build/test/,$(TESTS))

all: test

test: $(TEST_BINARIES)
	for TEST in $(TEST_BINARIES); do $$TEST; done

$(TEST_BINARIES): build/test/%: $(HEADERS) src/uffdw.c test/%.c
	mkdir -p build/test
	$(CC) $(CFLAGS) -o $@ src/uffdw.c test/$*.c

clean:
	rm -f build/test/*
