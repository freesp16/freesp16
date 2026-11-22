/* Desktop soak diagnostic (tools/soak/soak-run.py run --heap-stats): an LD_PRELOAD shim whose thread
 * appends glibc's main-arena statistics every 5 s to $SOAK_HEAPSTAT_OUT as "pid,unix_s,in_use,mmapped,arena,free"
 * (bytes, mallinfo2: glibc >= 2.33, the desktop only — never built for or shipped to the unit). It tells a leak (in_use
 * grows) from allocator fragmentation (arena and free grow, in_use flat) when RSS rises. Built by soak-run.py with cc. */
#define _GNU_SOURCE
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void* run(void* arg)
{
    (void) arg;
    const char* path = getenv("SOAK_HEAPSTAT_OUT");
    const int trim = getenv("SOAK_HEAPSTAT_TRIM") != NULL;
    for (long n = 1;; ++n)
    {
        sleep(5);
        if (trim && n % 12 == 0)
            malloc_trim(0);
        struct mallinfo2 m = mallinfo2();
        FILE* f = fopen(path, "a");
        if (f)
        {
            fprintf(f, "%d,%ld,%zu,%zu,%zu,%zu\n", (int) getpid(), (long) time(0), m.uordblks, m.hblkhd, m.arena,
                    m.fordblks);
            fclose(f);
        }
    }
    return 0;
}

__attribute__((constructor)) static void start(void)
{
    if (getenv("SOAK_HEAPSTAT_OUT") == NULL)
        return;
    pthread_t t;
    if (pthread_create(&t, 0, run, 0) == 0)
        pthread_detach(t);
}
