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

GOALS    = $(.TARGETS) $(MAKECMDGOALS)
TARGETS += .gib.bin
MATCHED  = $(TARGETS:M$(GOALS))
TODO     = $(GOALS:N$(MATCHED))

all $(.TARGETS) $(TODO): .gib.bin
	@./.gib.bin $@

.gib.bin: $(SRC) $(HDR)
	@cc -O2 -g -o .gib.bin $(SRC)

.PHONY: $(TODO) all
