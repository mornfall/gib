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

all $(.TARGETS) $(MAKECMDGOALS): .gib.bin
	@./.gib.bin $@

.gib.bin: $(SRC) $(HDR)
	@cc -g -o .gib.bin $(SRC)

.PHONY: $(.TARGETS) $(MAKECMDGOALS) all
