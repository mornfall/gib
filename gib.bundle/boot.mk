HDR = gib.bundle/span.h \
      gib.bundle/critbit.h \
      gib.bundle/common.h \
      gib.bundle/env.h \
      gib.bundle/graph.h \
      gib.bundle/job.h \
      gib.bundle/reader.h \
      gib.bundle/manifest.h \
      gib.bundle/outdb.h \
      gib.bundle/rules.h

$(.TARGETS) $(MAKECMDGOALS): .gib.bin
	@./.gib.bin $@

.gib.bin: gib.bundle/main.c $(HDR)
	@cc -g -o .gib.bin gib.bundle/main.c
