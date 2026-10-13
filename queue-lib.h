/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */

/******************************************************************************
** LATEST REVISION/AUTHOR
** December-17-2021/Andrey Tsigler
******************************************************************************/
/******************************************************************************
** December 17, 2021 - Change Summary
**  Modified the function prototypes for bsdlicQueueCreate(), 
**  bsdlicQueueSend(), and bsdlicQueueReceive(), to move the single_sender
**  and single_receiver flags from the Send/Receive functions to the 
**  bsdlicQueueCreate() function. 
**  The code is easier to maintain when the single sender/receiver is 
**  specified once during queue creation.
******************************************************************************/
/******************************************************************************
** December 6, 2021 - Change Summary
**  1. Fix a hang in the bsdlicQueueReceive() function. Sometimes this 
**     function misses a signal from the sender, which causes it to stop 
**     receiving messages even when messages are available in the queue.
**  2. Fix a blocking issue in bsdlicQueueSend() and bsdlicQueueReceive()
**     when multiple threads or processes attempt to send or receive on 
**     the same queue. For example if one process blocks on bsdlicQueueSend()
**     because the queue is full, if another process calls bsdlicQueueSend()
**     with QUEUE_NO_WAIT option on the same queue then this process also 
**     blocks. The correct behavior is to return immediately with the -4 
**     error code for the second process, while keeping the first process
**     blocked on the queue.
**  3. Improve performance of bsdlicQueueSend() and bsdlicQueueReceive()
**     functions by making better use of local variables and replacing
**     multiply/divide functions with shifts. This change yields on the order
**     of 20% faster throughput for 8-byte messages in "single-sender"/
**     "single-receiver" mode. 
**  4. Improve performance of bsdlicQueueSend() and bsdlicQueueReceive()
**     functions when using multiple-sender and multiple-receiver mode by 
**     changing the queue locking mechanism from mutexes to spin locks.
**     This change yields about 20% faster throughput. 
**     The spin locks are safe to use when processes are scheduled using 
**     the default scheduling mode (SCHED_OTHER). However, applications 
**     which use real time scheduling modes SCHED_FIFO or SCHED_RR cannot
**     use spin locks safely if multiple processes or threads with 
**     different priorities send to the same queue or receive from 
**     the same queue. To disable the use of spin locks either change the 
**     value of BSDLIC_QUEUE_USE_SPINLOCK to 0 in queue-lib.c or pass this
**     setting on the compiler command line:
**     -DBSDLIC_QUEUE_USE_SPINLOCK=0
**  5. Iprove performance for very large non-shared queues by leveraging
**     the Huge Memory Pages. This only helps for queues that consume 
**     tens of megabytes. 
******************************************************************************/
#ifndef QUEUE_LIB_H_INCLUDED
#define QUEUE_LIB_H_INCLUDED

/******************************************************************************
** This file contains bsdlicQueue Library APIs to manage message queues.
** These message queues can be used to send messages between threads 
** and processes.
**
** The main reason for this library is to improve message throughput for 
** small messages. Small messages are commonly used in networking products, 
** for example to send a pointer to a packet buffer, while the packet 
** buffer itself is stored in shared memory. Small messages are also 
** commonly used for exchanging control information between processes 
** and threads.
** 
** Linux already supports POSIX Queues and Unix Domain Sockets
** for sending messages between processes. However, the bsdlicQueue
** library leverages user space functions as opposed to kernel functions 
** to manage the queues, which makes it much faster. For example, with 
** 8-byte messages the bsdlicQueue APIs are 5 times faster than POSIX 
** message queues and 13 times faster than Unix Domain Sockets.
**
** Additionally, the bsdlicQueueReceive() and bsdlicQueueSend() APIs support
** the "single_receiver" and "single_sender" mode. Enabling these modes 
** improves performance. For example enabling only the "single_receiver"
** mode enables the message throughput test to run 6.5 times faster than
** POSIX message queues, and enabling both, "single_receiver" and 
** "single_sender" enables the message throughput test to run 8 times
** faster than the POSIX message queues.
**
** In most use cases applications receive messages only in one process 
** or thread, so enabling the "single_receiver" is almost always OK. 
**
** It is less likely that an application can take advantage of the 
** "single_sender" optimization, since in most use cases multiple 
** threads and processes can send messages into the same queue. 
** However, in some use cases where the queue is used only to send data 
** between two processes, the "single_sender" mode can also be enabled, 
** further optimizing the queue performance. 
** 
** Another important performance consideration is setting the overall queue
** size to be smaller than the data cache size. This is particularly 
** important for larger messages. In fact, if the queue size is larger 
** than the data cache size then the Unix Domain Sockets offer higher 
** throughput if the message size in more than about 4000 bytes. 
**
** Note that sockets are the best option for applications that send
** gigabytes of data between processes and threads. The bsdlicQueue 
** library works best for short messages, where high message throughput 
** is needed as opposed to high data throughput.
**
******************************************************************************/

