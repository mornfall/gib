#pragma once
#include "reader.h"

void load_dynamic( cb_tree *nodes, const char *path )
{}

void write_stamps( cb_tree *nodes, const char *path )
{
    char buffer[ BUFFER ];
    int ptr = 0;

    /* FIXME write new + rename */
    int fd = open( path, O_CREAT | O_WRONLY | O_TRUNC, 0666 );
    if ( fd < 0 )
        sys_error( "creating %s", path );

    for ( cb_iterator i = cb_begin( nodes ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *n = cb_get( &i );
        span_t name = span_lit( n-> name );

        if ( n->type != out_node )
            continue;

        while ( true )
        {
            int need = snprintf( buffer + ptr, BUFFER - ptr, "%llx ", n->stamp );

            if ( need + span_len( name ) < BUFFER - ptr )
            {
                ptr += need;
                break;
            }

            int wrote = write( fd, buffer, ptr );
            if ( wrote < 0 )
                sys_error( "writing %s", path );
            ptr -= wrote;
            memmove( buffer, buffer + wrote, ptr );
        }

        memcpy( buffer + ptr, name.str, name.end - name.str );
        ptr += span_len( name );
        buffer[ ptr++ ] = '\n';
    }

    int wptr = 0;

    while ( wptr < ptr )
    {
        int wrote = write( fd, buffer + wptr, ptr - wptr );
        if ( wrote < 0 )
            sys_error( "writing %s", path );
        wptr += wrote;
    }

    close( fd );
}

void load_stamps( cb_tree *nodes, const char *file )
{
    reader_t r;

    if ( !reader_init( &r, file ) )
    {
        if ( errno == ENOENT )
            return;
        else
            sys_error( "opening %s", file );
    }

    while ( fetch_line( &r ) )
    {
        span_t path = r.span;
        span_t stamp = fetch_word( &path );
        char *endptr;

        errno = 0;
        uint64_t value = strtoll( stamp.str, &endptr, 16 );
        assert( !errno );
        assert( endptr == stamp.end );

        node_t *node = graph_get( nodes, path );
        if ( !node )
            node = graph_add( nodes, path );

        node->stamp = value;
    }
}
