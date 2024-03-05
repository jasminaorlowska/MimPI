# MIMPI

[MPI](https://pl.wikipedia.org/wiki/Message_Passing_Interface)
is a standard communication protocol used to exchange data between processes in parallel programs,
primarily used in supercomputing. 
The goal of the task, as suggested by the name `MIMPI` - which stands for My Implementation of MPI - is to implement a small,
slightly modified fragment of MPI. According to the specifications below, you need to write:

- The code of the `mimpirun` program (in `mimpirun.c`) for running parallel computations,
- Implementation of the procedures declared in `mimpi.h` in the `mimpi.c` file.

## `Mimpirun` program

The `mimpirun` program accepts the following command line arguments:

- $n$ - the number of copies to run (it can be assumed that a natural number from 1 to 16, inclusive, will be provided).
- $prog$ - the path to the executable file (it may be located in the `PATH`).
- In case the appropriate exec call fails (e.g., due to an incorrect path),
`mimpirun` should terminate with a non-zero exit code.
- $args$ - optionally, and in any quantity, arguments to be passed to all the executed $prog$ programs.

The `mimpirun` program performs the following actions sequentially 
(the next action starts after the previous one is completely finished):

1) Prepares the environment (the implementer should definitely decide what this means).
2) Launches $n$ copies of the program $prog$, each in a separate process.
3) Waits for the completion of all created processes.
3) Exits.

## $Prog$ assumptions
- The programs $prog$ can once during their execution enter an MPI block.
To achieve this, they call the library function `MIMPI_Init` at the beginning,
and they execute the library function `MIMPI_Finalize` at the end.
An MPI block refers to the entire code fragment between the aforementioned calls.
- While in the MPI block, programs can execute various procedures from the `mimpi` library to communicate with other processes $prog$.
- They can perform any operations (write, read, open, close, etc.) on files whose file descriptor numbers are in the ranges $[0,19]$ and $[1024, \infty)$
(including STDIN, STDOUT, and STDERR).
- They do not modify the values of environment variables starting with the prefix `MIMPI`.
- They expect correctly set arguments, meaning the zeroth argument according to the Unix convention should be the name of the program $prog$,
while subsequent arguments should correspond to the arguments $args$.

## `Mimpi` library

You need to implement the following procedures with signatures from the header file `mimpi.h`:

### Helper procedures

- `void MIMPI_Init(bool enable_deadlock_detection)`

Opens the MPI block by initializing the resources needed for the `mimpi` library to function.
The `enable_deadlock_detection` flag enables deadlock detection until the end of the MPI block (described below in Enhancement 4).

- `void MIMPI_Finalize()`
Ends the _MPI block_.
All resources associated with the operation of the `mimpi` library:

- open files
- open communication channels
- allocated memory
- synchronization primitives 
etc. should be released before completing this procedure.

- `int MIMPI_World_size()`
Returns the number of processes $prog$ launched using the `mimpirun` program (should be equal to the parameter $n$ passed to the `mimpirun` call).


- `void MIMPI_World_rank()`
Returns a unique identifier within the group of processes launched by mimpirun.
Identifiers should be consecutive natural numbers from $0$ to $n-1$.

### Point-to-point communication procedures

- `MIMPI_Retcode MIMPI_Send(void const *data, int count, int destination, int tag)`

Sends data from the address data, interpreting it as an array of bytes of size count,
to the process with rank destination, tagging the message with tag.

Executing `MIMPI_Send` for a process that has already left the MPI block should
immediately fail with the error code `MIMPI_ERROR_REMOTE_FINISHED`.
There is no need to worry if the process for which `MIMPI_Send` was executed
terminates later (after the successful completion of the `MIMPI_Send` function in the sending process).

- `MIMPI_Retcode MIMPI_Recv(void *data, int count, int source, int tag)`

Waits for a message of size (exactly) count and tag tag from the process
with rank rank and stores its contents at the address data
(the caller is responsible for providing the appropriate amount of allocated memory).
The call is blocking, meaning it only finishes after receiving the entire message.

