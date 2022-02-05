HDR = gib/span.h gib/critbit.h gib/common.h gib/env.h gib/graph.h gib/job.h gib/reader.h \
      gib/manifest.h gib/outdb.h gib/rules.h

all: .aux/gib
	./.aux/gib all

gib.c: gib/main.c $(HDR) makefile
	cat $(HDR) gib/main.c | egrep -v '#pragma|#include "' > $@

$(.TARGETS) $(MAKECMDGOALS): .aux/gib
	./.aux/gib $@

.aux/gib: gib/main.c $(HDR)
	mkdir -p .aux
	cc -g -o .aux/gib gib/main.c
