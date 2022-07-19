#pragma once
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <assert.h>

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "span.h"
#include "critbit.h"

#define BUFFER 8192
#define SIZE_NAMED( type, len ) max( offsetof( type, name ) + len + 1, sizeof( type ) )

int max( int a, int b ) { return a > b ? a : b; }

typedef struct fileline
{
    const char *file;
    int line;
    char name[0];
} fileline_t;

struct location_stack
{
    fileline_t pos;
    const char *what;
    struct reader *reader;
    struct location_stack *next;
};

typedef struct location
{
    struct location_stack *stack;
    cb_tree names;
} location_t;

void location_vprint( struct location *s, const char *reason, va_list ap );
void location_print( struct location *s, const char *reason, ... )
{
    va_list ap;
    va_start( ap, reason );
    location_vprint( s, reason, ap );
    va_end( ap );
}

bool have_tty()
{
    static int status = -1;

    if ( status < 0 )
        status = isatty( 2 );

    return status;
}

bool tty_print( const char *fmt, ... )
{
    if ( !have_tty() )
        return false;

    fputs( "\r\033[J", stderr );
    va_list ap;
    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    va_end( ap );
    fflush( stderr );
    return true;
}

void error( struct location *s, const char *reason, ... )
{
    va_list ap;
    va_start( ap, reason );
    location_vprint( s, reason, ap );
    va_end( ap );
    exit( 3 );
}

void sys_error( struct location *s, const char *reason, ... )
{
    const char *err = strerror( errno );
    char msg[ 1024 ];

    va_list ap;
    va_start( ap, reason );
    char *ptr = msg + vsnprintf( msg, 1024, reason, ap );
    snprintf( ptr, 1024 - ( ptr - msg ), ": %s", err );
    location_print( s, "%s", msg );
    va_end( ap );
    exit( 1 );
}
