#pragma once
#include "common.h"
#include "reader.h"

void location_init( struct location *s )
{
    s->stack = NULL;
    cb_init( &s->names );
}

void location_print_trace( struct location_stack *s )
{
    fileline_t *pos = s->reader ? &s->reader->pos : &s->pos;

    if ( s->what )
        fprintf( stderr, "%s:%d: %s\n", pos->file, pos->line, s->what );
    else
        fprintf( stderr, "%s:%d: ", pos->file, pos->line );
}

void location_vprint( struct location *s, const char *reason, va_list ap )
{
    if ( !s )
    {
        vfprintf( stderr, reason, ap );
        fprintf( stderr, "\n" );
    }

    struct location_stack *bt = s->stack;
    location_print_trace( bt );
    vfprintf( stderr, reason, ap );
    fprintf( stderr, "\n" );

    for ( bt = bt->next; bt; bt = bt->next )
        if ( bt->what )
            location_print_trace( bt );
}

void location_pop( location_t *s )
{
    assert( s->stack );
    struct location_stack *del = s->stack;
    s->stack = s->stack->next;
    free( del );
}

void location_push_fixed( struct location *s, fileline_t pos, const char *what )
{
    struct location_stack *new = malloc( sizeof( struct location_stack ) );
    new->pos  = pos;
    new->what = what;
    new->next = s->stack;
    new->reader = NULL;
    s->stack = new;
}

void location_push_reader( struct location *s, struct reader *reader )
{
    struct location_stack *new = calloc( 1, sizeof( struct location_stack ) );
    new->reader = reader;
    new->next = s->stack;
    s->stack = new;
}

void location_push_current( struct location *s, const char *what )
{
    struct location_stack *top = s->stack;
    while ( !top->reader )
        top = top->next;
    if ( top )
        location_push_fixed( s, top->reader->pos, what );
}

fileline_t location_push_named( struct location *s, span_t name, const char *what )
{
    assert( cb_contains( &s->names, name ) );
    fileline_t *pos = cb_find( &s->names, name ).leaf;
    location_push_fixed( s, *pos, what );
    return *pos;
}

void location_set( struct location *s, span_t name )
{
    fileline_t *pos = malloc( offsetof( fileline_t, name ) + span_len( name ) + 1 );

    struct location_stack *top = s->stack;
    while ( !top->reader )
        top = top->next;

    assert( top );
    *pos = top->reader->pos;
    span_copy( pos->name, name );
    cb_insert( &s->names, pos, offsetof( fileline_t, name ), span_len( name ) );
}
