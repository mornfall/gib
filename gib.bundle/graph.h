#pragma once
#include "common.h"
#include "env.h"
#include "critbit.h"

typedef enum node_type { src_node, out_node, sys_node, meta_node } node_type;

typedef struct node
{
    node_type type:3;
    bool visited:1;
    bool failed:1;
    bool changed:1;
    bool dirty:1;

    int64_t stamp;
    cb_tree deps;
    cb_tree blocking;

    int64_t new_stamp;
    int waiting;
    value_t *cmd;
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

void graph_stat( node_t *n )
{
    struct stat st;
    if ( stat( n->name, &st ) != -1 )
        n->stamp = n->new_stamp = st.st_mtime;
}

node_t *graph_put( cb_tree *t, node_t *node, int len )
{
    cb_init( &node->deps );
    cb_init( &node->blocking );

    if ( cb_insert( t, node, VSIZE( node, name ), len ) )
        return node;
    else
        return 0;
}

node_t *graph_add( cb_tree *t, span_t name )
{
    node_t *node = calloc( 1, VSIZE( node, name ) + span_len( name ) + 1 );
    span_copy( node->name, name );
    return graph_put( t, node, span_len( name ) ) ?: ( free( node ), ( node_t * ) 0 );
}

void graph_add_dep( cb_tree *t, node_t *n, span_t name )
{
    node_t *dep = graph_get( t, name );

    if ( !dep && name.str[ 0 ] == '/' )
    {
        dep = graph_add( t, name );
        dep->type = sys_node;
        graph_stat( dep );
    }

    cb_insert( &n->deps, dep, VSIZE( dep, name ), -1 );
}

void graph_dump( FILE *out, cb_tree *t )
{
    for ( cb_iterator i = cb_begin( t ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *n = cb_get( &i );
        fprintf( out, "node: %s\n", n->name );

        for ( cb_iterator j = cb_begin( &n->deps ); !cb_end( &j ); cb_next( &j ) )
        {
            node_t *d = cb_get( &j );
            fprintf( out, "dep: %s\n", d->name );
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
