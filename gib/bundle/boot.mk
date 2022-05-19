HDR = gib/bundle/span.h \
      gib/bundle/critbit.h \
      gib/bundle/common.h \
      gib/bundle/env.h \
      gib/bundle/graph.h \
      gib/bundle/job.h \
      gib/bundle/queue.h \
      gib/bundle/reader.h \
      gib/bundle/writer.h \
      gib/bundle/manifest.h \
      gib/bundle/outdb.h \
      gib/bundle/rules.h
SRC = gib/bundle/main.c gib/bundle/sha1.c

# clear builtin suffix rules
.SUFFIXES:

# make sure we do not try to use gib to build these
$(HDR): ;
$(SRC): ;
gib/bundle/boot.mk: ;
makefile: ;

# building gib is easy-ish
.gib.bin: $(SRC) $(HDR)
	@sh gib/bundle/c99.sh -O2 -g -o .gib.bin $(SRC)

# openbsd make does not like prerequisites on .DEFAULT
.BEGIN: .gib.bin
.DEFAULT:
	@./.gib.bin $@

# gnumake would be okay with dep on .DEFAULT but does not know .BEGIN
%: .gib.bin
	@./.gib.bin $@
