#pragma once
#include "common.h"
#include "env.h"
#include "critbit.h"

typedef enum node_type { src_node, out_node, sys_node, meta_node } node_type;

typedef struct node
{
    node_type type:3;
    bool visited:1;
    int64_t stamp;
    cb_tree deps;

    int64_t new_stamp;
    int waiting;
    bool changed;
    value_t *cmd;
    char name[];
} node_t;

node_t *graph_get( cb_tree *t, span_t name )
{
    node_t *n = cb_find( t, name.str, span_len( name ) ).leaf;

    if ( n && span_eq( name, n->name ) )
        return n;
    else
        return 0;
}

node_t *graph_add( cb_tree *t, span_t name )
{
    node_t *node = calloc( 1, VSIZE( node, name ) + span_len( name ) + 1 );
    span_copy( node->name, name );
    cb_init( &node->deps );

    if ( !cb_insert( t, node, VSIZE( node, name ), span_len( name ) ) )
    {
        free( node );
        return 0;
    }

    return node;
}
}
