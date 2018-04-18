INCLUDEDIR = include

CFLAGS = -Wall -Wextra -I$(INCLUDEDIR) -lpthread -DDEBUG

HEADERS = $(INCLUDEDIR)/uffdw.h

all: build/test/page_repeat build/test/fork

build/test/page_repeat: $(HEADERS) src/uffdw.c test/page_repeat.c
	$(CC) $(CFLAGS) -o $@ src/uffdw.c test/page_repeat.c

build/test/fork: $(HEADERS) src/uffdw.c test/fork.c
	$(CC) $(CFLAGS) -o $@ src/uffdw.c test/fork.c

clean:
	rm -f build/test/page_repeat
