/*
 * Work Queue CPU affinity bug example, by Barys Chupryn, GitHub: @noop-dev
 * This app will attempt to issue independent async writes to N files while setting CPU affinity
 * To repro the bug just set the WQ affinity mask to include 2 or more isolated CPUs
 *
 * gcc -Wall -O2 -D_GNU_SOURCE -o wq-affinity wq-affinity.c -luring
 */

#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "liburing.h"

#define APP_NAME "wq-affinity V1.0 (https://github.com/noop-dev/io-uring-affinity-example)"

/* SQE ring size */
#define QD	0x100

/* Number of files to be written to */
#define NUM_FD 2
/* Block size for writes */
#define BS	0x1000
/* Filename template */
#define FILENAME "_test_%02d.bin"

typedef unsigned char u8;
typedef unsigned long long ULL;

static struct Cfg {
    u8 sqpoll;      /* Use SQPOLL */
    u8 sqe_async;   /* Force SQE ASYNC */

    unsigned wq_n_max;  /* Maximum number of workers (set separately for both types) */
    int main_cpu;       /* Main thread affinity. Single CPU index. -1: disabled */
    int sqpoll_cpu;     /* SQPOLL thread affinity. Single CPU index. -1: disabled */
    cpu_set_t worker_cpus;  /* Worker CPU affinity set. */
} cfg;

/**
 Sorry, configuration in this example is hardcoded, change as needed
 */
void set_config(void)
{
    cpu_set_t *cpus = &cfg.worker_cpus;
    CPU_ZERO(cpus);
    cfg.main_cpu = cfg.sqpoll_cpu = -1;

    CPU_SET(3, cpus);
    CPU_SET(4, cpus);
    CPU_SET(5, cpus);
    CPU_SET(6, cpus);
    CPU_SET(7, cpus);
    
/*    cfg.wq_n_max = 3; */
    cfg.sqe_async = 1;
    cfg.sqpoll = 0;
    
    cfg.sqpoll_cpu = 7;
    cfg.main_cpu = 2;
}

/**
  The rest of the vars are defined here
 */

static struct io_uring ring;

#define BUFFER_TAIL ((unsigned)-1)
static struct Buffers {
    u8 *alloc_ptr;
    size_t size;
    unsigned i_next_free, __padding;
    unsigned next[QD];
    struct iovec iovs[QD];
} buf;

static int fds[NUM_FD];
static unsigned long long n_written;
static unsigned n_free_sqe, n_prepared_sqe;
static volatile u8 should_stop;


void print_cpuset(FILE *f, cpu_set_t *set)
{
    const char *sep = "";
    unsigned start, i;
    u8 prev, bit;

    for(prev = start = i = 0; i <= CPU_SETSIZE; ++i) {
        bit = i < CPU_SETSIZE ? CPU_ISSET(i, set) : 0;
        if (bit < prev) {
            fprintf(f, (start + 1 < i ? "%s%u-%u" : "%s%u"), sep, start, i - 1);
            sep = ",";
        }
        
        start = bit > prev ? i : start;
        prev = bit;
    }
    
    if (!sep[0]) {
        fputc('*', f);
    }
}


int prepare_next_sqe(void)
{
    int i_fd;
    unsigned i_iov;
    struct io_uring_sqe *sqe;
    sqe = io_uring_get_sqe(&ring);
    if (NULL == sqe) {
        fprintf(stderr, "io_uring_get_sqe() returned NULL !! i_free_iov = %u\n", buf.i_next_free);
        return -1;
    }

    i_iov = buf.i_next_free;
    if (i_iov >= QD) {
        fprintf(stderr, "wrong buffer index: %u\n", i_iov);
        return -1;
    }

    buf.i_next_free = buf.next[i_iov];
    buf.next[i_iov] = BUFFER_TAIL; /* Sentinel value for safety */
    i_fd = n_written % NUM_FD;
    io_uring_prep_writev(sqe, i_fd /*fds[i_fd]*/, &buf.iovs[i_iov], 1, 0);
    sqe->user_data = (unsigned)i_iov;
    sqe->flags |= IOSQE_FIXED_FILE;
    if (cfg.sqe_async) {
        sqe->flags |= IOSQE_ASYNC;
    }

    ++n_written;
    --n_free_sqe;
    ++n_prepared_sqe;
    return 0;
}

void sigint_handler(int signo)
{
    printf(" ..stopping..\n");
    should_stop = 1;
}

void fail(const char *msg, int err)
{
    fprintf(stderr, errno ? "| ERROR: %s(): %d = %s\n" : "| ERROR: %s()\n", msg, err, strerror(err));
    printf("| DONE (%llu blocks submitted)\n", n_written);
    io_uring_queue_exit(&ring);
    exit(1);
}

