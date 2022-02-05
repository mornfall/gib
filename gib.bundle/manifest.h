#pragma once
#include "reader.h"
#include <sys/stat.h>

void load_manifest( cb_tree *nodes, var_t *src, const char *file )
{
    reader_t r;

    if ( !reader_init( &r, file ) )
        sys_error( "opening %s", file );

    int rootfd = open( ".", O_DIRECTORY | O_RDONLY );
    int dirfd = dup( rootfd );
    span_t dir = span_dup( span_lit( "" ) );

    while ( fetch_line( &r ) )
    {
        if ( span_empty( r.span ) ) /* skip empty lines */
            continue;

        span_t path = r.span;
        span_t op = fetch_word( &path );

        if ( span_empty( op ) || *op.str != 'd' && *op.str != 'f' )
            error( "%s:%d: malformed line", r.pos.file, r.pos.line );

        if ( *op.str == 'd' )
        {
            close( dirfd );
            span_free( dir );
            dir = span_dup( path );
            if ( ( dirfd = open( dir.str, O_DIRECTORY | O_RDONLY ) ) == -1 )
                sys_error( "%s:%d: opening %s", r.pos.file, r.pos.line, dir );
        }
        else
        {
            int slash = span_len( dir ) ? 1 : 0;
            int len = span_len( dir ) + span_len( path ) + slash;
            node_t *node = calloc( 1, VSIZE( node, name ) + len + 1 );
            span_copy( node->name, dir );

            if ( slash )
                node->name[ span_len( dir ) ] = '/';

            char *file = node->name + span_len( dir ) + slash;
            span_copy( file, path );
            var_add( src, span_mk( node->name, node->name + len ) );

            if ( !graph_put( nodes, node, len ) )
                error( "%s:%d: duplicate node '%s'", node->name );

            struct stat st;

            if ( fstatat( dirfd, file, &st, 0 ) == -1 )
                sys_error( "%s:%d: stat failed for %s", r.pos.file, r.pos.line, node->name );

            node->stamp = st.st_mtime;
            node->type = src_node;
        }
    }
}
