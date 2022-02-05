HDR = gib/critbit.h gib/common.h gib/env.h gib/graph.h gib/job.h gib/reader.h

all: .aux/gib
	./.aux/gib all

$(.TARGETS) $(MAKECMDGOALS): .aux/gib
	./.aux/gib $@

.aux/gib: gib/main.c $(HDR)
	mkdir -p .aux
	cc -g -o .aux/gib gib/main.c

gib.c: gib/main.c $(HDR)
	cat $(HDR) gib/main.c | egrep -v '#pragma|#include "' > $@
