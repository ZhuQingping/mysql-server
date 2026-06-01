/*******************************************************************
 * Copyright (C) Huawei Technologies, 2021
 *
 * memstats header file
 *******************************************************************/
#ifdef __cplusplus
#include <stddef.h>
extern "C" {
#endif

// this function will be overwrite by libtcmalloc.so from LD_PRELOAD

/*
 * prototype of the libtcmalloc MallocExtension_GetStats
 *
 * Parameters:
 * buffer (OUT)    - the stats result will be stored in this buffer
 * buffer_length (IN)  - length of the buffer
 */
void MallocExtension_GetStats(char *buffer, int buffer_length);

void __attribute__((weak))
CdeMallocStatsPrint(void (*write_cb)(void *, const char *), void *cbopaque,
                    const char *opts);

#ifdef __cplusplus
} /* extern "C" */
#endif