Executing `MIMPI_Recv` for a process that
did not send a matching message and has already left the MPI block should
fail with the error code `MIMPI_ERROR_REMOTE_FINISHED`.
Similar behavior is expected even if the second process leaves the MPI block
while waiting for `MIMPI_Recv`.

  - Basic version: It can be assumed that each process sends messages in exactly the order in which the recipient wants to receive them.
  However, it cannot be assumed that multiple processes do not send messages simultaneously to one recipient. 
  It can be assumed that data associated with one message is no larger than 512 bytes.
  - **Enhancement 1**: 
  The transmitted messages can be arbitrarily (reasonably) large, especially larger than the link buffer size (pipe). 
  - **Enhancement 2**: 
   Nothing can be assumed about the order of sent packets. 
   The recipient should be able to buffer incoming packets and, when calling `MIMPI_Recv`, return the first (in terms of arrival time)
   message matching the count, source, and tag parameters (there is no need to implement a very sophisticated mechanism for selecting the next matching packet;
   linear complexity with respect to the number of all unprocessed packets by the target process is sufficient). 
  - **Enhancement 3**:  
   The recipient should process received messages concurrently with performing other tasks to prevent message sending channels from overflowing. 
   In other words, sending a large number of messages is not blocking even if the receiving target does not process them
   (because they go to a growing buffer).
    **Enhancement4**: 
    Deadlock is a situation where a part of the system has reached a state where there is no possibility for further change 
    (there exists no sequence of possible future actions of processes that could resolve this deadlock). 
    Deadlock of a pair of processes is a situation where the deadlock arises from the current state of two processes 
    (considering whether they can be interrupted, we allow any actions of processes outside the pair - even those that are not allowed in their current state).
    Examples of certain situations constituting deadlocks of pairs of processes in our system include:
    - A pair of processes has mutually executed `MIMPI_Recv` on each other without previously sending a message using `MIMPI_Send` that could end the waiting for either of them.
    - One of the processes is waiting for a message from a process that is already waiting for synchronization related to the invocation of a procedure for group communication.
    
    As part of this enhancement, at least detection of deadlocks of type 1) should be implemented. 
    Detection of deadlocks of other types will not be checked (but can be implemented). Deadlocks should not be reported in situations where they are not present.
    In case a deadlock is detected, the active invocation of the library function `MIMPI_Recv` in both processes of the detected deadlock pair should immediately terminate,
    returning the error code `MIMPI_ERROR_DEADLOCK_DETECTED`.
    In the event of multiple deadlocked pairs occurring simultaneously, the invocation of the library function `MIMPI_Recv` 
    should be terminated in each process of each deadlocked pair.

Detecting deadlocks may require sending multiple auxiliary messages, which can significantly slow down the system.
Therefore, this functionality can be enabled and disabled during the execution of the entire MPI block by setting the appropriate
value of the enable_deadlock detection flag in the `MIMPI_Init` call that initiates this block.
The behavior when deadlock detection is enabled only in some processes of the current execution of `mimpirun` is undefined.

### Collective Communication Procedures

#### General Requirements

Each collective communication procedure $p$ serves as a synchronization point for all processes, meaning that instructions following the $i$-th invocation of $p$ in any process are executed after every instruction preceding the $i$-th invocation of $p$ in any other process.

If synchronization of all processes cannot be completed because one of the processes has already exited the MPI block, calling `MIMPI_Barrier` in at least one process should result in the error code `MIMPI_ERROR_REMOTE_FINISHED`.
If the process in which this occurs terminates in response to the error, calling `MIMPI_Barrier` should result in the error in at least one subsequent process.
By repeating the above behavior, we should reach a situation where each process exits the barrier with an error.

#### Efficiency

Each collective communication procedure $p$ should be implemented efficiently.
Specifically, assuming deadlock detection is inactive, we require that from the time $p$ is invoked by the last process to the time $p$ is completed in the last process, no more than $3\left \lceil\log_2(n+1)-1 \right \rceil t+\epsilon$ time where:

- $n$ is a number of processes
- $t$  is the longest time to transmit a single message - the sum of the execution times of corresponding `chsend` and `chrecv` calls. Additional information can be found in the example implementation `channel.c` and in the provided tests in the `tests/effectiveness` directory.
- $\epsilon$ is a small constant (on the order of at most milliseconds) that does not depend on $t$.

