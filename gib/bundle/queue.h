#pragma once
#include "outdb.h"
#include "rules.h"
#include "graph.h"
#include "job.h"
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/file.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#define MAX_FD 64

static volatile sig_atomic_t _signalled = 0, _restat = 0;

void sighandler( int sig )
{
    if ( sig == SIGUSR1 )
        _restat = 1;
    else
        _signalled = sig;
}

typedef struct
{
    const char *srcdir;

    int outdir_fd,
        logdir_fd,
        faildir_fd;
    time_t started;
    time_t stamp_rules;

    cb_tree *nodes;
    cb_tree jobs;

    job_t *job_next, *job_last;
    job_t *job_failed;
    job_t *running[ MAX_FD ];

    bool pause_output;
    int skipped_count;
    int failed_count;
    int ok_count;
    int waiting_count;
    int queued_count;
    int running_count;
    int running_max;
} queue_t;

void queue_set_failed( queue_t *q, node_t *n, bool failed )
{
    if ( n->failed && !failed )
        q->failed_count --;
    if ( !n->failed && failed )
        q->failed_count ++;

    n->failed = failed;
}

void queue_set_outdir( queue_t *q, cb_tree *env )
{
    var_t *var = env_get( env, span_lit( "outdir" ) );

    if ( !var )
    {
        var = env_set( NULL, env, span_lit( "outdir" ) );
        var_add( NULL, var, span_lit( "_build" ) );
    }

    var->frozen = true;

    if ( !var->list || var->list->next )
        error( NULL, "variable 'outdir' must be a singleton" );

    const char *dir = var->list->data;

    if ( q->outdir_fd >= 0 )
        return;

    mkdir( dir, 0777 ); /* ignore errors */
    q->outdir_fd = open( dir, O_DIRECTORY | O_CLOEXEC );

    mkdirat( q->outdir_fd, "_log", 0777 );
    mkdirat( q->outdir_fd, "_failed", 0777 );
    q->logdir_fd  = openat( q->outdir_fd, "_log",    O_DIRECTORY | O_CLOEXEC );
    q->faildir_fd = openat( q->outdir_fd, "_failed", O_DIRECTORY | O_CLOEXEC );

    if ( q->outdir_fd < 0 )
        sys_error( NULL, "opening the output directory '%s'", dir );

    if ( flock( q->outdir_fd, LOCK_EX | LOCK_NB ) == -1 )
    {
        fprintf( stderr, "output directory '%s' locked (waiting)\n", dir );

        if ( flock( q->outdir_fd, LOCK_EX ) == -1 )
            sys_error( NULL, "locking the output directory '%s'", dir );
    }

    load_dynamic( q->nodes, q->outdir_fd, "gib.dynamic" );
    load_stamps( q->nodes, q->outdir_fd, "gib.stamps" );

    DIR *fdir = fdopendir( dup( q->faildir_fd ) );
    struct dirent *fent;

    while ( fdir && ( fent = readdir( fdir ) ) )
        if ( fent->d_name[ 0 ] != '.' )
            unlinkat( q->faildir_fd, fent->d_name, 0 );
    closedir( fdir );
}

void queue_show_result( queue_t *q, node_t *n, job_t *j, int verbosity )
{
    const char *status = "??";
    int color = 0;
    bool changed = n->stamp_changed == n->stamp_want;
    bool updated = n->stamp_updated == n->stamp_want;

    if      ( n->failed )      status = "no", color = 31;
    else if ( !changed )       status = "--", color = 33, q->skipped_count ++;
    else if ( j && j->warned ) status = "ok", color = 33, q->ok_count ++;
    else                       status = "ok", color = 32, q->ok_count ++;

    if ( _signalled )
        return;

    if ( ( verbosity >= 2 || !q->pause_output ) && ( n->failed || changed && j && j->warned ) )
    {
        fprintf( stderr, "\033[J" );
        char filename[ strlen( n->name ) + 5 ], *p = filename;
        for ( char *i = n->name; *i; ++p, ++i )
            *p = ( *i == ' ' || *i == '/' ) ? '_' : *i;
        strcpy( p, ".txt" );

        if ( n->failed )
            linkat( q->logdir_fd, filename, q->faildir_fd, filename, 0 );

        reader_t log;

        if ( !reader_init( &log, q->logdir_fd, filename ) )
            sys_error( NULL, "opening logfile %s", filename );

        while ( read_line( &log ) )
            fprintf( stderr, " │ %.*s\n", span_len( log.span ), log.span.str );

        close( log.fd );
    }

    if ( verbosity >= 1 || !q->pause_output )
        fprintf( stderr, "\033[J\033[%dm%s\033[0m %s\n", color, status, n->name );
}

void queue_add( queue_t *q, job_t *j )
{
    assert( !j->queued );
    assert( j->node->dirty );

    j->changed = true;
    j->queued = true;
    j->next = NULL;
    ++ q->queued_count;

    if ( !q->job_next )
        q->job_next = j;

    if ( q->job_last )
        q->job_last->next = j;

    q->job_last = j;
}

