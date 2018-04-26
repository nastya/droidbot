/*
 * Copyright (C) 2006 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Process dmtrace output.
 *
 * This is the wrong way to go about it -- C is a clumsy language for
 * shuffling data around.  It'll do for a first pass.
 */
#define NOT_VM
#include "Profile.h"        // from VM header

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

/* Version number in the key file.
 * Version 1 uses one byte for the thread id.
 * Version 2 uses two bytes for the thread ids.
 * Version 3 encodes the record size and adds an optional extra timestamp field.
 */
int versionNumber;

/* arbitrarily limit indentation */
#define MAX_STACK_DEPTH 10000

/* thread list in key file is not reliable, so just max out */
#define MAX_THREADS     32768

/*
 * Values from the header of the data file.
 */
typedef struct DataHeader {
    unsigned int magic;
    short version;
    short offsetToData;
    long long startWhen;
    short recordSize;
} DataHeader;

/*
 * Entry from the thread list.
 */
typedef struct ThreadEntry {
    int         threadId;
    const char* threadName;
} ThreadEntry;

struct MethodEntry;
typedef struct TimedMethod {
    struct TimedMethod *next;
    uint64_t elapsedInclusive;
    int numCalls;
    struct MethodEntry *method;
} TimedMethod;

typedef struct ClassEntry {
    const char *className;
    uint64_t elapsedExclusive;
    int numMethods;
    struct MethodEntry **methods;       /* list of methods in this class */
    int numCalls[2];                    /* 0=normal, 1=recursive */
} ClassEntry;

typedef struct UniqueMethodEntry {
    uint64_t elapsedExclusive;
    int numMethods;
    struct MethodEntry **methods;       /* list of methods with same name */
    int numCalls[2];                    /* 0=normal, 1=recursive */
} UniqueMethodEntry;

/*
 * Entry from the method list.
 */
typedef struct MethodEntry {
    int64_t methodId;
    const char* className;
    const char* methodName;
    const char* signature;
    const char* fileName;
    int lineNum;
    uint64_t elapsedExclusive;
    uint64_t elapsedInclusive;
    uint64_t topExclusive;              /* non-recursive exclusive time */
    uint64_t recursiveInclusive;
    struct TimedMethod *parents[2];     /* 0=normal, 1=recursive */
    struct TimedMethod *children[2];    /* 0=normal, 1=recursive */
    int numCalls[2];                    /* 0=normal, 1=recursive */
    int index;                  /* used after sorting to number methods */
    int recursiveEntries;       /* number of entries on the stack */
    int graphState;             /* used when graphing to see if this method has been visited before */
} MethodEntry;

/*
 * The parsed contents of the key file.
 */
typedef struct DataKeys {
    char*        fileData;      /* contents of the entire file */
    long         fileLen;
    int          numThreads;
    ThreadEntry* threads;
    int          numMethods;
    MethodEntry* methods;       /* 2 extra methods: "toplevel" and "unknown" */
} DataKeys;

#define TOPLEVEL_INDEX 0
#define UNKNOWN_INDEX 1

typedef struct StackEntry {
    MethodEntry *method;
    uint64_t    entryTime;
} StackEntry;

typedef struct CallStack {
    int         top;
    StackEntry  calls[MAX_STACK_DEPTH];
    uint64_t    lastEventTime;
    uint64_t    threadStartTime;
} CallStack;

typedef struct DiffEntry {
    MethodEntry* method1;
    MethodEntry* method2;
    int64_t differenceExclusive;
    int64_t differenceInclusive;
    double differenceExclusivePercentage;
    double differenceInclusivePercentage;
} DiffEntry;


typedef struct TraceData {
    int numClasses;
    ClassEntry *classes;
    CallStack *stacks[MAX_THREADS];
    int depth[MAX_THREADS];
    int numUniqueMethods;
    UniqueMethodEntry *uniqueMethods;
} TraceData;

/* Initializes a MethodEntry
 */
