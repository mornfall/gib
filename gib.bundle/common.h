#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <assert.h>

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

void die( const char *reason, ... )
{
    const char *err = strerror( errno );

    va_list ap;
    va_start( ap, reason );
    vfprintf( stderr, reason, ap );
    fprintf( stderr, ": %s\n", err );
    va_end( ap );
    exit( 1 );
}
