#include "reader.h"
#include "graph.h"
#include "job.h"
#define MAX_FD 64

typedef struct
{
    char *srcdir;
    char *outdir;

    cb_tree env;
    cb_tree nodes;
    cb_tree jobs;

    job_t *job_next, *job_last;
    job_t *running[ MAX_FD ];

    int running_count;
    int running_max;
} state_t;

void job_queue( state_t *s, job_t *j )
{
    if ( !s->job_next )
        s->job_next = j;

    if ( s->job_last )
        s->job_last->next = j;

    s->job_last = j;
}

bool job_start( state_t *s )
{
    if ( !s->job_next )
        return false;

    job_t *j = s->job_next;
    s->job_next = j->next;
    job_fork( j );
    s->running_count ++;

    assert( j->pipe_fd < MAX_FD );
    s->running[ j->pipe_fd ] = j;

    return true;
}

void job_cleanup( state_t *s, int fd )
{
    job_t *j = s->running[ fd ];
    s->running_count --;
    cb_iterator i;

    for ( cb_begin( &i, &j->blocking ); !cb_end( &i ); cb_next( &i ) )
    {
        job_t *b = cb_get( &i );

        if ( --b->node->waiting )
            continue;
    }
}

void main_loop( state_t *s )
{
    while ( s->running_count < s->running_max )
        if ( !job_start( s ) )
            break;

    fd_set ready;
    FD_ZERO( &ready );

    for ( int i = 0; i < MAX_FD; ++i )
        if ( s->running[ i ] )
            FD_SET( i, &ready );

    if ( select( MAX_FD, &ready, 0, 0, 0 ) == -1 )
        die( "select" );

    for ( int fd = 0; fd < MAX_FD; ++ fd )
        if ( FD_ISSET( fd, &ready ) )
            if ( job_update( s->running[ fd ] ) )
                job_cleanup( s, fd );
}

void load_graph( state_t *s )
{
}

void create_jobs( state_t *s, node_t *goal )
{
    if ( goal->visited )
        return;

    goal->visited = true;
    node_t *out = goal->type == out_node ? goal : 0;
    cb_iterator i;

    for ( cb_begin( &i, &goal->deps ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *b = cb_get( &i );
        node_t *b_out = b->type == out_node ? b : 0;
        create_jobs( s, b );

        if ( out && out->new_stamp < b->stamp )
            out->new_stamp = b->stamp;

        if ( b_out && b_out->stamp != b_out->new_stamp )
            job_wanted( &s->jobs, b_out, goal );
    }
}

int main( int argc, const char *argv[] )
{
    state_t s;
    s.srcdir = getcwd( 0, 0 );

    cb_init( &s.env );
    cb_init( &s.nodes );
    cb_init( &s.jobs );

    load_rules( &s.nodes, &s.env, "gibfile" );
    var_t *outdir = env_get( &s.env, span_lit( "outdir" ) );

    if ( asprintf( &s.outdir, "%s/build.default",
                   outdir && outdir->list ? outdir->list->data : s.srcdir ) < 0 )
        die( "asprintf" );

    char *path_dyn, *path_stamp;

    if ( asprintf( &path_dyn, "%s/dynamic.gib", s.outdir ) < 0 )
        die( "asprintf" );

    if ( asprintf( &path_stamp, "%s/stamps.gib", s.outdir ) < 0 )
        die( "asprintf" );

    load_dynamic( &s.nodes, path_dyn );
    load_stamps( &s.nodes, path_stamp );

    s.job_next = 0;
    s.job_last = 0;

    for ( int i = 0; i < MAX_FD; ++i )
        s.running[ i ] = 0;

    var_t *jobs = env_get( &s.env, span_lit( "jobs" ) );
    node_t *all = graph_get( &s.nodes, span_lit( "all" ) );

    if ( !all )
        fprintf( stderr, "goal all does not exist\n" ), exit( 1 );

    s.running_count = 0;
    s.running_max = jobs && jobs->list ? atoi( jobs->list->data ) : 4;

    create_jobs( &s, all ); /* TODO */
    mkdir( s.outdir, 0777 ); /* ignore errors */
    main_loop( &s );

    /*
    write_stamps( &s.nodes );
    write_dynamic( &s.nodes );
    */

    return 0;
}