int main(int argc, char *argv[])
{
    struct io_uring_params params;
	int ret;
    int i;
    
    set_config();

    printf("%s\n"
           "| Writing into %d files, block size: %d bytes\n"
           "| queue depth: %d, sqpoll: %d, sqe_async: %d, max.workers: %d\n"
           "| main CPU: %d, SQPOLL CPU: %d, worker CPUs: "
           , APP_NAME, NUM_FD, BS
           , QD, cfg.sqpoll, cfg.sqe_async, cfg.wq_n_max
           , cfg.main_cpu, cfg.sqpoll_cpu);
    print_cpuset(stdout, &cfg.worker_cpus);
    puts("");
    
    for (i = 0; i < NUM_FD; ++i) {
        int fd;
        char buf[0x800];

        snprintf(buf, sizeof(buf), FILENAME, i);
        fd = fds[i] = open(buf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "failed to open %s\n", buf);
            return 1;
        }
    }
    
    if (cfg.main_cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cfg.main_cpu, &set);
        ret = sched_setaffinity(gettid(), sizeof(set), &set);
        if (ret < 0) {
            fprintf(stderr, "Failed to set main thread affinity: %s\n", strerror(errno));
        }
    }

    memset(&params, 0, sizeof(params));
    params.flags |= IORING_SETUP_SINGLE_ISSUER;
    if (cfg.sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = 2000;
        
        if (cfg.sqpoll_cpu >= 0) {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = cfg.sqpoll_cpu;
        }
    } else {
        params.flags |= IORING_SETUP_COOP_TASKRUN;
    }

    ret = io_uring_queue_init_params(QD, &ring, &params);
    if (ret < 0) {
        fprintf(stderr, "queue_init: %s\n", strerror(-ret));
        return 1;
    }

    if (CPU_COUNT(&cfg.worker_cpus))
    {
        ret = io_uring_register_iowq_aff(&ring, sizeof(cfg.worker_cpus), &cfg.worker_cpus);
        if (ret) {
            fail("io_uring_register_iowq_aff", -ret);
        }
    }

    if (cfg.wq_n_max)
    {
        unsigned limits[2] = {cfg.wq_n_max, cfg.wq_n_max};
        ret = io_uring_register_iowq_max_workers(&ring, limits);
        if (ret) {
            fail("io_uring_register_iowq_max_workers", -ret);
        }
    }

    ret = io_uring_register_files(&ring, fds, NUM_FD);
    if (ret < 0) {
        fail("io_uring_register_files", -ret);
    }

    buf.alloc_ptr = mmap(NULL, buf.size = QD * BS,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);

    if (MAP_FAILED == buf.alloc_ptr) {
        fprintf(stderr, "mmap alloc failed\n");
        return 1;
    }
    
    for (i = 0; i < QD; ++i) {
        u8 *p;
        p = buf.alloc_ptr + BS * i;
        buf.iovs[i].iov_base = p;
        buf.iovs[i].iov_len = BS;
        buf.next[i] = i + 1;
        memset(p, '0' + (i & 63), BS);
    }

    buf.next[QD - 1] = BUFFER_TAIL;

#if 0
    ret = io_uring_register_buffers(&ring, iovs, QD);
    if (ret < 0) {
        fail("io_uring_register_buffers", -ret);
    }
#endif
    
    signal(SIGINT, sigint_handler);
    n_free_sqe = QD;
    while (!should_stop) {
        struct io_uring_cqe *cqe;
        unsigned i_iov;
        unsigned cqe_head;

        n_prepared_sqe = 0;
        while (n_free_sqe) {
            if (0 != prepare_next_sqe())
                goto end;
        }

        assert(BUFFER_TAIL == buf.i_next_free);
        if (n_prepared_sqe) {
            ret = io_uring_submit(&ring);
            if (ret < 0) {
                fail("io_uring_submit", -ret);
            } else if (ret < n_prepared_sqe) {
                fprintf(stderr, "io_uring_submit submitted less than expected %u: %d\n", n_prepared_sqe, ret);
                fail("io_uring_submit", 0);
                return 1;
            }
            
            /* printf("# of SQE submitted: %u\n", i); */
            /* n_prepared_sqe -= ret;
             assert(0 == n_prepared_sqe); */
        }
#if 0
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) fail("io_uring_wait_cqe", -CPU_SET(6, cpus);ret);
#endif

        i = 0;
        io_uring_for_each_cqe(&ring, cqe_head, cqe) {
            i_iov = (unsigned)cqe->user_data;
            if (i_iov >= QD || buf.next[i_iov] != BUFFER_TAIL) {
                fprintf(stderr, "| ERROR: unexpected user data: %llx\n", (ULL)cqe->user_data);
                fail("io_uring_wait_cqe", 0);
                return 1;
            }
            
            buf.next[i_iov] = buf.i_next_free;
            buf.i_next_free = i_iov;
            ++n_free_sqe;
            ++i;
        }

        if (i) {
            io_uring_smp_store_release(ring.cq.khead, cqe_head);
            /* io_uring_cq_advance(&ring, i); */
            /* printf("# of CQE returned: %u\n", i); */
        }
    }

end:
    io_uring_queue_exit(&ring);
    munmap(buf.alloc_ptr, buf.size);
    for (i = 0; i < NUM_FD; ++i) {
        close(fds[i]);
    }

    printf("| DONE (%llu blocks submitted)\n", n_written);
	return 0;
}
