/*
 * Copyright (C) 2014 Dustin Lundquist <dustin@null-ptr.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>


struct Data {
    volatile size_t record_count;
    size_t max_records;
    struct Record {
        void *caller;
        volatile int status;
    } records[0];
};


void __attribute__ ((constructor)) init(void);
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
static void enable_core_dumps();
static void run_tests();
static int tests_complete();
static struct Record *lookup_caller(void *);

static const int tested_failure = 0x1;
static const int tested_success = 0x2;
static struct Data *data;
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static char *(*real_strdup)(const char *);
static char *(*real_strndup)(const char *, size_t);


void
init() {
    /* First setup our wrappers for malloc functions */
    real_malloc  = dlsym(RTLD_NEXT, "malloc");
    real_calloc  = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_strdup  = dlsym(RTLD_NEXT, "strdup");
    real_strndup = dlsym(RTLD_NEXT, "strndup");

    if (real_malloc == NULL ||
            real_calloc == NULL ||
            real_realloc == NULL) {
        fprintf(stderr, "Unable to find system malloc\n");
        exit(EXIT_FAILURE);
    }


    size_t length = sizeof(struct Data) + 1024 * sizeof(struct Record);
    data = mmap(NULL, length, PROT_READ|PROT_WRITE,
            MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (data == NULL) {
        perror("mmap failed: ");
        exit(EXIT_FAILURE);
    }
    memset(data, 0, length);
    data->record_count = 0;
    data->max_records = 1024;


    enable_core_dumps();


    run_tests();
}

void *
malloc(size_t size) {
    void *caller = __builtin_extract_return_addr(__builtin_return_address(0));
    struct Record *rec = lookup_caller(caller);

    if ((rec->status & tested_failure) == 0) {
        rec->status |= tested_failure;
        return NULL;
    } else {
        rec->status |= tested_success;
        return real_malloc(size);
    }
}

void *
calloc(size_t nmemb, size_t size) {
    void *caller = __builtin_extract_return_addr(__builtin_return_address(0));
    struct Record *rec = lookup_caller(caller);

    if ((rec->status & tested_failure) == 0) {
        rec->status |= tested_failure;
        return NULL;
    } else {
        rec->status |= tested_success;
        return real_calloc(nmemb, size);
    }
}

void *
realloc(void *ptr, size_t size) {
    void *caller = __builtin_extract_return_addr(__builtin_return_address(0));
    struct Record *rec = lookup_caller(caller);

    if ((rec->status & tested_failure) == 0) {
        rec->status |= tested_failure;
        return NULL;
    } else {
        rec->status |= tested_success;
        return real_realloc(ptr, size);
    }
}

char *
strdup(const char *s) {
    void *caller = __builtin_extract_return_addr(__builtin_return_address(0));
    struct Record *rec = lookup_caller(caller);

    if ((rec->status & tested_failure) == 0) {
        rec->status |= tested_failure;
        return NULL;
    } else {
        rec->status |= tested_success;
        return real_strdup(s);
    }
}

char *
strndup(const char *s, size_t n) {
    void *caller = __builtin_extract_return_addr(__builtin_return_address(0));
    struct Record *rec = lookup_caller(caller);

    if ((rec->status & tested_failure) == 0) {
        rec->status |= tested_failure;
        return NULL;
    } else {
        rec->status |= tested_success;
        return real_strndup(s, n);
    }
}

static void
run_tests() {
    size_t run_count = 0;

    do {
        run_count++;

        fprintf(stderr, "Run %ld...\n", run_count);

        /* Remove core dump, so we can let the next child core dump */
        unlink("./core");

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed: ");
            exit(EXIT_FAILURE);
        } else if (pid == 0) {
            /* child */
            return;
            /* will continue to execute program tracking memory allocations */
        }
        /* parent */
        int status = 0;
        waitpid(pid, &status, 0);

        /* newline to separate child output */
        fprintf(stderr, "\n");

        if (WIFSIGNALED(status)) {
            fprintf(stderr, "Run %ld terminated with signal %s (%d)\n",
                    run_count, strsignal(WTERMSIG(status)), WTERMSIG(status));

            char coredumpname[80] = { '\0' };
            snprintf(coredumpname, sizeof(coredumpname), "./core-run%ld", run_count);
            link("./core", coredumpname);
            unlink("./core");
        } else if (WIFEXITED(status)) {
            fprintf(stderr, "Run %ld exited with status %d\n", run_count,
                    WEXITSTATUS(status));
        } else {
            fprintf(stderr, "Unexpected status %x\n", status);
            exit(EXIT_FAILURE);
        }
    } while (tests_complete() == 0);

    fprintf(stderr, "Tested %ld memory allocation calls\n", data->record_count);

    fprintf(stderr, "\tCaller\t\tTested Success\tTested Failure\n");
    for (size_t i = 0; i < data->record_count; i++) {
        fprintf(stderr, "\t%p\t%s\t\t%s\n", data->records[i].caller,
                (data->records[i].status & tested_success) ? "yes" : "no",
                (data->records[i].status & tested_failure) ? "yes" : "no");
    }

    exit(EXIT_SUCCESS);
}

static int
tests_complete() {
    static size_t last_record_count = 0;
    static size_t last_checksum = 0;
    int done = 1;
    size_t checksum = 0;

    for (size_t i = 0; i < data->record_count; i++) {
        done &= (data->records[i].status & tested_failure);
        done &= (data->records[i].status & tested_success);
        checksum = checksum * 37 + data->records[i].status;
    }

    /*
     * If the tests are not making any forward progress discontinue the
     * test progress.
     */
    if (last_record_count == data->record_count && last_checksum ==
            checksum)
        done = 1;

    last_record_count = data->record_count;
    last_checksum = checksum;

    return done;
}

static struct Record *
lookup_caller(void *caller) {
    for (size_t i = 0; i < data->record_count; i++)
        if (data->records[i].caller == caller)
            return &data->records[i];

    if (data->record_count >= data->max_records) {
        fprintf(stderr, "Exceeded caller address limit %ld\n", data->max_records);
        exit(EXIT_FAILURE);
    }

    struct Record *rec = &data->records[data->record_count];
    data->record_count++;
    rec->caller = caller;
    rec->status = 0;

    return rec;
}

static void
enable_core_dumps() {
    struct rlimit core = { .rlim_cur = 0, .rlim_max = 0 };

    getrlimit(RLIMIT_CORE, &core);
    core.rlim_cur = core.rlim_max;
    setrlimit(RLIMIT_CORE, &core);
}
