#pragma once
#include "span.h"
#include "common.h"

typedef struct
{
    int fd;
    int ptr;
    int dirfd;
    const char *file;
    char *tmp;
    char buffer[ BUFFER ];
} writer_t;

bool writer_flush( writer_t *w )
{
    int wrote = write( w->fd, w->buffer, w->ptr );
    if ( wrote < 0 )
        sys_error( NULL, "writing %s", w->tmp );
    w->ptr -= wrote;
    memmove( w->buffer, w->buffer + wrote, w->ptr );
    return w->ptr > 0;
}

int writer_print( writer_t *w, const char *fmt, ... )
{
    va_list ap;

    while ( true )
    {
        va_start( ap, fmt );
        int need = vsnprintf( w->buffer + w->ptr, BUFFER - w->ptr, fmt, ap );
        va_end( ap );

        if ( need < 0 )
            sys_error( NULL, "vsnprintf" );

        if ( need >= BUFFER - w->ptr )
            writer_flush( w );
        else
            return w->ptr += need, need;
    }
}

void writer_append( writer_t *w, span_t span )
{
    if ( span_len( span ) >= BUFFER )
    {
        while ( writer_flush( w ) );
        int bytes = write( w->fd, span.str, span_len( span ) );
        if ( bytes < 0 )
            sys_error( NULL, "writing %s", w->tmp );
        span.str += bytes;
    }
    else
    {
        while ( span_len( span ) >= BUFFER - w->ptr )
            writer_flush( w );

        span_copy( w->buffer + w->ptr, span );
        w->ptr += span_len( span );
    }
}

void writer_open( writer_t *w, int dirfd, const char *path )
{
    if ( asprintf( &w->tmp, "%s.%d", path, getpid() ) < 0 )
        sys_error( NULL, "asprintf" );

    w->dirfd = dirfd;
    w->file = path;
    w->ptr = 0;
    w->fd = openat( dirfd, w->tmp, O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0666 );

    if ( w->fd < 0 )
        sys_error( NULL, "creating %s", w->tmp );
}

void writer_close( writer_t *w )
{
    while ( writer_flush( w ) );

    if ( renameat( w->dirfd, w->tmp, w->dirfd, w->file ) == -1 )
        sys_error( NULL, "renaming %s to %s", w->tmp, w->file );
    close( w->fd );
    free( w->tmp );
}


