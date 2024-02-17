#include <unistd.h>
#include <string.h>

int main( int argc, const char *argv[] )
{
    const char *str = "unchanged\n";
    write( 3, str, strlen( str ) );
    execv( argv[ 1 ], argv + 1 );
}
