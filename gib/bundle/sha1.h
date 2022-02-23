#pragma once
#include <stdint.h>

#define SHA1_BLOCK_LENGTH  64
#define SHA1_DIGEST_LENGTH 20

typedef struct
{
    uint32_t state[ 5 ];
    uint64_t count;
    uint8_t  buffer[ SHA1_BLOCK_LENGTH ];
} sha1_ctx;

void sha1_init( sha1_ctx *context );
void sha1_transform( uint32_t state[ 5 ], const uint8_t buffer[ SHA1_BLOCK_LENGTH ] );
void sha1_update( sha1_ctx *context, const void *data, unsigned int len );
void sha1_final( uint8_t digest[ SHA1_DIGEST_LENGTH ], sha1_ctx *context );
