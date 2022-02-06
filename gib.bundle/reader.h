#pragma once
#include "common.h"
#include "span.h"
#define BUFFER 8192

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

bool reader_init( reader_t *r, int dir_fd, const char *file )
{
    r->fd = openat( dir_fd, file, O_RDONLY );
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

bool read_line( reader_t *r )
{
    r->span.str = r->span.end;
    r->span.end = r->span.str;

    do
    {
        if ( r->span.end >= r->buffer + r->buffer_use - 1 )
            if ( !shift_buffer( r ) )
                return false;
        r->span.end ++;
    }
    while ( *r->span.end != '\n' );

    if ( r->span.str < r->span.end && *r->span.str == '\n' )
        r->span.str ++;

    r->pos.line ++;

    return true;
}

span_t fetch_until( span_t *in, char c, char esc )
{
    span_t r = *in;
    bool skip = false;

    while ( in->str < in->end && ( esc && skip || *in->str != c ) )
    {
        skip = *in->str == esc;
        ++ in->str;
    }

    r.end = in->str;

    while ( in->str < in->end && *in->str == c )
        in->str ++;

    return r;
}

span_t fetch_line( span_t *in )
{
    return fetch_until( in, '\n', 0 );
}

span_t fetch_word( span_t *in )
{
    return fetch_until( in, ' ', 0 );
}

span_t fetch_word_escaped( span_t *in )
{
    return fetch_until( in, ' ', '\\' );
}

bool fetch_int( span_t *in, int base, int64_t *result ) /* sigh */
{
    char buffer[ span_len( *in ) + 1 ], *endptr;
    span_copy( buffer, *in );
    errno = 0;
    *result = strtoll( buffer, &endptr, base );
    in->str += endptr - buffer;
    return errno == 0;
}