Additionally, for an implementation to be considered efficient, transmitted data should not be burdened with excessive metadata.
In particular, we expect that collective functions invoked for data sizes smaller than 256 bytes will invoke `chsend` and `chrecv` for packets of size smaller than or equal to 512 bytes.

Tests from the `tests/effectiveness directory` included in the package verify the above-defined notion of efficiency.
Passing them successfully is a necessary condition (though not necessarily sufficient) to earn points for an efficient implementation of collective functions.

#### Available Procedures

- `MIMPI_Retcode MIMPI_Barrier()`

  Performs synchronization of all processes.

- `MIMPI_Retcode MIMPI_Bcast(void *data, int count, int root)`

  Sends data provided by the process with rank root to all other processes.

- `MIMPI_Retcode MIMPI_Reduce(const void *send_data, void *recv_data, int count, MPI_Op op, int root)`

  Gathers data provided by all processes into send_data
  (treated as an array of uint8_t numbers of size count)
  and performs a reduction of type op on elements with the same indices
  from the send_data arrays of all processes (including root).
  The result of the reduction, an array of uint8_t of size count,
  is written to the address `recv_data` exclusively in the process with rank `root` (writing to `recv_data` address in other processes is not allowed).

The following reduction types (values of the enum `MIMPI_Op`) are available:

- MIMPI_MAX: maximum
- MIMPI_MIN: minimum 
- MIMPI_SUM: sum 
- MIMPI_PROD: product
It should be noted that all of the above operations on available data types
are commutative and associative, so `MIMPI_Reduce` should be optimized accordingly.

### Semantics of `MIMPI_Retcode`

See: documentation in the `mimp.h` code:

- documentation of `MIMPI_Retcode`,
- documentation of individual procedures returning `MIMPI_Retcode`.

### Semantics of Tags

Przyjmujemy konwencję:

We adopt the following convention:

- tag > 0 is intended for library users for their own purposes,

- tag = 0 denotes `ANY_TAG`. Its use in `MIMPI_Recv` causes matching to any tag.
It should not be used in `MIMPI_Send` (the effect of its use is undefined).

- tag < 0 is reserved for the library implementers and can be used for internal communication.

In particular, this means that user programs (e.g., our test programs) will never directly call `MIMPI_Send` or `MIMPI_Recv` with a tag < 0.

## Interprocess Communication

The MPI standard is designed for computations run on supercomputers.
Therefore, communication between individual processes usually occurs over a network
and is slower than data exchange within a single computer.

To better simulate the environment of a real library's operation
and thus address its implementation problems,
interprocess communication must be conducted exclusively
using the channels provided in the `channel.h` library.
The channel.h library provides the following functions for handling channels:

- void channels_init() - initialize the channel library
- void channels_finalize() - finalize the channel library
- int channel(int pipefd[2]) - create a channel 
- int chsend(int __fd, const void *__buf, size_t __n) - send a message 
- int chrecv(int __fd, void *__buf, size_t __nbytes) - receive a message

`channel`, `chsend`, `chrecv` działają podobnie do `pipe`, `write` i `read` odpowiednio.
Zamysł jest taki, że jedyna istotna (z perspektywy rozwiązania zadania)
różnica w zachowaniu funkcji zapewnionych przez `channel.h` jest
taka, że mogą one mieć znacząco dłuższy czas wykonania od swoich oryginałów.
W szczególności zapewnione funkcje:

- have the same signature as the original functions
- similarly create entries in the open file table
- guarantee atomicity of reads and writes up to 512 bytes inclusive
- guarantee the existence of a buffer of at least 4 KB in size


All reads and writes on file descriptors returned by the channel function must be performed using `chsend` and `chrecv`.
Additionally, no system functions modifying file properties such as fcntl should be called on file descriptors returned by the channel function.

It should be noted that from the guarantees provided for the `chsend` and `chrecv` functions,
it does not follow that they will not process fewer bytes than requested. 
This may happen if the requested size exceeds the guaranteed size of the channel buffer, 
or if the amount of data in the input buffer is insufficient. Implementations must correctly handle such situations.

