# cdcstore: chunked, deduplicated file repository (one file per chunk) with an index.
#   cdcstore-add      store files (to a DEST repository, or IN PLACE with -X)
#   cdcstore-remove   remove files (chunks nobody uses are deleted)
#   cdcstore-restore  restore everything, a folder, a file or wildcards
#   cdcstore-list     list contents, -s statistics, -k the chunk files for a sorter
#   cdcstore-verify   read everything back and check it (-c DIR compares with originals)
#   cdcstore-mount    mount the repository read-only (FUSE)

# Needs only gcc. No compression here: chunks are plain files; sort them (sortof-*) and archive them (tar|zstd, zfrag, ...).
# To add a program: write cdcstore-NAME.c that includes cdcstore-common.h and add NAME to PROGS.
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local
MANDIR  ?= $(PREFIX)/share/man

PROGS = add remove restore list verify mount
BINS  = $(addprefix cdcstore-,$(PROGS))

all: $(BINS)

cdcstore-common.o: cdcstore-common.c cdcstore-common.h
	$(CC) $(CFLAGS) -c -o $@ $<

cdcstore-%: cdcstore-%.c cdcstore-common.o cdcstore-common.h
	$(CC) $(CFLAGS) -o $@ $< cdcstore-common.o

# the mount needs libfuse3 (apt install libfuse3-dev pkg-config)
cdcstore-mount: cdcstore-mount.c cdcstore-common.o cdcstore-common.h
	$(CC) $(CFLAGS) $(shell pkg-config --cflags fuse3) -pthread -o $@ $< cdcstore-common.o $(shell pkg-config --libs fuse3)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BINS) $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(MANDIR)/man1
	install -m 644 man/*.1 $(DESTDIR)$(MANDIR)/man1
	install -d $(DESTDIR)$(MANDIR)/man7
	install -m 644 man/*.7 $(DESTDIR)$(MANDIR)/man7

clean:
	rm -f $(BINS) cdcstore-common.o

.PHONY: all install clean
