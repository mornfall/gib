#pragma once
#include "reader.h"
#include <sys/stat.h>

void load_manifest( cb_tree *nodes, var_t *src, var_t *dirs, int rootfd, const char *file )
{
    reader_t r;

    if ( !reader_init( &r, rootfd, file ) )
        sys_error( "opening %s", file );

    int dirfd = dup( rootfd );
    span_t dir = span_dup( span_lit( "" ) );

    while ( read_line( &r ) )
    {
        if ( span_empty( r.span ) ) /* skip empty lines */
            continue;

        span_t path = r.span;
        span_t op = fetch_word( &path );

        if ( span_empty( op ) || *op.str != 'd' && *op.str != 'f' )
            error( "%s:%d: malformed line", r.pos.file, r.pos.line );

        if ( *op.str == 'd' )
        {
            if ( close( dirfd ) ) sys_error( "closing fd %d", dirfd );
            span_free( dir );
            dir = span_dup( path );
            if ( ( dirfd = open( dir.str, O_DIRECTORY | O_RDONLY ) ) == -1 )
                sys_error( "%s:%d: opening %s", r.pos.file, r.pos.line, dir );
            var_add( dirs, dir );
        }
        else
        {
            int slash = span_len( dir ) ? 1 : 0;
            int len = span_len( dir ) + span_len( path ) + slash;
            node_t *node = calloc( 1, VSIZE( node, name ) + len + 1 );
            span_copy( node->name, dir );
            node->frozen = true;

            if ( slash )
                node->name[ span_len( dir ) ] = '/';

            char *file = node->name + span_len( dir ) + slash;
            span_copy( file, path );
            span_t name = span_mk( node->name, node->name + len );
            var_add( src, name );

            struct stat st;

            if ( fstatat( dirfd, file, &st, 0 ) == -1 )
                sys_error( "%s:%d: stat failed for %s", r.pos.file, r.pos.line, node->name );

            if ( !graph_put( nodes, node, len ) )
            {
                node_t *prev = graph_get( nodes, name );
                if ( prev->frozen )
                    error( "%s:%d: duplicate node '%s'", r.pos.file, r.pos.line, node->name );
                prev->frozen = true;
                free( node );
                node = prev;
            }

            graph_use_stat( node, &st );
            node->type = src_node;
        }
    }

    if ( close( dirfd ) )  sys_error( "closing fd %d", dirfd );
}
