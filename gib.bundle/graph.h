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

node_t *graph_get( cb_tree *t, const char *name )
{
    node_t *n = cb_find( t, name, strlen( name ) ).leaf;

    if ( !n || strcmp( n->name, name ) )
        return 0;
    else
        return n;
}
