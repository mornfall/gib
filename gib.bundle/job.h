#include "common.h"
#include "env.h"
#include <sys/socket.h>
#define MAX_ARGS 128

typedef struct job
{
    node_t *node;
    pid_t pid;
    bool queued;
    int pipe_fd;
    struct job *next;
    char *dyn_info;
    int dyn_size;
    char name[];
} job_t;

void job_fork( job_t *j, int dirfd )
{
    int fds[ 2 ];

    if ( socketpair( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds ) )
        sys_error( "socketpair %s", j->name );

    value_t *cmd = j->node->cmd;
    j->pipe_fd = fds[ 0 ];
    j->pid = fork();

    if ( j->pid == 0 ) /* child */
    {
        char *argv[ MAX_ARGS ];
        value_t *n = cmd;
        int i = 0;

        while ( n )
        {
            argv[ i++ ] = n->data;
            n = n->next;
        }

        argv[ i ] = 0;

        fchdir( dirfd );
        dup2( fds[ 1 ], 3 );
        mkdir( "gib.log", 0777 );

        char path[ 1024 ];
        sprintf( path, "gib.log/%s.txt", j->name );

        for ( char *c = path + 8; *c; ++c )
            if ( *c == '/' || *c == ' ' )
                *c = '_';

        unlink( path );
        int logfd = open( path, O_CREAT | O_TRUNC | O_WRONLY | O_EXCL, 0666 );
        int nullfd = open( "/dev/null", O_RDONLY );

        if ( logfd < 0 )
            sys_error( "opening logfile %s", path );

        if ( nullfd < 0 )
            sys_error( "opening /dev/null" );

        dup2( nullfd, 0 );
        dup2( logfd, 1 );
        dup2( logfd, 2 );

        execv( cmd->data, argv );
        sys_error( "execv %s (job %s):", cmd->data, j->name );
    }

    close( fds[ 1 ] );

    if ( j->pid == -1 )
        sys_error( "fork %s [%s]:", j->name, cmd->data );
}

job_t *job_find( cb_tree *jobs, node_t *node )
{
    job_t *j = cb_find( jobs, span_lit( node->name ) ).leaf;
    if ( j && !strcmp( j->name, node->name ) )
        return j;
    else
        return 0;
}

job_t *job_add( cb_tree *jobs, node_t *build )
{
    span_t name = span_lit( build->name );
    job_t *j = job_find( jobs, build );

    if ( !j )
    {
        j = calloc( 1, VSIZE( j, name ) + span_len( name ) + 1 );
        span_copy( j->name, name );
        j->node = build;
        build->changed = true;
        cb_insert( jobs, j, VSIZE( j, name ), -1 );
    }

    return j;
}

bool job_update( job_t *j )
{
    char buff[ 1024 ];
    int bytes = read( j->pipe_fd, buff, 1024 );
    if ( bytes < 0 )
        sys_error( "reading from status pipe of %s", j->name );

    if ( bytes == 0 )
    {
        close( j->pipe_fd );
        return true;
    }

    j->dyn_info = realloc( j->dyn_info, j->dyn_size + bytes );
    memcpy( j->dyn_info + j->dyn_size, buff, bytes );
    j->dyn_size += bytes;
    return false;
}
