# IO_URING worker CPU affinity example
## Description

This sample application performs writes to N(=2) temporary files via **io_uring** API at maximum possible speed, tries to set CPU affinity for the main thread, SQPOLL thread (if enabled), and for the worker threads spawned by **io_uring**.

Displays strange behavior when WQ CPU mask is used, making it essentially impossible to run multiple worker threads on isolated CPUs.

## Building the application
### Pre-requisites
* Linux kernel should support `io_uring` API
* C compiler & libc (tested with gcc & CLang)
* `liburing` - https://github.com/axboe/liburing
###
```
make
```
or
```
gcc -Wall -O2 -D_GNU_SOURCE -o wq-affinity src/wq-affinity.c -luring
```

## Configuration options
The following parameters can be changed by editing the source file:
* `main_cpu` - main CPU (-1 = not set)
* `sqpoll_cpu` - SQPOLL CPU, if SQPOLL enabled (-1 = not set)
* `worker_cpus` - `cpu_set_t` for worker threads
* `wq_n_max` - limit the maximum number of workers
* `sqe_async` - force creation of worker threads, if set
* `sqpoll` - enable SQPOLL thread, if set

These can be also changed, but irrelevant
* `QD` - Queue Depth (defines the size of SQE and CQE arrays)
* `BS` - Block Size (of the data repeatedly written to files)
* `NUM_FD` - Number of files (2 by default)

## Results
The following system was used for testing:
AWS instance c6in.2xlarge with Fedora Linux (6.6.3-200.fc39.x86_64)

4 out of 8 vCPUs were isolated:
`cat /sys/devices/system/cpu/isolated` -> `2-3,6-7`
 
The following results were observed:
* If only worker affinity mask is set, only non-isolated cores, works as intended
* If only worker affinity mask is set, only isolated cores, they go to the first CPU of the mask
* `sqpoll, sqpoll_cpu=3, wq_cpus=3-7` -> All workers go to CPU 3
* `sqpoll, sqpoll_cpu=2, wq_cpus=3-7` -> All workers go to CPU 3
* `sqpoll, sqpoll_cpu=1, wq_cpus=3-7` -> All workers go to CPUs 4-5
* `sqpoll, sqpoll_cpu=3, wq_cpus=4-7` -> All workers go to CPUs 4-5

Similar results observed if there is no sqpoll thread, but the main thread CPU affinity is set.