void initMethodEntry(MethodEntry *method, int64_t methodId,
                     const char *className, const char *methodName,
                     const char *signature, const char* fileName,
                     const char* lineNumStr)
{
    method->methodId = methodId;
    method->className = className;
    method->methodName = methodName;
    method->signature = signature;
    method->fileName = fileName;
    method->lineNum = (lineNumStr != NULL) ? atoi(lineNumStr) : -1;
    method->elapsedExclusive = 0;
    method->elapsedInclusive = 0;
    method->topExclusive = 0;
    method->recursiveInclusive = 0;
    method->parents[0] = NULL;
    method->parents[1] = NULL;
    method->children[0] = NULL;
    method->children[1] = NULL;
    method->numCalls[0] = 0;
    method->numCalls[1] = 0;
    method->index = 0;
    method->recursiveEntries = 0;
}

/*
 * This comparison function is called from qsort() to sort
 * TimedMethods into decreasing order of inclusive elapsed time.
 */
int compareTimedMethod(const void *a, const void *b) {
    const TimedMethod *timedA, *timedB;
    uint64_t elapsed1, elapsed2;
    int result;

    timedA = (TimedMethod const *)a;
    timedB = (TimedMethod const *)b;
    elapsed1 = timedA->elapsedInclusive;
    elapsed2 = timedB->elapsedInclusive;
    if (elapsed1 < elapsed2)
        return 1;
    if (elapsed1 > elapsed2)
        return -1;

    /* If the elapsed times of two methods are equal, then sort them
     * into alphabetical order.
     */
    MethodEntry *methodA = timedA->method;
    MethodEntry *methodB = timedB->method;
    result = strcmp(methodA->className, methodB->className);
    if (result == 0) {
        if (methodA->methodName == NULL || methodB->methodName == NULL) {
            int64_t idA = methodA->methodId;
            int64_t idB = methodB->methodId;
            if (idA < idB)
                return -1;
            if (idA > idB)
                return 1;
            return 0;
        }
        result = strcmp(methodA->methodName, methodB->methodName);
        if (result == 0)
            result = strcmp(methodA->signature, methodB->signature);
    }
    return result;
}

/*
 * Free a DataKeys struct.
 */
void freeDataKeys(DataKeys* pKeys)
{
    if (pKeys == NULL)
        return;

    free(pKeys->fileData);
    free(pKeys->threads);
    free(pKeys->methods);
    free(pKeys);
}

/*
 * Find the offset to the next occurrence of the specified character.
 *
 * "data" should point somewhere within the current line.  "len" is the
 * number of bytes left in the buffer.
 *
 * Returns -1 if we hit the end of the buffer.
 */
int findNextChar(const char* data, int len, char lookFor)
{
    const char* start = data;

    while (len > 0) {
        if (*data == lookFor)
            return data - start;

        data++;
        len--;
    }

    return -1;
}

/*
 * Count the number of lines until the next token.
 *
 * Returns -1 if none found before EOF.
 */
int countLinesToToken(const char* data, int len)
{
    int count = 0;
    int next;

    while (*data != TOKEN_CHAR) {
        next = findNextChar(data, len, '\n');
        if (next < 0)
            return -1;
        count++;
        data += next+1;
        len -= next+1;
    }

    return count;
}

/*
 * Make sure we're at the start of the right section.
 *
 * Returns the length of the token line, or -1 if something is wrong.
 */
int checkToken(const char* data, int len, const char* cmpStr)
{
    int cmpLen = strlen(cmpStr);
    int next;

    if (*data != TOKEN_CHAR) {
        fprintf(stderr,
            "ERROR: not at start of %s (found '%.10s')\n", cmpStr, data);
        return -1;
    }

    next = findNextChar(data, len, '\n');
    if (next < cmpLen+1)
        return -1;

    if (strncmp(data+1, cmpStr, cmpLen) != 0) {
        fprintf(stderr, "ERROR: '%s' not found (got '%.7s')\n", cmpStr, data+1);
        return -1;
    }

    return next+1;
}

/*
 * Parse the "*version" section.
 */
