#pragma once
#include "reader.h"
#include <sys/stat.h>

void load_manifest( cb_tree *nodes, var_t *src, var_t *dirs,
                    int rootfd, int filedirfd, const char *manifest )
{
    reader_t r;

    if ( !reader_init( &r, filedirfd, manifest ) )
        sys_error( NULL, "opening %s", manifest );

    int dirfd = dup( rootfd );
    span_t dir = span_dup( span_lit( "" ) );

    while ( read_line( &r ) )
    {
        if ( span_empty( r.span ) ) /* skip empty lines */
            continue;

        span_t path = r.span;
        span_t op = fetch_word( &path );

        if ( span_empty( op ) || *op.str != 'd' && *op.str != 'f' )
            error( NULL, "%s:%d: malformed line", r.pos.file, r.pos.line );

        bool is_dir = *op.str == 'd';

        if ( is_dir )
        {
            if ( close( dirfd ) )
                sys_error( NULL, "closing fd %d", dirfd );
            span_free( dir );
            dir = span_dup( path );
            if ( ( dirfd = open( dir.str, O_DIRECTORY | O_RDONLY ) ) == -1 )
                sys_error( NULL, "%s:%d: opening %s", r.pos.file, r.pos.line, dir );
            var_add( NULL, dirs, dir );
        }

        int slash = !is_dir && span_len( dir ) ? 1 : 0;
        int len = ( is_dir ? 0 : span_len( dir ) ) + span_len( path ) + slash;
        char *file = NULL;

        node_t *node = calloc( 1, offsetof( node_t, name ) + len + 1 );
        span_copy( node->name, dir );
        node->frozen = true;
        span_t name = span_mk( node->name, node->name + len );

        if ( slash )
            node->name[ span_len( dir ) ] = '/';

        if ( !is_dir )
        {
            file = node->name + span_len( dir ) + slash;
            span_copy( file, path );
            var_add( NULL, src, name );
        }

        struct stat st;

        if ( fstatat( dirfd, is_dir ? "." : file, &st, 0 ) == -1 )
            sys_error( NULL, "%s:%d: stat failed for %s", r.pos.file, r.pos.line, node->name );

        if ( !graph_put( nodes, node, len ) )
        {
            node_t *prev = graph_get( nodes, name );
            if ( prev->frozen )
                error( NULL, "%s:%d: duplicate node '%s'", r.pos.file, r.pos.line, node->name );
            prev->frozen = true;
            free( node );
            node = prev;
        }

        graph_set_stamps( node, st.st_mtime );
        node->type = src_node;
    }

    span_free( dir );

    if ( close( dirfd ) )
        sys_error( NULL, "closing fd %d", dirfd );
}
