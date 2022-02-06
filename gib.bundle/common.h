#pragma once
#define _GNU_SOURCE
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

/* TODO error handling? */

void sys_error( const char *reason, ... )
{
    const char *err = strerror( errno );

    va_list ap;
    va_start( ap, reason );
    vfprintf( stderr, reason, ap );
    fprintf( stderr, ": %s\n", err );
    va_end( ap );
    exit( 1 );
}

void error( const char *reason, ... )
{
    va_list ap;
    va_start( ap, reason );
    vfprintf( stderr, reason, ap );
    fprintf( stderr, "\n" );
    va_end( ap );
    exit( 2 );
}
