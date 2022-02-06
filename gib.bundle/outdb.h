#pragma once
#include "reader.h"
#include "graph.h"

typedef struct
{
    span_t data;
    char name[];
} dyn_t;

typedef struct
{
    int fd;
    int ptr;
    const char *file;
    char *tmp;
    char buffer[ BUFFER ];
} writer_t;

bool writer_flush( writer_t *w )
{
    int wrote = write( w->fd, w->buffer, w->ptr );
    if ( wrote < 0 )
        sys_error( "writing %s", w->tmp );
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
            sys_error( "vsnprintf" );

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
            sys_error( "writing %s", w->tmp );
        span.str += bytes;
    }
    else
    {
        while ( span_len( span ) > BUFFER - w->ptr )
            writer_flush( w );

        span_copy( w->buffer + w->ptr, span );
        w->ptr += span_len( span );
    }
}

void writer_open( writer_t *w, const char *path )
{
    if ( asprintf( &w->tmp, "%s.%d", path, getpid() ) < 0 )
        sys_error( "asprintf" );

    w->file = path;
    w->ptr = 0;
    w->fd = open( w->tmp, O_CREAT | O_WRONLY | O_TRUNC | O_EXCL, 0666 );

    if ( w->fd < 0 )
        sys_error( "creating %s", w->tmp );
}

void writer_close( writer_t *w )
{
    while ( writer_flush( w ) );

    if ( rename( w->tmp, w->file ) == -1 )
        sys_error( "renaming %s to %s", w->tmp, w->file );
    close( w->fd );
    free( w->tmp );
}

void save_dynamic( cb_tree *dyn, const char *path )
{
    writer_t w;
    writer_open( &w, path );

    for ( cb_iterator i = cb_begin( dyn ); !cb_end( &i ); cb_next( &i ) )
    {
        dyn_t *dyn = cb_get( &i );
        writer_print( &w, "out %s\n", dyn->name );
        writer_append( &w, dyn->data );
        writer_print( &w, "\n" );
    }

    writer_close( &w );
}

void load_dynamic( cb_tree *nodes, cb_tree *dyn, const char *path )
{
    reader_t r;
    node_t *n = NULL;

    char *buf = NULL;
    int buf_size = 0;
    int buf_ptr = 0;

    if ( !reader_init( &r, AT_FDCWD, path ) )
    {
        if ( errno == ENOENT )
            return;
        else
            sys_error( "reading %s", path );
    }

    while ( true )
    {
        bool done = !read_line( &r );

        if ( span_empty( r.span ) && n )
        {
            dyn_t *di = malloc( VSIZE( di, name ) + strlen( n->name ) + 1 );
            strcpy( di->name, n->name );
            di->data = span_mk( buf, buf + buf_ptr );
            cb_insert( dyn, di, VSIZE( di, name ), -1 );

            buf = NULL;
            buf_ptr = buf_size = 0;
            n = NULL;
            continue;
        }

        if ( done )
            break;

        span_t line = r.span;
        span_t word = fetch_word( &r.span );

        if ( span_eq( word, "out" ) )
            if ( !( n = graph_get( nodes, r.span ) ) )
                error( "%s:%d: node %.*s does not exist",
                       path, r.pos.line, span_len( r.span ), r.span.str  );

        if ( span_eq( word, "dep" ) )
        {
            if ( !n )
                error( "%s:%d: dep not part of an 'out' block", path, r.pos.line );

            graph_add_dep( nodes, n, r.span );

            if ( buf_ptr + span_len( line ) >= buf_size )
                buf = realloc( buf, buf_size += buf_size + span_len( line ) + 1 );
            span_copy( buf + buf_ptr, line );
            buf_ptr += span_len( line );
            buf[ buf_ptr++ ] = '\n';
        }
    }
}

void write_stamps( cb_tree *nodes, const char *path )
{
    writer_t w;
    writer_open( &w, path );

    for ( cb_iterator i = cb_begin( nodes ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *n = cb_get( &i );
        span_t name = span_lit( n-> name );

        if ( n->type != out_node )
            continue;

        writer_print( &w, "%08llx %08llx ", n->stamp_updated, n->stamp_changed );
        writer_append( &w, name );
        writer_append( &w, span_lit( "\n" ) );
    }

    writer_close( &w );
}

void load_stamps( cb_tree *nodes, const char *file )
{
    reader_t r;

    if ( !reader_init( &r, AT_FDCWD, file ) )
    {
        if ( errno == ENOENT )
            return;
        else
            sys_error( "opening %s", file );
    }

    while ( read_line( &r ) )
    {
        span_t path = r.span;
        span_t updated = fetch_word( &path );
        span_t changed = fetch_word( &path );

        node_t *node = graph_get( nodes, path );
        if ( !node )
            node = graph_add( nodes, path );

        if ( !fetch_int( &updated, 16, &node->stamp_updated ) ||
             !fetch_int( &changed, 16, &node->stamp_changed ) )
            error( "%s:%d: error reading timestamp(s)", file, r.pos.line );

        node->stamp_want = node->stamp_updated;
    }
}
