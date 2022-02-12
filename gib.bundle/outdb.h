#pragma once
#include "reader.h"
#include "writer.h"
#include "graph.h"

void save_dynamic( cb_tree *nodes, const char *path )
{
    writer_t w;
    writer_open( &w, path );

    for ( cb_iterator i = cb_begin( nodes ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *n = cb_get( &i );

        if ( !n->deps_dyn.root )
            continue;

        writer_print( &w, "out %s\n", n->name );

        for ( cb_iterator j = cb_begin( &n->deps_dyn ); !cb_end( &j ); cb_next( &j ) )
        {
            node_t *dep = cb_get( &j );
            writer_print( &w, "dep %s\n", dep->name );
        }

        writer_print( &w, "\n" );
    }

    writer_close( &w );
}

void load_dynamic( cb_tree *nodes, const char *path )
{
    reader_t r;
    node_t *n = NULL;

    if ( !reader_init( &r, AT_FDCWD, path ) )
    {
        if ( errno == ENOENT )
            return;
        else
            sys_error( "reading %s", path );
    }

    while ( read_line( &r ) )
    {
        if ( span_empty( r.span ) )
            n = NULL;

        span_t line = r.span;
        span_t word = fetch_word( &r.span );

        if ( span_eq( word, "out" ) )
            n = graph_get( nodes, r.span ) ?: graph_add( nodes, r.span );

        if ( span_eq( word, "dep" ) && n )
        {
            graph_add_dep( nodes, n, r.span, true );
            graph_add_dep( nodes, n, r.span, false );
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

        writer_print( &w, "%08llx %08llx %x %016llx ",
                          n->stamp_updated, n->stamp_changed, n->dirty, n->cmd_hash );
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
        span_t updated = fetch_word( &path ),
               changed = fetch_word( &path ),
               dirty   = fetch_word( &path ),
               cmdhash = fetch_word( &path );
        int64_t num_dirty;

        node_t *node = graph_get( nodes, path );
        if ( !node )
            node = graph_add( nodes, path );

        if ( !fetch_int( &updated, 16, &node->stamp_updated ) ||
             !fetch_int( &changed, 16, &node->stamp_changed ) ||
             !fetch_uint( &dirty, 16, &num_dirty ) ||
             !fetch_uint( &cmdhash, 16, &node->cmd_hash ) )

            error( "%s:%d: error reading timestamp(s)", file, r.pos.line );

        node->dirty      = num_dirty;
        node->stamp_want = node->stamp_updated;
    }
}
