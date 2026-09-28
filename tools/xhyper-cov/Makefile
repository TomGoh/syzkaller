all: libxhcov.so

libxhcov.so: libxhcov.c
	gcc -shared -fPIC -Wall -Wextra -I. $(shell pkg-config --cflags glib-2.0) libxhcov.c -o libxhcov.so $(shell pkg-config --libs glib-2.0)

strict: libxhcov.c
	gcc -shared -fPIC -Wall -Wextra -I. $(shell pkg-config --cflags glib-2.0) libxhcov.c -o /dev/null $(shell pkg-config --libs glib-2.0)

selftest: libxhcov.so
	./selftest.sh

clean:
	rm -f libxhcov.so payload.bin st_payload.o st_payload.elf st_payload.S
	rm -f st_el2.* st_el1.*

.PHONY: all strict selftest clean