long parseVersion(DataKeys* pKeys, long offset, int verbose)
{
    char* data;
    char* dataEnd;
    int i, count, next;

    if (offset < 0)
        return -1;

    data = pKeys->fileData + offset;
    dataEnd = pKeys->fileData + pKeys->fileLen;
    next = checkToken(data, dataEnd - data, "version");
    if (next <= 0)
        return -1;

    data += next;

    /*
     * Count the number of items in the "version" section.
     */
    count = countLinesToToken(data, dataEnd - data);
    if (count <= 0) {
        fprintf(stderr,
            "ERROR: failed while reading version (found %d)\n", count);
        return -1;
    }

    /* find the end of the line */
    next = findNextChar(data, dataEnd - data, '\n');
    if (next < 0)
        return -1;

    data[next] = '\0';
    versionNumber = strtoul(data, NULL, 0);
    if (verbose)
        printf("VERSION: %d\n", versionNumber);

    data += next+1;

    /* skip over the rest of the stuff, which is "name=value" lines */
    for (i = 1; i < count; i++) {
        next = findNextChar(data, dataEnd - data, '\n');
        if (next < 0)
            return -1;
        //data[next] = '\0';
        //printf("IGNORING: '%s'\n", data);
        data += next+1;
    }

    return data - pKeys->fileData;
}

/*
 * Parse the "*threads" section.
 */
long parseThreads(DataKeys* pKeys, long offset)
{
    char* data;
    char* dataEnd;
    int i, next, tab, count;

    if (offset < 0)
        return -1;

    data = pKeys->fileData + offset;
    dataEnd = pKeys->fileData + pKeys->fileLen;
    next = checkToken(data, dataEnd - data, "threads");

    data += next;

    /*
     * Count the number of thread entries (one per line).
     */
    count = countLinesToToken(data, dataEnd - data);
    if (count <= 0) {
        fprintf(stderr,
            "ERROR: failed while reading threads (found %d)\n", count);
        return -1;
    }

    //printf("+++ found %d threads\n", count);
    pKeys->threads = (ThreadEntry*) malloc(sizeof(ThreadEntry) * count);
    if (pKeys->threads == NULL)
        return -1;

    /*
     * Extract all entries.
     */
    for (i = 0; i < count; i++) {
        next = findNextChar(data, dataEnd - data, '\n');
        assert(next > 0);
        data[next] = '\0';

        tab = findNextChar(data, next, '\t');
        data[tab] = '\0';

        pKeys->threads[i].threadId = atoi(data);
        pKeys->threads[i].threadName = data + tab +1;

        data += next+1;
    }

    pKeys->numThreads = count;
    return data - pKeys->fileData;
}

/*
 * Parse the "*methods" section.
 */
long parseMethods(DataKeys* pKeys, long offset)
{
    char* data;
    char* dataEnd;
    int i, next, count;

    if (offset < 0)
        return -1;

    data = pKeys->fileData + offset;
    dataEnd = pKeys->fileData + pKeys->fileLen;
    next = checkToken(data, dataEnd - data, "methods");
    if (next < 0)
        return -1;

    data += next;

    /*
     * Count the number of method entries (one per line).
     */
    count = countLinesToToken(data, dataEnd - data);
    if (count <= 0) {
        fprintf(stderr,
            "ERROR: failed while reading methods (found %d)\n", count);
        return -1;
    }

    /* Reserve an extra method at location 0 for the "toplevel" method,
     * and another extra method for all other "unknown" methods.
     */
    count += 2;
    pKeys->methods = (MethodEntry*) malloc(sizeof(MethodEntry) * count);
    if (pKeys->methods == NULL)
        return -1;
    initMethodEntry(&pKeys->methods[TOPLEVEL_INDEX], -2, "(toplevel)",
        NULL, NULL, NULL, NULL);
    initMethodEntry(&pKeys->methods[UNKNOWN_INDEX], -1, "(unknown)",
        NULL, NULL, NULL, NULL);

    /*
     * Extract all entries, starting with index 2.
     */
    for (i = UNKNOWN_INDEX + 1; i < count; i++) {
        int tab1, tab2, tab3, tab4, tab5;
        int64_t id;
        char* endptr;

        next = findNextChar(data, dataEnd - data, '\n');
        assert(next > 0);
        data[next] = '\0';

        tab1 = findNextChar(data, next, '\t');
        tab2 = findNextChar(data+(tab1+1), next-(tab1+1), '\t');
        tab3 = findNextChar(data+(tab1+tab2+2), next-(tab1+tab2+2), '\t');
        tab4 = findNextChar(data+(tab1+tab2+tab3+3),
                            next-(tab1+tab2+tab3+3), '\t');
        tab5 = findNextChar(data+(tab1+tab2+tab3+tab4+4),
                            next-(tab1+tab2+tab3+tab4+4), '\t');
        if (tab1 < 0) {
            fprintf(stderr, "ERROR: missing field on method line: '%s'\n",
                    data);
            return -1;
        }
        assert(data[tab1] == '\t');
        data[tab1] = '\0';

        id = strtoul(data, &endptr, 0);
        if (*endptr != '\0') {
            fprintf(stderr, "ERROR: bad method ID '%s'\n", data);
            return -1;
        }

        // Allow files that specify just a function name, instead of requiring
        // "class \t method \t signature"
        if (tab2 > 0 && tab3 > 0) {
            tab2 += tab1+1;
            tab3 += tab2+1;
            assert(data[tab2] == '\t');
            assert(data[tab3] == '\t');
            data[tab2] = data[tab3] = '\0';

            // This is starting to get awkward.  Allow filename and line #.
            if (tab4 > 0 && tab5 > 0) {
                tab4 += tab3+1;
                tab5 += tab4+1;

                assert(data[tab4] == '\t');
                assert(data[tab5] == '\t');
                data[tab4] = data[tab5] = '\0';

                initMethodEntry(&pKeys->methods[i], id, data + tab1 +1,
                        data + tab2 +1, data + tab3 +1, data + tab4 +1,
                        data + tab5 +1);
            } else {
                initMethodEntry(&pKeys->methods[i], id, data + tab1 +1,
                        data + tab2 +1, data + tab3 +1, NULL, NULL);
            }
        } else {
            initMethodEntry(&pKeys->methods[i], id, data + tab1 +1,
                NULL, NULL, NULL, NULL);
        }

        data += next+1;
    }

    pKeys->numMethods = count;
    return data - pKeys->fileData;
}

