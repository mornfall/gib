#pragma once
#include "common.h"
#include "env.h"
#include "graph.h"

#define BUFFER 4096

typedef struct fileline
{
    const char*file;
    int line;
    char name[0];
} fileline_t;

typedef struct reader
{
    int fd;
    char buffer[ BUFFER ];
    int buffer_use;
    span_t span;
    fileline_t pos;
} reader_t;

bool reader_init( reader_t *r, const char *file )
{
    r->fd = open( file, O_RDONLY );
    r->span.str = r->span.end = r->buffer;
    r->buffer_use = 0;

    r->pos.file = file;
    r->pos.line = 0;

    return r->fd >= 0;
}

bool shift_buffer( reader_t *r )
{
    assert( r->buffer + r->buffer_use >= r->span.str );
    int delete = r->span.str - r->buffer;
    r->buffer_use -= delete;

    if ( r->buffer_use == BUFFER )
        error( "ran out of line buffer reading %s, line %d", r->pos.file, r->pos.line );

    memmove( r->buffer, r->span.str, r->buffer_use );

    r->span.str -= delete;
    r->span.end -= delete;

    int bytes = read( r->fd, r->buffer + r->buffer_use, BUFFER - r->buffer_use );

    if ( bytes == 0 )
        return false;

    if ( bytes < 0 )
    {
        perror( "read" );
        abort();
    }

    r->buffer_use += bytes;
    return true;
}

bool fetch_line( reader_t *r )
{
    r->span.str = r->span.end;
    r->span.end = r->span.str;

    do
    {
        if ( r->span.end >= r->buffer + r->buffer_use )
            if ( !shift_buffer( r ) )
                return false;
        r->span.end ++;
    }
    while ( *r->span.end != '\n' );

    if ( *r->span.str == '\n' )
        r->span.str ++;

    r->pos.line ++;

    return true;
}

span_t fetch_word( span_t *in )
{
    span_t r = *in;

    while ( in->str < in->end && *in->str != ' ' )
        ++ in->str;

    r.end = in->str;

    while ( *in->str == ' ' )
        in->str ++;

    return r;
}
