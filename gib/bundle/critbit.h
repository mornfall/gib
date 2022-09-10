#pragma once
#include "span.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include <sys/types.h>
#include <errno.h>

static const uint16_t CB_VSIZE_INTERNAL = -1;

typedef struct
{
    void *child[ 2 ];
    uint32_t byte:24;
    uint32_t mask:8;
    uint16_t vsize[ 2 ];
} cb_node;

typedef struct
{
    cb_node *root;
    uint16_t vsize;
} cb_tree;

typedef struct
{
    void *leaf;
    uint16_t vsize;
} cb_result;

typedef struct
{
    int depth, max_depth;
    void **stack;
} cb_iterator;

bool cb_dir( uint8_t c, uint8_t mask )
{
    return ( 1 + ( mask | c ) ) >> 8;
}

bool cb_bit( cb_node *n, span_t k )
{
    uint8_t c = 0;

    if ( n->byte < span_len( k ) )
        c = k.str[ n->byte ];

    return cb_dir( c, n->mask );
}

void *cb_get( cb_iterator *i )
{
    return i->stack[ i->depth ];
}

void cb_iter_realloc( cb_iterator *i )
{
    if ( i->depth + 1 == i->max_depth )
    {
        i->max_depth *= 2;
        i->stack = realloc( i->stack, i->max_depth * sizeof( cb_node * ) );
    }
}

void cb_iter_dive( cb_iterator *i, span_t where )
{
    cb_node *n = i->stack[ i->depth ];

    while ( n )
    {
        cb_iter_realloc( i );
        bool dir = cb_bit( n, where );

        if ( n->vsize[ dir ] == CB_VSIZE_INTERNAL )
            n = n->child[ dir ];
        else
            break;

        i->stack[ ++i->depth ] = n;
    }

    cb_iter_realloc( i );
    i->stack[ ++i->depth ] = n->child[ cb_bit( n, where ) ];
}

cb_iterator cb_begin_at( cb_tree *t, span_t where )
{
    cb_iterator i;
    cb_node *n = t->root;
    i.depth = -1;
    i.stack = NULL;

    if ( !n )
        return i;

    i.depth = 0;
    i.max_depth = 8;

    i.stack = calloc( i.max_depth + 1, sizeof( cb_node * ) );
    i.stack[ 0 ] = t->root;

    if ( t->vsize == CB_VSIZE_INTERNAL )
        cb_iter_dive( &i, where );

    return i;
}

cb_iterator cb_begin( cb_tree *t )
{
    return cb_begin_at( t, span_lit( "" ) );
}

void cb_next( cb_iterator *i )
{
    int count = 0;

    while ( i->depth > 0 &&
            i->stack[ i->depth ] == ( ( cb_node * ) i->stack[ i->depth - 1 ] )->child[ 1 ] )
    {
        -- i->depth, ++ count;
    }

    if ( !i->depth )
    {
        -- i->depth;
        return;
    }

    cb_node *parent = i->stack[ i->depth - 1 ];
    i->stack[ i->depth ] = parent->child[ 1 ];

    if ( parent->vsize[ 1 ] == CB_VSIZE_INTERNAL )
        cb_iter_dive( i, span_lit( "" ) );
}

void cb_iterator_free( cb_iterator *i )
{
    free( i->stack );
    i->stack = NULL;
}

bool cb_end( cb_iterator *i )
{
    if ( i->depth == -1 )
        return cb_iterator_free( i ), true;
    else
        return false;
}

cb_result cb_find( cb_tree *t, span_t k )
{
    assert( t );
    cb_result r = { t->root, t->vsize };
    cb_node *n = t->root;

    if ( n )
        while ( r.vsize == CB_VSIZE_INTERNAL )
        {
            bool dir = cb_bit( n, k );
            r.leaf = n->child[ dir ];
            r.vsize = n->vsize[ dir ];
            n = n->child[ dir ];
        }

    return r;
}

int cb_contains( cb_tree *t, span_t k )
{
    cb_result r = cb_find( t, k );
    return r.vsize != CB_VSIZE_INTERNAL && r.leaf && span_eq( k, r.leaf + r.vsize );
}

void cb_init( cb_tree *t )
{
    t->root = 0;
    t->vsize = CB_VSIZE_INTERNAL;
}

cb_tree *cb_alloc()
{
    cb_tree *t = malloc( sizeof( cb_tree ) );
    cb_init( t );
    return t;
}

bool cb_insert_at( cb_tree *t, cb_result r, int byte, uint8_t mask, void *leaf,
                   uint16_t vsize, int len )
{
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask  = ( mask & ~( mask >> 1 ) ) ^ 0xff;

    const uint8_t *old_k = r.leaf + r.vsize;
    const uint8_t *new_k = leaf + vsize;
    cb_node *new = calloc( 1, sizeof( cb_node ) );
    new->byte = byte;
    new->mask = mask;

    int n_dir = cb_dir( old_k[ byte ], mask );
    new->child[ 1 - n_dir ] = leaf;
    new->vsize[ 1 - n_dir ] = vsize;

    cb_node **s_node = &t->root;
    uint16_t *s_vsize = &t->vsize;

    while ( *s_vsize == CB_VSIZE_INTERNAL )
    {
        cb_node *next = *s_node;

        if ( next->byte > byte ||
             next->byte == byte && next->mask > mask )
            break;

        const int dir = cb_bit( next, span_mk( new_k, new_k + len ) );
        s_node  = ( cb_node ** ) next->child + dir;
        s_vsize = next->vsize + dir;
    }

    new->child[ n_dir ] = *s_node;
    new->vsize[ n_dir ] = *s_vsize;
    *s_node = new;
    *s_vsize = CB_VSIZE_INTERNAL;
    return true;
}

bool cb_insert( cb_tree *t, void *leaf, uint16_t vsize, int len )
{
    if ( !t->root )
    {
        t->root = leaf;
        t->vsize = vsize;
        return true;
    }

    const uint8_t *new_k = leaf + vsize;
    if ( len < 0 )
        len = strlen( new_k );

    cb_result n = cb_find( t, span_mk( new_k, new_k + len ) );
    const uint8_t *old_k = n.leaf + n.vsize;
    int i = 0;

    for ( ; i < len ; ++i )
        if ( old_k[ i ] != new_k[ i ] )
            return cb_insert_at( t, n, i, old_k[ i ] ^ new_k[ i ], leaf, vsize, len );

    if ( old_k[ i ] )
        return cb_insert_at( t, n, i, old_k[ i ], leaf, vsize, len );

    return false; /* not inserted */
}

void cb_free( cb_node *n )
{
    for ( int ch = 0; ch <= 1; ++ch )
        if ( n && n->vsize[ ch ] == CB_VSIZE_INTERNAL )
        {
            cb_free( n->child[ ch ] );
            free( n->child[ ch ] );
        }
}

void cb_clear( cb_tree *t, bool release )
{
    bool free_root = t->vsize == CB_VSIZE_INTERNAL;

    if ( release )
        for ( cb_iterator i = cb_begin( t ); !cb_end( &i ); cb_next( &i ) )
            free( cb_get( &i ) );

    if ( free_root )
    {
        cb_free( t->root );
        free( t->root );
    }

    cb_init( t );
}