/*
 * Parse the "*end" section.
 */
long parseEnd(DataKeys* pKeys, long offset)
{
    char* data;
    char* dataEnd;
    int next;

    if (offset < 0)
        return -1;

    data = pKeys->fileData + offset;
    dataEnd = pKeys->fileData + pKeys->fileLen;
    next = checkToken(data, dataEnd - data, "end");
    if (next < 0)
        return -1;

    data += next;

    return data - pKeys->fileData;
}

/*
 * Sort the thread list entries.
 */
static int compareThreads(const void* thread1, const void* thread2)
{
    return ((const ThreadEntry*) thread1)->threadId -
            ((const ThreadEntry*) thread2)->threadId;
}

void sortThreadList(DataKeys* pKeys)
{
    qsort(pKeys->threads, pKeys->numThreads, sizeof(pKeys->threads[0]),
        compareThreads);
}

/*
 * Sort the method list entries.
 */
static int compareMethods(const void* meth1, const void* meth2)
{
    int64_t id1, id2;

    id1 = ((const MethodEntry*) meth1)->methodId;
    id2 = ((const MethodEntry*) meth2)->methodId;
    if (id1 < id2)
        return -1;
    if (id1 > id2)
        return 1;
    return 0;
}

void sortMethodList(DataKeys* pKeys)
{
    qsort(pKeys->methods, pKeys->numMethods, sizeof(MethodEntry),
        compareMethods);
}

/*
 * Parse the key section, and return a copy of the parsed contents.
 */