## Notes

### General

- The `mimpirun` program or any functions from the `mimpi` library must not create named files in the file system.
- The `mimpirun` program and functions from the `mimpi` library can use descriptors numbered from $[20, 1023]$ in any way.
Additionally, it can be assumed that descriptors from this range are not occupied at the time of `mimpirun` program execution.
- The `mimpirun` program or any functions from the `mimpi` library must not modify existing entries in the open file table
from positions outside $[20, 1023]$.
- The `mimpirun` program or any functions from the `mimpi` library must not perform any operations on files they did not open themselves
(especially on STDIN, STDOUT, and STDERR).
- Active or semi-active waiting is not allowed at any point.
This means that no sleeping functions (sleep, usleep, nanosleep) or their timeout variants (such as select) should be used.
Only events independent of time, such as the appearance of a message, should be waited for.
Solutions will be tested for memory leaks and/or other resource leaks (unclosed files, etc.). 
Carefully trace and test paths that may lead to leaks.
It can be assumed that corresponding i-th calls to group communication functions in different processes are of the same types
(i.e., the same functions) and have the same parameter values count, root, and op (if the current function type has a given parameter).
In the case of an error in a system function, the calling program should terminate with a nonzero exit code, e.g., by using the provided `ASSERT_SYS_OK` macro.
If programs $prog$ use the library in a way that is inconsistent with the guarantees listed in this document,
any action can be taken (such situations will not be checked).

### `MIMPI` Library

- The implemented functions do not need to be _thread-safe_, 
meaning it can be assumed that they are not called from multiple threads simultaneously.
- Implementations of functions should be reasonably efficient,
meaning they should not add significant overhead beyond the expected execution time 
(e.g., due to waiting for a message) even under extreme loads (e.g., handling hundreds of thousands of messages).
- Calling a procedure other than `MIMPI_Init` outside of an MPI block has undefined behavior.
- Calling `MIMPI_Init` multiple times has undefined behavior.
  We guarantee that `channels_init` will set the handling of the `SIGPIPE` signal to be ignored.
This will facilitate dealing with the requirement for `MIMPI_Send` to return `MIMPI_ERROR_REMOTE_FINISHED`
in the appropriate situation.

## Package Description

Paczka zawiera następujące pliki niebędące częścią rozwiązania:

The package contains the following files that are not part of the solution:

- `examples/*`: simple example programs using the mimpi library
- `tests/*`: tests checking various configurations of running example programs using mimpirun
- `assignment.md`: this description
- `channel.h`: header file declaring functions for inter-process communication handling
- `channel.c`: example implementation of channel.h
- `mimpi.h`: header file declaring functions of the MIMPI library
- `Makefile`: sample file automating the compilation of mimpirun, example programs, and running tests
- `self`: helper script to run tests provided in the task
- `test`: script executing all tests from the tests/ directory locally
- `test_on_public_repo`: script executing tests according to the grading scheme below
- `template_hash`: file specifying the version of the template used to prepare the solution

**Templates to be filled-in**:

- mimpi.c: file containing skeletons of implementations for MIMPI library functions
- mimpirun.c: file containing skeleton implementation for the mimpirun program
- mimpi_common.h: header file for declaring common functionalities of the MIMPI library and mimpirun program
- mimpi_common.c: file for implementing common functionalities of the MIMPI library and mimpirun program

### Useful commands

- Build `mimpirun` and all examples from the examples/ directory: `make`
- Run local tests: `./test` 
- Run tests according to the official grading scheme: `./test_on_public_repo `
- The above command helps ensure that the solution meets the technical requirements outlined in the grading scheme. 
- List all files opened by processes launched by `mimpirun`: e.g., `./mimpirun 2 ls -l /proc/self/fd `
  - Track memory errors, leaks, and resources:
    - The *valgrind* tool can be useful, especially with flags like:
    - `--track-origins=yes`
    - `--track-fds=yes` 
- Another useful tool for debugging is ASAN (Address Sanitizer).
You can enable it by passing the `-fsanitize=address` flag to `gcc`.


