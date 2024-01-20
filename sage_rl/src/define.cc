#include "define.h"


double mul(double a, double b)
{
    return ((a*b)>MAX_32Bit)?MAX_32Bit:a*b;
}


uint64_t raw_timestamp( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_REALTIME, &ts );
    uint64_t us = ts.tv_nsec / 1000;
    us += (uint64_t)ts.tv_sec * 1000000;
    return us;
}


uint64_t timestamp_begin(bool set)
{
        static uint64_t start;
        if(set)
            start = raw_timestamp();
        return start;
}


uint64_t timestamp_end( void )
{
    return raw_timestamp() - timestamp_begin(0);
}


uint64_t initial_timestamp( void )
{
    static uint64_t initial_value = raw_timestamp();
    return initial_value;
}


uint64_t timestamp( void )
{
    return raw_timestamp() - initial_timestamp();
}


void handler(int sig) {
    void *array[10];
    size_t size;
    DBGMARK(DBGSERVER,2,"=============================================================== Start\n");
    size = backtrace(array, 20);
    fprintf(stderr, "We got signal %d:\n", sig);
    DBGMARK(DBGSERVER,2,"=============================================================== End\n");
    shmdt(shared_memory);
    shmctl(shmid, IPC_RMID, NULL);
    shmdt(shared_memory_rl);
    shmctl(shmid_rl, IPC_RMID, NULL);
    exit(1);
}