DataKeys* parseKeys(FILE *fp, int verbose)
{
    DataKeys* pKeys = NULL;
    long offset;
    int i;

    pKeys = (DataKeys*) calloc(1, sizeof(DataKeys));
    if (pKeys == NULL)
        goto fail;

    /*
     * We load the entire file into memory.  We do this, rather than memory-
     * mapping it, because we want to change some whitespace to NULs.
     */
    if (fseek(fp, 0L, SEEK_END) != 0) {
        perror("fseek");
        goto fail;
    }
    pKeys->fileLen = ftell(fp);
    if (pKeys->fileLen == 0) {
        fprintf(stderr, "Key file is empty.\n");
        goto fail;
    }
    rewind(fp);

    pKeys->fileData = (char*) malloc(pKeys->fileLen);
    if (pKeys->fileData == NULL) {
        fprintf(stderr, "ERROR: unable to alloc %ld bytes\n", pKeys->fileLen);
        goto fail;
    }

    if (fread(pKeys->fileData, 1, pKeys->fileLen, fp) != (size_t) pKeys->fileLen)
    {
        fprintf(stderr, "ERROR: unable to read %ld bytes from trace file\n",
            pKeys->fileLen);
        goto fail;
    }

    offset = 0;

    offset = parseVersion(pKeys, offset, verbose);
    offset = parseThreads(pKeys, offset);
    offset = parseMethods(pKeys, offset);
    offset = parseEnd(pKeys, offset);
    if (offset < 0)
        goto fail;

    /* Reduce our allocation now that we know where the end of the key section is. */
    pKeys->fileData = (char *)realloc(pKeys->fileData, offset);
    pKeys->fileLen = offset;
    /* Leave fp pointing to the beginning of the data section. */
    fseek(fp, offset, SEEK_SET);

    sortThreadList(pKeys);
    sortMethodList(pKeys);

    /*
     * Dump list of threads.
     */
    if (verbose) {
        printf("Threads (%d):\n", pKeys->numThreads);
        for (i = 0; i < pKeys->numThreads; i++) {
            printf("%2d %s\n",
                   pKeys->threads[i].threadId, pKeys->threads[i].threadName);
        }
    }

#if 0
    /*
     * Dump list of methods.
     */
    if (verbose) {
        printf("Methods (%d):\n", pKeys->numMethods);
        for (i = 0; i < pKeys->numMethods; i++) {
            printf("0x%08x %s : %s : %s\n",
                   pKeys->methods[i].methodId, pKeys->methods[i].className,
                   pKeys->methods[i].methodName, pKeys->methods[i].signature);
        }
    }
#endif

    return pKeys;

fail:
    freeDataKeys(pKeys);
    return NULL;
}


/*
 * Read values from the binary data file.
 */

/* Make the return value "unsigned int" instead of "unsigned short" so that
 * we can detect EOF.
 */
unsigned int read2LE(FILE* fp)
{
    unsigned int val;

    val = getc(fp);
    val |= getc(fp) << 8;
    return val;
}
unsigned int read4LE(FILE* fp)
{
    unsigned int val;

    val = getc(fp);
    val |= getc(fp) << 8;
    val |= getc(fp) << 16;
    val |= getc(fp) << 24;
    return val;
}
unsigned long long read8LE(FILE* fp)
{
    unsigned long long val;

    val = getc(fp);
    val |= (unsigned long long) getc(fp) << 8;
    val |= (unsigned long long) getc(fp) << 16;
    val |= (unsigned long long) getc(fp) << 24;
    val |= (unsigned long long) getc(fp) << 32;
    val |= (unsigned long long) getc(fp) << 40;
    val |= (unsigned long long) getc(fp) << 48;
    val |= (unsigned long long) getc(fp) << 56;
    return val;
}

/*
 * Parse the header of the data section.
 *
 * Returns with the file positioned at the start of the record data.
 */
int parseDataHeader(FILE *fp, DataHeader* pHeader)
{
    int bytesToRead;

    pHeader->magic = read4LE(fp);
    pHeader->version = read2LE(fp);
    pHeader->offsetToData = read2LE(fp);
    pHeader->startWhen = read8LE(fp);
    bytesToRead = pHeader->offsetToData - 16;
    if (pHeader->version == 1) {
        pHeader->recordSize = 9;
    } else if (pHeader->version == 2) {
        pHeader->recordSize = 10;
    } else if (pHeader->version == 3) {
        pHeader->recordSize = read2LE(fp);
        bytesToRead -= 2;
    } else {
        fprintf(stderr, "Unsupported trace file version: %d\n", pHeader->version);
        return -1;
    }

    if (fseek(fp, bytesToRead, SEEK_CUR) != 0) {
        return -1;
    }

    return 0;
}

/*
 * Look up a method by it's method ID.
 *
 * Returns NULL if no matching method was found.
 */
MethodEntry* lookupMethod(DataKeys* pKeys, int64_t methodId)
{
    int hi, lo, mid;
    int64_t id;

    lo = 0;
    hi = pKeys->numMethods - 1;

    while (hi >= lo) {
        mid = (hi + lo) / 2;

        id = pKeys->methods[mid].methodId;
        if (id == methodId)           /* match */
            return &pKeys->methods[mid];
        else if (id < methodId)       /* too low */
            lo = mid + 1;
        else                          /* too high */
            hi = mid - 1;
    }

    return NULL;
}

/*
 * Reads the next data record, and assigns the data values to threadId,
 * methodVal and elapsedTime.  On end-of-file, the threadId, methodVal,
 * and elapsedTime are unchanged.  Returns 1 on end-of-file, otherwise
 * returns 0.
 */
