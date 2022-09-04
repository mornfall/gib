#pragma once
#include "common.h"
#include "env.h"
#include "critbit.h"

typedef enum node_type { src_node, out_node, sys_node } node_type;

typedef struct node
{
    /* Stamps govern what needs to be built:
     *
     *  • updated – when was the node was last rebuilt (or updated by the
     *    user, if it's a sys or src node),
     *  • changed – when did a rebuild of the node last change it (this stamp
     *    is not updated when a job notices that its output didn't change since
     *    last rebuild)
     *  • want – the next value for updated (i.e. after we are done with this
     *    run, the node will be up to date with respect to all its dependencies
     *    as of this stamp). */

    int64_t stamp_updated;
    int64_t stamp_changed;
    int64_t stamp_want;

    cb_tree deps;
    cb_tree deps_dyn;
    cb_tree blocking;

    uint64_t cmd_hash;
    value_t *cmd;

    node_type type:3;
    bool visited:1; /* create jobs, restat */
    bool changed:1; /* restat */
    bool failed:1;
    bool dirty:1;
    bool frozen:1;
    int waiting:24;

    char name[];
} node_t;

node_t *graph_get( cb_tree *t, span_t name )
{
    node_t *n = cb_find( t, name ).leaf;

    if ( n && span_eq( name, n->name ) )
        return n;
    else
        return 0;
}

void graph_set_stamps( node_t *n, int64_t value )
{
    if ( n->stamp_updated != value )
        n->changed = true;

    n->stamp_want = n->stamp_changed = n->stamp_updated = value;
}

bool graph_do_stat( node_t *n )
{
    struct stat st;

    if ( stat( n->name, &st ) == -1 )
        return false;
    else
        return graph_set_stamps( n, st.st_mtime ), true;
}

node_t *graph_put( cb_tree *t, node_t *node, int len )
{
    cb_init( &node->deps );
    cb_init( &node->deps_dyn );
    cb_init( &node->blocking );

    if ( cb_insert( t, node, offsetof( node_t, name ), len ) )
        return node;
    else
        return 0;
}

node_t *graph_add( cb_tree *t, span_t name )
{
    node_t *node = graph_get( t, name );

    if ( !node )
    {
        node = calloc( 1, SIZE_NAMED( node_t, + span_len( name ) ) );
        span_copy( node->name, name );
        graph_put( t, node, span_len( name ) );
    }

    return node;
}

node_t *graph_find_file( cb_tree *nodes, span_t name )
{
    node_t *n = graph_add( nodes, name );

    if ( !n->frozen )
    {
        graph_do_stat( n );
        n->type   = src_node;
        n->frozen = true;
    }

    return n;
}

void graph_add_dep( cb_tree *t, node_t *n, span_t name, bool dyn )
{
    node_t *dep = graph_get( t, name );

    if ( !dep )
    {
        dep = graph_add( t, name );
        dep->type = span_len( name ) >= 1 && name.str[ 0 ] == '/' ? sys_node : src_node;

        if ( !graph_do_stat( dep ) )
            dep->type = sys_node;
    }

    if ( !dep )
        error( NULL, "dependency %.*s not defined", span_len( name ), name );

    if ( dyn )
        cb_insert( &n->deps_dyn, dep, offsetof( node_t, name ), span_len( name ) );
    else
        cb_insert( &n->deps, dep, offsetof( node_t, name ), span_len( name ) );
}

void graph_free( node_t *node )
{
    cb_clear( &node->deps, false );
    cb_clear( &node->deps_dyn, false );
    cb_clear( &node->blocking, false );

    for ( value_t *v = node->cmd, *next; v; v = next )
        next = v->next, free( v );

    node->cmd = NULL;
}

void graph_clear_visited( cb_tree *nodes )
{
    for ( cb_iterator i = cb_begin( nodes ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *n = cb_get( &i );

        if ( n->visited )
        {
            n->visited = false;
            graph_clear_visited( &n->deps );
            graph_clear_visited( &n->deps_dyn );
        }
    }
}

void graph_dump( FILE *out, cb_tree *t )
{
    for ( cb_iterator i = cb_begin( t ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *n = cb_get( &i );
        fprintf( out, "node: %s\n", n->name );
        fprintf( out, "stamps: %08llx updated | %08llx changed\n", n->stamp_updated, n->stamp_changed );

        if ( n->dirty )
            fprintf( out, "dirty\n" );

        for ( cb_iterator j = cb_begin( &n->deps ); !cb_end( &j ); cb_next( &j ) )
        {
            node_t *d = cb_get( &j );
            fprintf( out, "dep: %s\n", d->name );
        }

        for ( cb_iterator j = cb_begin( &n->deps_dyn ); !cb_end( &j ); cb_next( &j ) )
        {
            node_t *d = cb_get( &j );
            fprintf( out, "dyn: %s\n", d->name );
        }

        if ( n->cmd )
        {
            fprintf( out, "cmd: " );
            for ( value_t *v = n->cmd; v; v = v->next )
                fprintf( out, "%s\n     ", v->data );
        }

        fprintf( out, "\n" );
    }
}