bool queue_start_next( queue_t *q )
{
    if ( !q->job_next )
        return false;

    job_t *j = q->job_next;
    q->job_next = j->next;

    if ( !q->job_next )
        q->job_last = NULL;

    cb_clear( &j->node->deps_dyn, false );
    job_fork( j, q->outdir_fd, q->logdir_fd );

    q->running_count ++;
    q->queued_count --;

    assert( j->pipe_fd < MAX_FD );
    q->running[ j->pipe_fd ] = j;

    return true;
}

void queue_cleanup_node( queue_t *q, node_t *n )
{
    if ( n->type != out_node && n->type != meta_node || n->dirty && !n->failed || n->waiting )
        return;

    for ( cb_iterator i = cb_begin( &n->blocking ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *b = cb_get( &i );
        assert( b->waiting > 0 );
        assert( b != n );

        if ( n->stamp_changed > b->stamp_updated )
            b->dirty = true;

        if ( n->failed )
            queue_set_failed( q, b, true );

        if ( ! -- b->waiting )
        {
            q->waiting_count --;

            if ( b->dirty && b->stamp_updated < b->stamp_want && !b->failed )
                queue_add( q, job_add( &q->jobs, b ) );
            else
                queue_cleanup_node( q, b );
        }
    }

    cb_clear( &n->blocking, false );
}

void queue_cleanup_job( queue_t *q, int fd )
{
    job_t *j = q->running[ fd ];
    q->running[ fd ] = 0;
    node_t *n = j->node;

    int status;

    if ( waitpid( j->pid, &status, 0 ) == -1 )
        sys_error( NULL, "waitpid %d", j->pid );

    if ( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 )
    {
        n->stamp_updated = n->stamp_want;
        n->cmd_hash = var_hash( n->cmd );
        n->dirty = false;

        if ( j->changed )
            n->stamp_changed = n->stamp_want;
    }
    else if ( !_signalled )
        queue_set_failed( q, n, true );

    queue_show_result( q, n, j, 0 );

    if ( n->failed )
    {
        q->pause_output = true;
        j->next = q->job_failed;
        q->job_failed = j;
    }

    j->queued = false;
    q->running_count --;
    queue_cleanup_node( q, j->node );
}

void queue_teardown( queue_t *q )
{
    fprintf( stderr, "\033[J\r[caught signal %d, cleaning up]\n", _signalled );
    fflush( stderr );

    job_t *j = NULL;

    for ( int fd = 0; fd < MAX_FD; ++ fd )
        if ( ( j = q->running[ fd ] ) )
            kill( j->pid, SIGTERM );

    for ( int fd = 0; fd < MAX_FD; ++ fd )
        if ( q->running[ fd ] )
            queue_cleanup_job( q, fd );
}

bool queue_loop( queue_t *q )
{
    fd_set ready, except;
    FD_ZERO( &ready );
    FD_ZERO( &except );

    if ( q->running_count )
    {
        for ( int i = 0; i < MAX_FD; ++i )
            if ( q->running[ i ] )
                FD_SET( i, &ready ), FD_SET( i, &except );

        struct timeval tv = { 1, 0 };

        switch ( select( MAX_FD, &ready, 0, &except, &tv ) )
        {
            case 0:
                return true;
            case -1:
                if ( errno != EINTR )
                    sys_error( NULL, "select" );
            default:
                ;
        }
    }

    if ( _signalled )
        queue_teardown( q );

    if ( q->running_count )
        for ( int fd = 0; fd < MAX_FD; ++ fd )
            if ( FD_ISSET( fd, &ready ) || FD_ISSET( fd, &except ) )
                if ( job_update( q->running[ fd ], q->nodes, q->srcdir ) )
                    queue_cleanup_job( q, fd );

    if ( !_signalled )
        while ( q->running_count < q->running_max )
            if ( !queue_start_next( q ) )
                break;

    return !_signalled && ( q->running_count || q->job_next );
}

void queue_update_blocking( queue_t *q, node_t *goal, node_t *out, node_t *dep )
{
    node_t *dep_out = dep->type == out_node ? dep : 0;

    if ( dep_out && !dep_out->failed )
        if ( dep_out->stamp_want > dep_out->stamp_updated || dep_out->dirty )
            if ( cb_insert( &dep->blocking, goal, offsetof( node_t, name ), -1 ) )
                if ( !goal->waiting ++ )
                    ++ q->waiting_count;

    if ( dep->stamp_changed > out->stamp_updated )
        out->dirty = true;
}

void queue_create_jobs( queue_t *q, node_t *goal, node_t *requested_by )
{
    if ( goal->visited )
        goto end;

    goal->visited = true;
    node_t *out = goal->type == out_node ? goal : 0;

    for ( cb_iterator i = cb_begin( &goal->deps ); !cb_end( &i ); cb_next( &i ) )
        queue_create_jobs( q, cb_get( &i ), out );
    for ( cb_iterator i = cb_begin( &goal->deps_dyn ); !cb_end( &i ); cb_next( &i ) )
        queue_create_jobs( q, cb_get( &i ), out );

    if ( !out )
        goto end;

    if ( var_hash( out->cmd ) != out->cmd_hash )
    {
        out->dirty = true;

        if ( q->stamp_rules > out->stamp_want )
            out->stamp_want = q->stamp_rules;
    }

    if ( out->stamp_want > out->stamp_updated || out->dirty )
    {
        for ( cb_iterator i = cb_begin( &goal->deps ); !cb_end( &i ); cb_next( &i ) )
            queue_update_blocking( q, goal, out, cb_get( &i ) );
        for ( cb_iterator i = cb_begin( &goal->deps_dyn ); !cb_end( &i ); cb_next( &i ) )
            queue_update_blocking( q, goal, out, cb_get( &i ) );
    }

    if ( !out->waiting && out->dirty ) /* can run right away */
        queue_add( q, job_add( &q->jobs, out ) );

end:
    if ( requested_by )
    {
        if ( requested_by->stamp_want < goal->stamp_want )
            requested_by->stamp_want = goal->stamp_want;
        if ( goal->failed )
            queue_set_failed( q, requested_by, true );
    }
}

void queue_add_goal( queue_t *q, const char *name )
{
    node_t *goal = graph_get( q->nodes, span_lit( name ) );

    if ( goal )
        queue_create_jobs( q, goal, NULL );
    else
        error( NULL, "goal %s not defined", name );
}

void queue_goals( queue_t *q, cb_tree *goals, cb_tree *nodes )
{
    for ( cb_iterator i = cb_begin( goals ); !cb_end( &i ); cb_next( &i ) )
        queue_create_jobs( q, cb_get( &i ), NULL );

    for ( cb_iterator i = cb_begin( nodes ); !cb_end( &i ); cb_next( &i ) )
        queue_cleanup_node( q, cb_get( &i ) );
}

bool queue_restat( queue_t *q, cb_tree *nodes )
{
    bool changed = false;

    for ( cb_iterator i = cb_begin( nodes ); !cb_end( &i ); cb_next( &i ) )
    {
        node_t *n = cb_get( &i );

        if ( !n->visited )
        {
            n->visited = true;
            n->changed = false;
            assert( n->waiting == 0 );

            if ( n->type == src_node )
                if ( !graph_do_stat( n ) )
                    sys_error( NULL, "stat failed on %s", n->name );

            if ( n->type == out_node )
                if ( queue_restat( q, &n->deps ) | queue_restat( q, &n->deps_dyn ) )
                {
                    n->changed = true;
                    queue_set_failed( q, n, false );
                }
        }

        changed = changed || n->changed;
    }

    return changed;
}

void queue_init( queue_t *q, cb_tree *nodes, const char *srcdir )
{
    q->outdir_fd = -1;
    q->srcdir = srcdir;
    q->nodes = nodes;
    q->stamp_rules = 0;

    cb_init( &q->jobs );

    q->started = time( NULL );
    q->failed_count = 0;
    q->skipped_count = 0;
    q->ok_count = 0;
    q->running_count = 0;
    q->queued_count = 0;
    q->waiting_count = 0;
    q->running_max = 4;
    q->pause_output = false;

    q->job_next = NULL;
    q->job_last = NULL;
    q->job_failed = NULL;

    for ( int i = 0; i < MAX_FD; ++i )
        q->running[ i ] = 0;
}

void queue_monitor( queue_t *q, bool endmsg )
{
    time_t elapsed = 0;
    int fail_count = 0;

    signal( SIGHUP, sighandler );
    signal( SIGINT, sighandler );
    signal( SIGTERM, sighandler );
    signal( SIGUSR1, sighandler );

    signal( SIGPIPE, sighandler ); // ??
    signal( SIGALRM, sighandler ); // ??

    while ( queue_loop( q ) )
    {
        elapsed = time( NULL ) - q->started;
        fprintf( stderr, "%d/%d running + %d/%d queued | %d ok + %d failed | %lld:%02lld elapsed\r",
                 q->running_count, q->running_max, q->queued_count, q->queued_count + q->waiting_count,
                 q->ok_count, q->failed_count, elapsed / 60, elapsed % 60 );
    }

    elapsed = time( NULL ) - q->started;

    if ( endmsg )
    {
        for ( job_t *j = q->job_failed; j; j = j->next )
            if ( fail_count || j->next || !q->pause_output )
                queue_show_result( q, j->node, j, ++fail_count > 10 && !j->next ? 2 : 1 );

        fprintf( stderr, "\033[J\rbuild finished: %d ok, %d failed, %d skipped, %lld:%02lld elapsed\n",
                 q->ok_count, q->failed_count, q->skipped_count, elapsed / 60, elapsed % 60 );

        q->pause_output = false;
    }
}