int readDataRecord(FILE *dataFp, DataHeader* dataHeader,
        int *threadId, unsigned int *methodVal, uint64_t *elapsedTime)
{
    int id;
    int bytesToRead;

    bytesToRead = dataHeader->recordSize;
    if (dataHeader->version == 1) {
        id = getc(dataFp);
        bytesToRead -= 1;
    } else {
        id = read2LE(dataFp);
        bytesToRead -= 2;
    }
    if (id == EOF)
        return 1;
    *threadId = id;

    *methodVal = read4LE(dataFp);
    *elapsedTime = read4LE(dataFp);
    bytesToRead -= 8;

    while (bytesToRead-- > 0) {
        getc(dataFp);
    }

    if (feof(dataFp)) {
        fprintf(stderr, "WARNING: hit EOF mid-record\n");
        return 1;
    }
    return 0;
}

/* This routine adds the given time to the parent and child methods.
 * This is called when the child routine exits, after the child has
 * been popped from the stack.  The elapsedTime parameter is the
 * duration of the child routine, including time spent in called routines.
 */
void addInclusiveTime(MethodEntry *parent, MethodEntry *child,
                      uint64_t elapsedTime)
{
    TimedMethod *pTimed;

#if 0
    bool verbose = false;
    if (strcmp(child->className, debugClassName) == 0)
        verbose = true;
#endif

    int childIsRecursive = (child->recursiveEntries > 0);
    int parentIsRecursive = (parent->recursiveEntries > 1);

    if (child->recursiveEntries == 0) {
        child->elapsedInclusive += elapsedTime;
    } else if (child->recursiveEntries == 1) {
        child->recursiveInclusive += elapsedTime;
    }
    child->numCalls[childIsRecursive] += 1;

#if 0
    if (verbose) {
        fprintf(stderr,
                "%s %d elapsedTime: %lld eI: %lld, rI: %lld\n",
                child->className, child->recursiveEntries,
                elapsedTime, child->elapsedInclusive,
                child->recursiveInclusive);
    }
#endif

    /* Find the child method in the parent */
    TimedMethod *children = parent->children[parentIsRecursive];
    for (pTimed = children; pTimed; pTimed = pTimed->next) {
        if (pTimed->method == child) {
            pTimed->elapsedInclusive += elapsedTime;
            pTimed->numCalls += 1;
            break;
        }
    }
    if (pTimed == NULL) {
        /* Allocate a new TimedMethod */
        pTimed = (TimedMethod *) malloc(sizeof(TimedMethod));
        pTimed->elapsedInclusive = elapsedTime;
        pTimed->numCalls = 1;
        pTimed->method = child;

        /* Add it to the front of the list */
        pTimed->next = children;
        parent->children[parentIsRecursive] = pTimed;
    }

    /* Find the parent method in the child */
    TimedMethod *parents = child->parents[childIsRecursive];
    for (pTimed = parents; pTimed; pTimed = pTimed->next) {
        if (pTimed->method == parent) {
            pTimed->elapsedInclusive += elapsedTime;
            pTimed->numCalls += 1;
            break;
        }
    }
    if (pTimed == NULL) {
        /* Allocate a new TimedMethod */
        pTimed = (TimedMethod *) malloc(sizeof(TimedMethod));
        pTimed->elapsedInclusive = elapsedTime;
        pTimed->numCalls = 1;
        pTimed->method = parent;

        /* Add it to the front of the list */
        pTimed->next = parents;
        child->parents[childIsRecursive] = pTimed;
    }

#if 0
    if (verbose) {
        fprintf(stderr,
                "  %s %d eI: %lld\n",
                parent->className, parent->recursiveEntries,
                pTimed->elapsedInclusive);
    }
#endif
}

/* Sorts a linked list and returns a newly allocated array containing
 * the sorted entries.
 */
