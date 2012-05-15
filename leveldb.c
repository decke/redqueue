/*
 * Copyright (C) 2011 Bernhard Froehlich <decke@bluelife.at>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Author's name may not be used endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* LevelDB */
#include <leveldb/c.h>

#include "log.h"
#include "util.h"

#define CheckNoError(err) \
    if ((err) != NULL) { \
        logerror("%s:%d: %s\n", __FILE__, __LINE__, (err)); \
        abort(); \
    }


leveldb_t* db;
leveldb_cache_t* cache;
leveldb_env_t* env;
leveldb_options_t* options;


int leveldb_init(void)
{
    char *error = NULL;

    /* Initialize LevelDB */
    env = leveldb_create_default_env();
    cache = leveldb_cache_create_lru(100000);
        
    options = leveldb_options_create();
    leveldb_options_set_cache(options, cache);
    leveldb_options_set_env(options, env);
    leveldb_options_set_create_if_missing(options, 1);
    leveldb_options_set_error_if_exists(options, 0);

    db = leveldb_open(options, configget("dbFile"), &error);
    if(error != NULL){
       logerror("LevelDB Error: %s", error);
       return 1;
    }

    return 0;
}


int leveldb_free(void)
{
    leveldb_close(db);
    leveldb_options_destroy(options);
    leveldb_cache_destroy(cache);
    leveldb_env_destroy(env);
}

