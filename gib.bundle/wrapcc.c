#include "reader.h"
#include <sys/wait.h>

typedef struct buffer
{
    char *data;
    char *read, *write;
    char *end;
} buf_t;

void buf_addchar( buf_t *b, int c )
{
    if ( b->write == b->end )
    {
        int size = ( b->end - b->data ) * 2;
        int r_off = b->read - b->data;
        int w_off = b->write - b->data;
        b->data = realloc( b->data, size );
        b->read = b->data + r_off;
        b->write = b->data + w_off;
        b->end = b->data + size;
    }

    *b->write++ = c;
}

void buf_clear( buf_t *b )
{
    b->write = b->read = b->data;
}

int buf_getchar( buf_t *b, int fd )
{
    if ( b->read == b->write )
    {
        buf_clear( b );
        int bytes = read( fd, b->write, b->end - b->write );

        if ( bytes < 0 )
            sys_error( "read" );

        if ( bytes == 0 )
            return EOF;

        b->write += bytes;
    }

    return *b->read++;
}

void buf_init( buf_t *b )
{
    b->data = malloc( 64 );
    b->end = b->data + 64;
    buf_clear( b );
}

void buf_free( buf_t *b )
{
    free( b->data );
    b->data = NULL;
    buf_clear( b );
}

/* adapted from make/lowparse.c, (c) 1999, 2000 Mark Espie, BSD2 */

bool read_logical_line( int fd, buf_t *readbuf, buf_t *linebuf )
{
    int c = buf_getchar( readbuf, fd );

    while ( true )
    {
        if ( c == '\n' )
            break;

        if ( c == EOF )
            break;

        buf_addchar( linebuf, c );
        c = buf_getchar( readbuf, fd );

        while ( c == '\\' )
        {
            c = buf_getchar( readbuf, fd );

            if ( c == '\n' )
            {
                buf_addchar( linebuf, ' ' );
                do
                    c = buf_getchar( readbuf, fd );
                while ( c == ' ' || c == '\t' );
            }
            else
            {
                buf_addchar( linebuf, '\\' );

                if ( c == '\\' )
                {
                    buf_addchar( linebuf, '\\' );
                    c = buf_getchar( readbuf, fd );
                }

                break;
            }
        }
    }

    *linebuf->write = 0;
    return c != EOF;
}

void process_depfile( const char *path )
{
    int fd = open( path, O_RDONLY );

    if ( fd < 0 )
        sys_error( "open %s\n", path );

    unlink( path );

    buf_t readbuf, linebuf, outbuf;
    buf_init( &readbuf );
    buf_init( &linebuf );
    buf_init( &outbuf );
    bool found = false;

    while ( read_logical_line( fd, &readbuf, &linebuf ) )
    {
        span_t line = span_mk( linebuf.read, linebuf.write );
        span_t target = fetch_word( &line );

        if ( !span_eq( target, "out:" ) )
        {
            buf_clear( &linebuf );
            continue;
        }

        found = true;

        while ( !span_empty( line ) )
        {
            span_t word = fetch_word_escaped( &line );

            strcpy( outbuf.write, "dep " );
            outbuf.write += 4;

            for ( const char *c = word.str; c != word.end; ++c )
            {
                if ( c[ 0 ] == '$' && c[ 1 ] == '$' )
                    ++ c;
                buf_addchar( &outbuf, *c );
            }

            buf_addchar( &outbuf, '\n' );
            span_t parsed = span_mk( outbuf.read, outbuf.write );

            while ( !span_empty( parsed ) )
            {
                int bytes = write( 3, parsed.str, span_len( parsed ) );

                if ( bytes < 0 )
                    sys_error( "write 3" );

                parsed.str += bytes;
            }

            buf_clear( &outbuf );
        }
    }

    if ( !found )
        error( "did not find the dependency line" );
}

int main( int argc, char **argv )
{
    char *depfile;

    if ( argc <= 1 )
        error( "need at least 1 argument" );

    if ( asprintf( &depfile, "wrapcc.%d.d", getpid() ) < 0 )
        sys_error( "asprintf" );

    off_t stderr_pos = lseek( 2, 0, SEEK_CUR );
    pid_t pid = fork();

    if ( pid == 0 )
    {
        char *argv_n[ argc + 6 ];

        for ( int i = 1; i < argc; ++ i )
            argv_n[ i - 1 ] = argv[ i ];

        argv_n[ argc - 1 ] = "-MD";
        argv_n[ argc + 0 ] = "-MT";
        argv_n[ argc + 1 ] = "out";
        argv_n[ argc + 2 ] = "-MF";
        argv_n[ argc + 3 ] = depfile;
        argv_n[ argc + 4 ] = 0;
        execv( argv_n[ 0 ], argv_n );
        sys_error( "execv %s", argv_n[ 0 ] );
        return 127;
    }

    if ( pid < 0 )
        sys_error( "fork" );

    int status;
    waitpid( pid, &status, 0 );

    if ( WIFEXITED( status ) )
    {
        if ( WEXITSTATUS( status ) == 0 )
        {
            if ( stderr_pos >= 0 && stderr_pos != lseek( 2, 0, SEEK_CUR ) )
                write( 3, "warning\n", 8 );
            process_depfile( depfile );
        }
        else
            unlink( depfile );

        return WEXITSTATUS( status );
    }

    if ( WIFSIGNALED( status ) )
    {
        fprintf( stderr, "%s terminated by signal %d\n", argv[ 1 ], WTERMSIG( status ) );
        return 128 + WTERMSIG( status );
    }

    return 1;
}
