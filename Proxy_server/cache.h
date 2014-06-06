/* 
 * cache.h
 * Team Members:
 * Tejal Kudav - tkudav@andrew.cmu.edu
 * Ramya Krishnan - ramyakrv@andrew.cmu.edu
 *
 * The cache.h function defines a structure for cache
 * which consist of a 2 pointers for locating the previous 
 * and the next uri along with the time variable,size ,uri 
 * and data. This header also defines the various cache 
 * functions that will be implemented in cache.c
 *
 */

struct cacheline{
    struct cacheline* next;
    struct cacheline* prev;
    time_t inserted_time;
    int size;
    char* uri;
    char* data;
 };
typedef struct cacheline cacheLine;

cacheLine* findLRU();
void freeLine(cacheLine * lru);
void evictFromCache(int size);
void writeToCache(char * uri, char * data, int size);
cacheLine* readFromCache(char * uri);
cacheLine* inCache(char * uri);