TimedMethod *sortTimedMethodList(TimedMethod *list, int *num)
{
    int ii;
    TimedMethod *pTimed, *sorted;

    /* Count the elements */
    int num_entries = 0;
    for (pTimed = list; pTimed; pTimed = pTimed->next)
        num_entries += 1;
    *num = num_entries;
    if (num_entries == 0)
        return NULL;

    /* Copy all the list elements to a new array and sort them */
    sorted = (TimedMethod *) malloc(sizeof(TimedMethod) * num_entries);
    for (ii = 0, pTimed = list; pTimed; pTimed = pTimed->next, ++ii)
        memcpy(&sorted[ii], pTimed, sizeof(TimedMethod));
    qsort(sorted, num_entries, sizeof(TimedMethod), compareTimedMethod);

    /* Fix up the "next" pointers so that they work. */
    for (ii = 0; ii < num_entries - 1; ++ii)
        sorted[ii].next = &sorted[ii + 1];
    sorted[num_entries - 1].next = NULL;

    return sorted;
}

void countRecursiveEntries(CallStack *pStack, int top, MethodEntry *method)
{
    int ii;

    method->recursiveEntries = 0;
    for (ii = 0; ii < top; ++ii) {
        if (pStack->calls[ii].method == method)
            method->recursiveEntries += 1;
    }
}

void stackDump(CallStack *pStack, int top)
{
    int ii;

    for (ii = 0; ii < top; ++ii) {
        MethodEntry *method = pStack->calls[ii].method;
        uint64_t entryTime = pStack->calls[ii].entryTime;
        if (method->methodName) {
            fprintf(stderr, "  %2d: %8llu %s.%s %s\n", ii, entryTime,
                   method->className, method->methodName, method->signature);
        } else {
            fprintf(stderr, "  %2d: %8llu %s\n", ii, entryTime, method->className);
        }
    }
}

/*
 * Read the key and data files and return the MethodEntries for those files
 */