/* Maximum number of characters in the Queue name.
** This size includes the 0 string terminator.
*/
#define MAX_QUEUE_NAME_SIZE  64

/* Maximum number of bytes in the queue.
** Note that these queues are optimized for small message size and 
** low latency. If you need a bigger queue then you should probably
** use sockets.
*/
#define MAX_QUEUE_SIZE (1024*1024*1024)

/* Maximum number of characters in the common prefix used for all 
** files in the /dev/shm directory.
*/
#define MAX_QUEUE_PREFIX_SIZE 32

/* This timeout indicates that the caller should wait forever.
*/
#define QUEUE_WAIT_FOREVER 0xffffffff

/* This timeout indicates that the caller should return immediately.
*/
#define QUEUE_NO_WAIT 0

/* Externally visible Queue pointer.
** The user should not directly manipulate these structure elements.
*/
typedef struct 
{
  void *msg_queue;   /* Pointer to the queue control structure */
  unsigned int queue_id; /* Unique queue identifier. */
} bsdlicQueue_t;

/* Information about the queue.
*/
typedef struct
{
  /* Number of bytes allocated for the queue
  ** in the bsdlicQueueCreate() function.
  */        
  unsigned int queue_size;

  /* The current number of bytes used by all pending messages in the queue.
  */
  _Atomic unsigned int current_queue_size;

  /* Total number of sends that caused the sender to 
  ** block because the queue is full.
  */
  _Atomic unsigned long long total_queue_full_send_blocks;

  /* Total number of sends that caused the sender to wake up
  ** the receiver because the receiver was blocked on an empty
  ** queue.
  */
  _Atomic unsigned long long total_queue_empty_receive_blocks;

} bsdlicQueueInfo_t;

/* Initialize the Queue library. 
** This function must be called by every process that uses the Queues.
** This function must be called once during process initialization, before 
** any other Queue functions are invoked. Multiple invocations of this 
** function in the same process can lead to unpredictable results.
**
** This function configures the maximum number of private and shared 
** Queues supported by the process.
**
** If the max_shared_queues is 0 then the process doesn't create any 
** shared memory regions. 
**
** A process can support shared and private queues at the same time. 
** The name spaces for shared and private queues are different, so 
** it is possible to create a queue with the same name in the shared and 
** private spaces.
**
** It is an error to set both, the max_private_queues and 
** max_shared_queues to 0 at the same time.
**
** If the queue_prefix is set to a valid string then the process looks
** for an existing Queue shared region with the specified prefix. If this
** region exists then the process attaches to it, otherwise the process 
** creates a new region. For example if the queue_prefix is set to "queue-" 
** then the process checks whether there is a file called 
** "/dev/shm/queue-control". If this file exists then the process 
** attaches to the Queues defined in this shared region.
**
** Note that when using shared Queues is is important to delete all
** Queue files from the /dev/shm directory prior to starting the 
** application. Otherwise the application will use stale data which is 
** left over from the previous invocation of the application. For 
** applications that support restartability this may be a desired behavior.
**
** It is possible for multiple applications to run concurrently on the 
** same system and use different Queue regions. A different queue_prefix
** must be specified for each application. 
** 
** Only applications running with the same UID/GID as the Queue region
** creator process can access the Queue region.
** 
** max_private_queues - Total number of private Queues. The private queues
**                         are only visible by the process which invoked this 
**                         function, and by threads within that process.
**                         Set this to 0 if private Queues
**                         are not supported.
** max_shared_queues - Total number of shared Queues between all processes
**                        in the Queue region. Set this to 0 if shared Queues
**                        are not supported.
** queue_prefix - This is a string which is pre-pended to all Queue files
**              created in the /dev/shm directory. The string may contain
**              only letters, digits, '-', and '_' characters. 
**              The maximum queue_prefix length, including
**              the terminating 0 character, is MAX_QUEUE_PREFIX_SIZE.
**              This parameter must be 0 if max_shared_queues is 0.
**
** Return Values:
**  0 - Success.
** -1 - Failure.
*/
int bsdlicQueueInit (unsigned int max_private_queues,
                   unsigned int max_shared_queues,
                        char *queue_prefix);

