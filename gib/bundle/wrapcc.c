#include "reader.h"
#include "writer.h"
#include "error.h"
#include <sys/wait.h>

bool process_line( span_t line, writer_t *out )
{
    while ( !span_empty( line ) && *line.str == ' ' )
        ++ line.str;

    while ( !span_empty( line ) )
    {
        span_t word = fetch_word_escaped( &line );

        if ( span_empty( line ) && span_eq( word, "\\" ) )
            return true; /* continue */

        assert( !span_empty( word ) );
        writer_append( out, span_lit( "dep " ) );

        for ( const char *c = word.str; c != word.end; ++c )
        {
            if ( c[ 0 ] == '$' && c[ 1 ] == '$' )
                ++ c;
            writer_append( out, span_mk( c, c + 1 ) );
        }

        writer_append( out, span_lit( "\n" ) );
    }

    return false;
}

void process_depfile( const char *path )
{
    reader_t reader;
    writer_t writer;
    bool found = false;

    if ( !reader_init( &reader, AT_FDCWD, path ) )
        sys_error( NULL, "open %s", path );

    writer.file = writer.tmp = NULL;
    writer.ptr = 0;
    writer.fd = 3;

    unlink( path );

    while ( read_line( &reader ) )
    {
        if ( !found )
            found = span_eq( fetch_word( &reader.span ), "out:" );

        if ( found && !process_line( reader.span, &writer ) )
            break;
    }

    if ( !found )
        error( NULL, "did not find the dependency line" );

    writer_flush( &writer );
}

bool wrap( const char **argv, int *rv )
{
    pid_t pid = fork();

    if ( pid == 0 )
    {
        execv( argv[ 0 ], argv );
        sys_error( NULL, "execv %s", argv[ 0 ] );
        exit( 127 );
    }

    if ( pid < 0 )
        sys_error( NULL, "fork" );

    int status;
    waitpid( pid, &status, 0 );

    if ( WIFEXITED( status ) )
    {
        *rv = WEXITSTATUS( status );
        return *rv == 0;
    }

    if ( WIFSIGNALED( status ) )
    {
        fprintf( stderr, "%s terminated by signal %d\n", argv[ 0 ], WTERMSIG( status ) );
        *rv = 128 + WTERMSIG( status );
        return false;
    }
}

int main( int argc, char **argv )
{
    char *depfile;
    int rv;

    if ( argc <= 1 )
        error( NULL, "need at least 1 argument" );

    if ( asprintf( &depfile, "-MF./wrapcc.%d.d", getpid() ) < 0 )
        sys_error( NULL, "asprintf" );

    off_t stderr_pos = lseek( 2, 0, SEEK_CUR );
    char *argv_n[ argc + 3 ];

    for ( int i = 1; i < argc; ++ i )
        argv_n[ i - 1 ] = argv[ i ];

    argv_n[ argc - 1 ] = "-MD";
    argv_n[ argc + 0 ] = "-MTout";
    argv_n[ argc + 1 ] = depfile;
    argv_n[ argc + 2 ] = 0;

    if ( wrap( argv_n, &rv ) )
        if ( stderr_pos >= 0 && stderr_pos != lseek( 2, 0, SEEK_CUR ) )
            write( 3, "warning\n", 8 );

    process_depfile( depfile + 3 );
    unlink( depfile + 3 );
    return rv;
}
