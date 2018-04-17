INCLUDEDIR = include

CFLAGS = -Wall -Wextra -I$(INCLUDEDIR) -lpthread -DDEBUG

HEADERS = $(INCLUDEDIR)/uffdw.h

build/test/page_repeat: $(HEADERS) src/uffdw.c test/page_repeat.c
	$(CC) $(CFLAGS) -o $@ src/uffdw.c test/page_repeat.c

clean:
	rm -f build/test/page_repeat