/* Create a new Queue.
**
** When creating shared queues make sure that the /dev/shm file system
** has sufficient space for the Queue. For example to support
** Queues up to 8GB use the following command:
** sudo mount -o remount,size=8G /dev/shm
**
** queue - (output) Pointer to the Queue structure. 
** name - NULL-terminated unique name for the Queue.
**        The maximum name length, including the terminating 0 is
**        MAX_QUEUE_NAME_SIZE.
**        The name may contain letters, digits, '-', and 
**        '_' characters. Spaces or other punctuation is not 
**        allowed.
** queue_size - Number of bytes allocated for message storage for this queue.
**        The messages can vary in size. Internal storage for each message
**        rounds up the message size to the next multiple of four and 
**        adds a four-byte message header. The specified queue size must
**        be big enough for at least one message, so must be greater or
**        equal to 8. Also the queue size must be divisible by 4. 
**        The maximum queue size is 1GB. 
** shared - 0-Queue is used in one process (maybe multi-threaded). 
**          1-Queue is shared by different processes.
** single_sender - Flag indicating that only one process or thread sends
**                  messages to this queue. 
**                  0 - Multiple processes or threads are sending to the queue.
**		    1 - Only one process/thread is sending to the queue.
** single_receiver - Flag indicating that only one process or thread receives
**                  messages from this queue. 
**                  0 - Multiple processes or threads are receiving
**                      from the queue.
**		    1 - Only one process/thread is receiving from the queue.
**
** Return Values:
**  0 - Queue is created successfully.
** -1 - Error in the input parameters.
** -2 - Queue with the specified name already exists.
*/
int bsdlicQueueCreate (bsdlicQueue_t *queue,
                char *name,
                unsigned int queue_size,
                unsigned int shared,
                unsigned int single_sender,
                unsigned int single_receiver);

/* Free all resources associated with the previously created Queue.
**
** Invoking this function while other processes or threads are 
** still using the queue may lead to unpredictable results. 
** The alternative approach for cleaning up memory used by shared queues
** is to delete the Queue shared memory files from the 
** /dev/shm file system after all processes using the shared queue exit.
**
** Note that all memory for private queues is released when the process
** which created the queue terminates.
**
**  queue -  The pointer to the Queue.
**
** Return Values:
**  0 - Queue is destroyed successfully.
** -1 - Error.
*/
int bsdlicQueueDestroy (bsdlicQueue_t *queue);

/* Send the specified message on the Queue. 
** If the free space in the queue is not enough to accommodate the message 
** then the function blocks for the specified number of milliseconds to 
** wait for the free space to become available.
**
**  queue - The pointer to the Queue.
**  msg   - The pointer to the message.
**  msg_size - Number of bytes in the message. The message size must be
**             greater than 0 and less or equal to queue size minus 4.
**  wait_ms - Number of milliseconds to wait for space in the queue
**            to become available. This value must be greater than 
**            0 or set to the following special flags:
**            QUEUE_WAIT_FOREVER - Block until the space is available.
**            QUEUE_NO_WAIT - Return immediately if no space for the message.
**
** Return Values:
**  0 - Success.
** -1 - Error in input parameters.
** -2 - Timed out.
** -3 - Invalid message size.
** -4 - Queue is full.
*/
int bsdlicQueueSend (bsdlicQueue_t *queue, 
                unsigned char *msg,
                unsigned int  msg_size,
                unsigned int  wait_ms);

/* Receive the next message from the queue.
** Multiple threads or processes can invoke this function on the same 
** queue at the same time. In this case only one thread/process
** receives the message, and there is no specific order in which
** the messages are distributed to the receiver threads/processes.
**
**  queue - The pointer to the Queue.
**  msg - The pointer to the message. 
**  msg_buf_size - Number of bytes the caller allocated for the message.
**  msg_size - (output) The actual message size received from the queue.
**             If the msg_buf_size is smaller than the received message 
**             size then the message is truncated. 
**  wait_ms - Number of milliseconds to wait for the message.
**            This value must be greater than 
**            0 or set to the following special flags:
**            QUEUE_WAIT_FOREVER - Block until the message is received.
**            QUEUE_NO_WAIT - Return immediately if no message in the queue.
**
** Return Values:
**  0 - Success. 
** -1 - Error in input parameters.
** -2 - Timeout.
*/ 
int bsdlicQueueReceive (bsdlicQueue_t *queue, 
                unsigned char *msg,
                unsigned int msg_buf_size,
                unsigned int *msg_size,
                unsigned int wait_ms);

/* Get information about the queue.
**
**  queue - The pointer to the Queue.
**  info - Queue information.
**  clear_stats - When this value is non-zero statistics are reset to 0.
**
** Return Values:
**  0 - Success.
** -1 - Error in input parameters.
*/
int bsdlicQueueInfoGet (bsdlicQueue_t *queue, 
                bsdlicQueueInfo_t *info,
                unsigned int  clear_stats);

/* This is a debug function displays the content of the Queue 
** control block. Optionally the function displays information about 
** every Queue defined in the control block.
**
** Note that this function doesn't display the queued messages.
**
** shared - 0 - Show information about the private queues.
**          1 - Show information about the shared queues.
** queue_info - 0 - Don't show information about every queue.
**             1 - Show information about every queue.
**
*/
void bsdlicDebugQueueControlShow(unsigned int shared, unsigned int queue_info);

#endif /* QUEUE_LIB_H_INCLUDED */