DataKeys* parseDataKeys(TraceData* traceData, const char* traceFileName, uint64_t* threadTime)
{
    DataKeys* dataKeys = NULL;
    MethodEntry **pMethods = NULL;
    MethodEntry* method;
    FILE* dataFp = NULL;
    DataHeader dataHeader;
    int ii;
    uint64_t currentTime;
    MethodEntry* caller;

    dataFp = fopen(traceFileName, "rb");
    if (dataFp == NULL)
        goto bail;

    if ((dataKeys = parseKeys(dataFp, 0)) == NULL)
        goto bail;

    if (parseDataHeader(dataFp, &dataHeader) < 0)
        goto bail;

#if 1
    FILE *dumpStream = stdout;
#endif
    while (1) {
        int threadId;
        unsigned int methodVal;
        int action;
        int64_t methodId;
        CallStack *pStack;
        /*
         * Extract values from file.
         */
        if (readDataRecord(dataFp, &dataHeader, &threadId, &methodVal, &currentTime))
            break;

        action = METHOD_ACTION(methodVal);
        methodId = METHOD_ID(methodVal);

        /* Get the call stack for this thread */
        pStack = traceData->stacks[threadId];

        /* If there is no call stack yet for this thread, then allocate one */
        if (pStack == NULL) {
            pStack = malloc(sizeof(CallStack));
            pStack->top = 0;
            pStack->lastEventTime = currentTime;
            pStack->threadStartTime = currentTime;
            traceData->stacks[threadId] = pStack;
        }

        /* Lookup the current method */
        method = lookupMethod(dataKeys, methodId);
        if (method == NULL)
            method = &dataKeys->methods[UNKNOWN_INDEX];

#if 1
        if (method->methodName) {
            fprintf(dumpStream, "%2d %-8llu %d %8llu r %d c %d %s.%s %s\n",
                    threadId, currentTime, action, pStack->threadStartTime,
                    method->recursiveEntries,
                    pStack->top, method->className, method->methodName,
                    method->signature);
        } else {
            fprintf(dumpStream, "%2d %-8llu %d %8llu r %d c %d %s\n",
                    threadId, currentTime, action, pStack->threadStartTime,
                    method->recursiveEntries,
                    pStack->top, method->className);
        }
#endif

        if (action == METHOD_TRACE_ENTER) {
            /* This is a method entry */
            if (pStack->top >= MAX_STACK_DEPTH) {
                fprintf(stderr, "Stack overflow (exceeded %d frames)\n",
                        MAX_STACK_DEPTH);
                exit(1);
            }

            /* Get the caller method */
            if (pStack->top >= 1)
                caller = pStack->calls[pStack->top - 1].method;
            else
                caller = &dataKeys->methods[TOPLEVEL_INDEX];
            countRecursiveEntries(pStack, pStack->top, caller);
            caller->elapsedExclusive += currentTime - pStack->lastEventTime;
#if 0
            if (caller->elapsedExclusive > 10000000)
                fprintf(dumpStream, "%llu current %llu last %llu diff %llu\n",
                        caller->elapsedExclusive, currentTime,
                        pStack->lastEventTime,
                        currentTime - pStack->lastEventTime);
#endif
            if (caller->recursiveEntries <= 1) {
                caller->topExclusive += currentTime - pStack->lastEventTime;
            }

            /* Push the method on the stack for this thread */
            pStack->calls[pStack->top].method = method;
            pStack->calls[pStack->top++].entryTime = currentTime;
        } else {
            /* This is a method exit */
            uint64_t entryTime = 0;

            /* Pop the method off the stack for this thread */
            if (pStack->top > 0) {
                pStack->top -= 1;
                entryTime = pStack->calls[pStack->top].entryTime;
                if (method != pStack->calls[pStack->top].method) {
                    if (method->methodName) {
                        fprintf(stderr,
                            "Exit from method %s.%s %s does not match stack:\n",
                            method->className, method->methodName,
                            method->signature);
                    } else {
                        fprintf(stderr,
                            "Exit from method %s does not match stack:\n",
                            method->className);
                    }
                    stackDump(pStack, pStack->top + 1);
                    exit(1);
                }
            }

            /* Get the caller method */
            if (pStack->top >= 1)
                caller = pStack->calls[pStack->top - 1].method;
            else
                caller = &dataKeys->methods[TOPLEVEL_INDEX];
            countRecursiveEntries(pStack, pStack->top, caller);
            countRecursiveEntries(pStack, pStack->top, method);
            uint64_t elapsed = currentTime - entryTime;
            addInclusiveTime(caller, method, elapsed);
            method->elapsedExclusive += currentTime - pStack->lastEventTime;
            if (method->recursiveEntries == 0) {
                method->topExclusive += currentTime - pStack->lastEventTime;
            }
        }
        /* Remember the time of the last entry or exit event */
        pStack->lastEventTime = currentTime;
    }

    /* If we have calls on the stack when the trace ends, then clean
     * up the stack and add time to the callers by pretending that we
     * are exiting from their methods now.
     */
    CallStack *pStack;
    int threadId;
    uint64_t sumThreadTime = 0;
    for (threadId = 0; threadId < MAX_THREADS; ++threadId) {
        pStack = traceData->stacks[threadId];

        /* If this thread never existed, then continue with next thread */
        if (pStack == NULL)
            continue;

        /* Also, add up the time taken by all of the threads */
        sumThreadTime += pStack->lastEventTime - pStack->threadStartTime;

        for (ii = 0; ii < pStack->top; ++ii) {
            if (ii == 0)
                caller = &dataKeys->methods[TOPLEVEL_INDEX];
            else
                caller = pStack->calls[ii - 1].method;
            method = pStack->calls[ii].method;
            countRecursiveEntries(pStack, ii, caller);
            countRecursiveEntries(pStack, ii, method);

            uint64_t entryTime = pStack->calls[ii].entryTime;
            uint64_t elapsed = pStack->lastEventTime - entryTime;
            addInclusiveTime(caller, method, elapsed);
        }
    }
    caller = &dataKeys->methods[TOPLEVEL_INDEX];
    caller->elapsedInclusive = sumThreadTime;

#if 1
    fclose(dumpStream);
#endif

    if (threadTime != NULL) {
        *threadTime = sumThreadTime;
    }

bail:
    if (dataFp != NULL)
        fclose(dataFp);

    return dataKeys;
}


int usage(const char *program)
{
    fprintf(stderr, "Copyright (C) 2006 The Android Open Source Project\n\n");
    fprintf(stderr, "usage: %s trace-file-name\n", program);
    return 2;
}

/*
 * Parse args.
 */
int main(int argc, char** argv)
{
    // Parse the options
    if (argc < 2)
        return usage(argv[0]);

    char * traceFileName = argv[1];

    uint64_t sumThreadTime = 0;

    TraceData data1;
    DataKeys* dataKeys = parseDataKeys(&data1, traceFileName,
                                       &sumThreadTime);
    if (dataKeys == NULL) {
        fprintf(stderr, "Cannot read \"%s\".\n", traceFileName);
        exit(1);
    }

    freeDataKeys(dataKeys);

    return 0;
}
