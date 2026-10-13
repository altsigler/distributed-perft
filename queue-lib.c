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
** This file contains APIs to manage Message Queues. See comments in 
** queue-lib.h for detailed API descriptions.
**
******************************************************************************/
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <time.h>
#include <malloc.h>
#include <sched.h>

#include "queue-lib.h"
#include "queue-internal.h"

/* Number of times to retry the send before blocking on full queue.
** If this value is 0 then a full queue blocks the sender after 
** the initial send attempt.
*/
#define QUEUE_SEND_RETRY_COUNT  1

/* Number of times to retry the receive before blocking on empty queue.
** If this value is 0 then an empty queue blocks the receiver after 
** the initial receive attempt.
*/
#define QUEUE_RECEIVE_RETRY_COUNT  1

#define QUEUE_CONTROL_LOCK  bsdlicMutexLock (&queue_control->mutex)
#define QUEUE_CONTROL_UNLOCK  bsdlicMutexUnlock (&queue_control->mutex)

/* Use Mutex Functions.
*/
#define QUEUE_HEAD_LOCK(a) (bsdlicMutexLock(&(a)->head_mutex))
#define QUEUE_HEAD_UNLOCK(a) (bsdlicMutexNoYieldUnlock(&(a)->head_mutex))

#define QUEUE_TAIL_LOCK(a) (bsdlicMutexLock(&(a)->tail_mutex))
#define QUEUE_TAIL_UNLOCK(a) (bsdlicMutexNoYieldUnlock(&(a)->tail_mutex))

#define QUEUE_LOCK(a)     (QUEUE_HEAD_LOCK(a), QUEUE_TAIL_LOCK(a))
#define QUEUE_UNLOCK(a)   (QUEUE_HEAD_UNLOCK(a), QUEUE_TAIL_UNLOCK(a))


static controlQueue_t *shared_queue_control = 0; 
static controlQueue_t *private_queue_control = 0; 


/******************************************************************************
** Initialize the Queue control structure.
**
******************************************************************************/
static void queueControlInit (controlQueue_t *queue_control,
                        unsigned int max_queues,
                        unsigned int queue_control_size,
                        unsigned int shared_supported,
                        char *queue_prefix)
{
  memset (queue_control, 0, queue_control_size);
  queue_control->max_queues = max_queues;
  queue_control->shared_supported = shared_supported;
  strcpy (queue_control->queue_prefix, queue_prefix);
  bsdlicMutexInit(&queue_control->mutex, shared_supported);
  queue_control->queue_control_size = queue_control_size;
}

/******************************************************************************
*******************************************************************************
** API Functions
*******************************************************************************
******************************************************************************/

/******************************************************************************
** Initialize the Queue library for the caller process. 
******************************************************************************/
int bsdlicQueueInit (unsigned int max_private_queues,
                        unsigned int max_shared_queues,
                        char *queue_prefix)
{
  int i;
  int fd;
  int rc = -1;
  int rv;
  unsigned int queue_control_size;
  char file_name[MAX_QUEUE_PREFIX_SIZE + 64];
  unsigned int shared_supported;
  controlQueue_t *queue_control;


  if ((max_shared_queues == 0) && (max_private_queues == 0))
  {
    return rc;
  }
  if ((max_shared_queues != 0) && (queue_prefix == 0))
  {
    return rc;
  }
  if (max_shared_queues != 0)
  {
    if (strlen(queue_prefix) >= MAX_QUEUE_PREFIX_SIZE)
    {
      return rc;
    }
    for (i = 0; i < (int) strlen(queue_prefix); i++)
    {
      if ((0 == isalpha(queue_prefix[i])) && (0 == isdigit(queue_prefix[i])) &&
          (queue_prefix[i] != '-') && (queue_prefix[i] != '_'))
      {
        return rc;
      }
    }
  }

  shared_queue_control = 0;
  private_queue_control = 0;

  do  /* Single-iteration loop - Used for error checking. */
  {
    if (max_shared_queues)
    {
      shared_supported = 1;
      queue_control_size = sizeof(controlQueue_t);
      queue_control_size += sizeof(msgQueue_t) * max_shared_queues;

      strcpy (file_name, queue_prefix);
      strcat (file_name, "control");
      fd = shm_open (file_name, O_CREAT | O_RDWR | O_TRUNC | O_EXCL, 0660);
      if (fd < 0)
      {
          /* Try to open the file without creating it.
          */
          fd = shm_open (file_name, O_RDWR, 0660);
          if (fd < 0)
          {
            /* Can't access the file, so exit with error.
            */
            break;
          }
      }
      /* Obtain exclusive lock on the file before making any changes to it.
      */
      do 
      {
        rv = flock (fd, LOCK_EX);
      } while ((rv < 0) && (errno == EINTR));

      if (rv < 0)
      {
        perror ("flock");
        break;
      }
      do  /* Single-iteration loop - Used for error checking. */
      {
        struct stat st;

        (void) fstat (fd, &st);

        /* The zero file size means that the control file is not 
        ** initialized, so we need to initialize it.
        */
        if (st.st_size == 0)
        {
          do 
          {
            rv = ftruncate (fd, queue_control_size);
          } while ((rv < 0) && (errno == EINTR));

          if (rv < 0)
          {
            perror ("ftruncate");
            break;
          }
        } 

        queue_control = mmap(NULL, queue_control_size, 
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    fd, 0);
        if (queue_control == MAP_FAILED)
        {
          perror ("mmap");
          queue_control = 0;
          break;
        }
        if (st.st_size != 0)
        {
          /* The Queue control structure already exists.
          ** Verify that the maximum number of Queues is the
          ** same value as passed in the input parameter.
          */
          if (queue_control->max_queues != max_shared_queues)
          {
            (void) munmap (queue_control, queue_control_size);
            break;
          }
        } else
        {
          /* Initialize the Queue library control structure.
          */
          queueControlInit (queue_control, max_shared_queues, queue_control_size,
                          shared_supported, queue_prefix);
        }
        rc = 0;
        shared_queue_control = queue_control;
      } while (0);
      (void) flock (fd, LOCK_UN);
      (void) close (fd);
    } 

    if (max_shared_queues && !shared_queue_control)
    {
      /* We were not able to create the shared queue, so exit now.
      */
      break;
    }

    /* If private queues are supported then define the private queue
    ** control region.
    */
    if (max_private_queues > 0)
    {
      shared_supported = 0;
      queue_control_size = sizeof(controlQueue_t);
      queue_control_size += sizeof(msgQueue_t) * max_private_queues;
      rc = -1;

      /* If the Queue sharing mode is disabled then the Queue control structure is 
      ** created in local memory.
      */
      queue_control = malloc (queue_control_size);
      if (0 != queue_control)
      {
        /* Initialize the Queue library control structure.
        */
        queueControlInit (queue_control, max_private_queues, queue_control_size,
                        shared_supported, "");
        rc = 0;
        private_queue_control = queue_control;
      } 
    }
  } while (0);

  return rc;
}

/******************************************************************************
** Create a new Queue.
******************************************************************************/
int bsdlicQueueCreate (bsdlicQueue_t *queue,
                char *name,
                unsigned int queue_size,
                unsigned int shared,
                unsigned int single_sender,
                unsigned int single_receiver)
{
  int fd;
  int i;
  int rc = -1;
  int rv;
  controlQueue_t *queue_control;
  msgQueue_t *queue_list;
  msgQueue_t *msg_queue;
  unsigned char *queue_msg_buf;

  if ((queue_size < 8) || (queue_size > MAX_QUEUE_SIZE))
  {
    return rc;
  }

  if (queue_size % 4)
  {
    return rc;
  }

  if ((shared != 0) && (shared != 1))
  {
    return rc;
  }
  if ((shared != 0) && (0 == shared_queue_control))
  {
    /* Shared Queue structure is not initialized in this process context.
    */
    return rc;
  }
  if ((shared == 0) && (0 == private_queue_control))
  {
    /* Shared Queue structure is not initialized in this process context.
    */
    return rc;
  }

  /* Check input parameters.
  */
  if (0 == queue)
  {
    return rc;
  }
  if (name == 0)  
  {
    return rc;
  }
  if ((strlen(name) < 1) || (strlen(name) >= MAX_QUEUE_NAME_SIZE))
  {
    return rc;
  }
  for (i = 0;  i < (int) strlen(name); i++)
  {
    if (!isalpha(name[i]) && !isdigit(name[i]) && 
         (name[i] != '-') && (name[i] != '_'))
    {
      return rc;
    }
  }

  if (shared == 1)
  {
    queue_control = shared_queue_control;
  } else
  {
    queue_control = private_queue_control;
  }
  queue_list = (msgQueue_t *) (((unsigned char *) queue_control) + 
                  sizeof(controlQueue_t));

  QUEUE_CONTROL_LOCK;
  do  /* Single-iteration loop - Used for error checking. */
  {
    if (queue_control->num_queues == queue_control->max_queues)
    {
      /* Already created the maximum number of Queues.
      */
      break;
    }

    /* Verify that the specified Queue name doesn't already exist.
    */ 
    for (i = 0; i < (int) queue_control->max_queues; i++)
    {
      if (0 == strcmp(queue_list[i].name, name))
          
      {
        break;
      }
    }

    if (i != (int) queue_control->max_queues)
    {
      rc = -2;
      break; /* This queue already exists. */
    }

    if (shared == 0)
    {
      /* If the queue is very large, such as tens of MBs, then 
      ** using huge pages may improve performance. Note that it is 
      ** best to keep the queue size small, so that it fits into 
      ** the data cache.
      */
      queue_msg_buf = memalign((size_t) sysconf(_SC_PAGESIZE), queue_size);
      (void) madvise (queue_msg_buf, queue_size, MADV_HUGEPAGE);
    } else
    {
      /* Verify that the Queue file name is not too big.
      */
      if ((strlen(queue_control->queue_prefix) + 
           strlen(name) + 1) >= NAME_MAX)
      {
        break;
      }

      queue_msg_buf = 0;
      do /* Single-iteration loop - Used for error checking. */
      {
        char file_name[NAME_MAX];
  
        strcpy (file_name, queue_control->queue_prefix);
        strcat (file_name, name);

        /* Delete the file if it already exists.
        */
        (void) shm_unlink(file_name);

        fd = shm_open (file_name, O_CREAT | O_RDWR | O_TRUNC, 0660);
        if (fd < 0)
        {
          perror ("shm_open");
          break;
        }
        do 
        {
          rv = ftruncate (fd,  queue_size);
        } while ((rv < 0) && (errno == EINTR));
        if (rv < 0)
        {
          perror ("ftruncate");
          (void) close (fd);
          (void) shm_unlink(file_name);
          break;
        }
        queue_msg_buf = mmap(NULL, queue_size, 
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED,
                        fd, 0);
        if (queue_msg_buf == MAP_FAILED)
        {
          perror ("mmap");
          (void) close (fd);
          (void) shm_unlink(file_name);
          queue_msg_buf = 0;
          break;
        }
        (void) close (fd);
      } while (0);
    }
    if (queue_msg_buf == 0)
    {
      break;
    }
    memset (queue_msg_buf, 0, queue_size);

    /* Add the newly created Queue to the global Queue list.
    */ 
    for (i = 0; i < (int) queue_control->max_queues; i++)
    {
      if (queue_list[i].info.queue_size == 0)
      {
        break;
      }
    }

    msg_queue = &queue_list[i];

    memset (msg_queue, 0, sizeof(msgQueue_t));
    msg_queue->single_sender = single_sender;
    msg_queue->single_receiver = single_receiver;
    msg_queue->queue_msg_buf = queue_msg_buf;
    msg_queue->info.queue_size = queue_size;
    msg_queue->shared = shared;
    strcpy (msg_queue->name, name);
    bsdlicMutexInit(&msg_queue->head_mutex, shared);
    bsdlicMutexInit(&msg_queue->tail_mutex, shared);
    msg_queue->queue_id = ++queue_control->queue_id;
    if (shared == 0)
    {
      msg_queue->queue_id &= 0x7fffffff;
    } else
    {
      msg_queue->queue_id |= 0x80000000;
    }

    queue->msg_queue = msg_queue;
    queue->queue_id = msg_queue->queue_id;

    queue_control->num_queues++;
    rc = 0;

  } while (0);
  QUEUE_CONTROL_UNLOCK;

  return rc;
}

/******************************************************************************
** Delete an existing Queue.
******************************************************************************/
int bsdlicQueueDestroy (bsdlicQueue_t *queue)
{
  int rc = -1;
  controlQueue_t *queue_control;
  msgQueue_t *msg_queue;

  if (queue == 0)
  {
    return rc;  
  }
  msg_queue = queue->msg_queue;

  if (msg_queue == 0)
  {
    return rc;
  }

  if (queue->queue_id != msg_queue->queue_id)
  {
    return rc;
  }

  if (msg_queue->shared != 0)
  {
    queue_control = shared_queue_control;
  } else
  {
    queue_control = private_queue_control;
  }

  QUEUE_CONTROL_LOCK;
  do  /* Single-iteration loop - Used for error checking. */
  {

    /* Wait until the Queue is not being accessed.
    */
    QUEUE_LOCK(msg_queue);

    /* Free the memory previously allocated for the Queue.
    */
    if (queue_control->shared_supported)
    {
      char file_name[NAME_MAX];
  
      strcpy (file_name, queue_control->queue_prefix);
      strcat (file_name, msg_queue->name);

      (void) munmap(msg_queue->queue_msg_buf, 
                      msg_queue->info.queue_size); 

      /* Delete the file 
      */
      (void) shm_unlink(file_name);
    } else
    {
      free (msg_queue->queue_msg_buf); 
    }

    /* Clear out the Queue entry from the global Queue list.
    */
    msg_queue->queue_id = 0;
    msg_queue->info.queue_size = 0;
    memset (msg_queue->name, 0, MAX_QUEUE_NAME_SIZE);

    /* Clear out the Queue handle.
    */
    memset (queue, 0, sizeof(bsdlicQueue_t));

    QUEUE_UNLOCK(msg_queue);

    queue_control->num_queues--;

    queue->msg_queue = 0;

    rc = 0;
  } while (0);
  QUEUE_CONTROL_UNLOCK;

  return rc;
}

/******************************************************************************
** Send message to the specified queue.
******************************************************************************/
int bsdlicQueueSend (bsdlicQueue_t *queue,
                unsigned char *msg,
                unsigned int  msg_size,
                unsigned int  wait_ms)
{
  int rc = -1;
  long rv;
  msgQueue_t *msg_queue;
  unsigned int queue_is_full = 0;
  unsigned int timeout_expired = 0;
  unsigned int msize;
  struct timespec timeout;
  struct timespec start_time;
  struct timespec current_time;
  unsigned int expired_ms;
  unsigned int remaining_ms = 0;
  int first_time = 1;
  int timed_wait = 0;
  unsigned int current_queue_size = 0;
  unsigned int queue_size;
  unsigned int msg_queue_head;
  unsigned int send_retry_count = 0;
  unsigned int single_sender;

  if ((queue == 0) || (msg == 0))
  {
    return rc;
  }

  if (queue->msg_queue == 0) 
  {
    return rc;
  }

  if ((wait_ms != QUEUE_NO_WAIT) && (wait_ms != QUEUE_WAIT_FOREVER))
  {
    timed_wait = 1;
  }

  msg_queue = queue->msg_queue;
  queue_size = msg_queue->info.queue_size;
  single_sender = msg_queue->single_sender;

  /* The actual message size in the queue is always a multiple of 
  ** four, and includes a 4-byte header.
  */
  msize = ((msg_size & 0x03) == 0)?msg_size:(((msg_size >> 2)+1) << 2);
  msize += 4;


  if (queue->queue_id != msg_queue->queue_id)
  {
    return rc;
  }

  if ((msg_size == 0) || (msize > queue_size))
  {
    rc = -3;
    return rc;
  } 

  if (single_sender == 0)
  {
    QUEUE_HEAD_LOCK(msg_queue);
  }
  do  /* Send Retry Loop */
  {
    current_queue_size = atomic_load(&msg_queue->info.current_queue_size);

    if (((current_queue_size & QUEUE_SIZE_MASK) + msize) > queue_size)
    {
      if (wait_ms == QUEUE_NO_WAIT)
      {
        rc = -4;
        break;
      } else
      {
        send_retry_count++;
        if (send_retry_count <= QUEUE_SEND_RETRY_COUNT)
        {
          sched_yield();
          continue;
        }
    
        (void) atomic_fetch_add (&msg_queue->info.total_queue_full_send_blocks, 1); 
        queue_is_full = 1;
      }
    }

    while (queue_is_full)
    {
      if (timed_wait && first_time)
      {
        first_time =  0;
        (void) clock_gettime (CLOCK_MONOTONIC_COARSE, &start_time);
        remaining_ms = wait_ms;
      }

      /* Wait until there is space in the queue, or the timeout expires.
      */
      if (atomic_compare_exchange_strong (&msg_queue->info.current_queue_size,
                              &current_queue_size,
                              current_queue_size | QUEUE_STATUS_SENDER_BLOCKED))
      {
        if (single_sender == 0)
        {
          QUEUE_HEAD_UNLOCK(msg_queue);
        }
        if (!timed_wait)
        {
          (void) bsdlic_futex (&msg_queue->info.current_queue_size, 
                        msg_queue->shared?FUTEX_WAIT:FUTEX_WAIT_PRIVATE,
                        current_queue_size | QUEUE_STATUS_SENDER_BLOCKED, 
                        0, 0, 0);
        } else
        {
          timeout.tv_sec = remaining_ms / 1000;
          timeout.tv_nsec = (remaining_ms % 1000) * 1000000;
          rv = bsdlic_futex (&msg_queue->info.current_queue_size, 
                        msg_queue->shared?FUTEX_WAIT:FUTEX_WAIT_PRIVATE,
                        current_queue_size | QUEUE_STATUS_SENDER_BLOCKED, 
                        &timeout, 
                        0, 0);
          if ((rv < 0) && (errno == ETIMEDOUT))
          {
            timeout_expired = 1;
          } else
          {
            (void) clock_gettime (CLOCK_MONOTONIC_COARSE, &current_time);

            /* Adjust wait time to subtract the time we already waited.
            */
            expired_ms = (unsigned int) (current_time.tv_sec - start_time.tv_sec) * 1000;
            expired_ms += (current_time.tv_nsec - start_time.tv_nsec) / 1000000;

            if (expired_ms >= wait_ms)
            {
              timeout_expired = 1;
            } else
            {
              remaining_ms = wait_ms - expired_ms;
            }
          }
        }
        if (single_sender == 0)
        {
         QUEUE_HEAD_LOCK(msg_queue);
        }
      }

      current_queue_size = atomic_load(&msg_queue->info.current_queue_size);
      if (((current_queue_size & QUEUE_SIZE_MASK) + msize) <= queue_size)
      {
        break;
      }

      if (timeout_expired)
      {
        break;
      }
    }
    if (timeout_expired)
    {
      rc = -2;
      break;
    }

    /* Copy the message size into the queue.
    ** Keep in mind that queue size and message size is divisible by 4, 
    ** so the 'head' is always aligned on a 4-byte boundary.
    */
    msg_queue_head = msg_queue->head;
    *(unsigned int *) &msg_queue->queue_msg_buf[msg_queue_head] = msg_size;
    msg_queue_head += 4;

    if ((msg_queue_head >= queue_size))
    { 
      msg_queue_head = 0;
    }

    /* Copy the message into the queue.
    */
    if (((msg_queue_head + msg_size) <= queue_size))
    {
      memcpy (&msg_queue->queue_msg_buf[msg_queue_head], msg, msg_size);
      msg_queue_head += (msize - 4);
      if ((msg_queue_head >= queue_size))
      {
        msg_queue_head = 0;
      }
    } else
   {
      memcpy (&msg_queue->queue_msg_buf[msg_queue_head], msg,
                      queue_size - msg_queue_head);
      memcpy (msg_queue->queue_msg_buf,
              &msg[queue_size - msg_queue_head],
              msg_size - (queue_size - msg_queue_head));
      msg_queue_head = (msize - 4) -
                     (queue_size - msg_queue_head);
    }
    msg_queue->head = msg_queue_head;

    /* Update queue size and wake up the receiver if needed.
    */
    current_queue_size = atomic_fetch_add (&msg_queue->info.current_queue_size, msize);
    if ((current_queue_size & QUEUE_STATUS_RECEIVER_BLOCKED))
    {
        (void) atomic_fetch_sub (&msg_queue->info.current_queue_size,
                                                  QUEUE_STATUS_RECEIVER_BLOCKED);
        (void) bsdlic_futex (&msg_queue->info.current_queue_size,
                msg_queue->shared?FUTEX_WAKE:FUTEX_WAKE_PRIVATE,
                1, 0, 0, 0);
    }


    rc = 0;
    break;
  } while (1);
  if (single_sender == 0)
  {
    QUEUE_HEAD_UNLOCK(msg_queue);
  }

  return rc;
}

/******************************************************************************
** Receive message from the specified queue.
******************************************************************************/
int bsdlicQueueReceive (bsdlicQueue_t *queue,
                unsigned char *msg,
                unsigned int msg_buf_size,
                unsigned int *msg_size,
                unsigned int wait_ms)
{
  int rc = -1;
  long rv;
  msgQueue_t *msg_queue;
  unsigned int queue_is_empty = 0;
  unsigned int timeout_expired = 0;
  unsigned int msize;
  struct timespec timeout;
  struct timespec start_time;
  struct timespec current_time;
  unsigned int expired_ms;
  unsigned int remaining_ms = 0;
  int first_time = 1;
  int timed_wait = 0;
  unsigned int current_queue_size;
  unsigned int queue_size;
  unsigned int rx_msg_size;
  unsigned int receive_retry_count = 0;
  unsigned int single_receiver;

  if ((queue == 0) || (msg == 0) || (msg_size == 0))
  {
    return rc;
  }

  if (queue->msg_queue == 0) 
  {
    return rc;
  }

  if ((wait_ms != QUEUE_NO_WAIT) && (wait_ms != QUEUE_WAIT_FOREVER))
  {
    timed_wait = 1;
  }

  msg_queue = queue->msg_queue;
  single_receiver = msg_queue->single_receiver;

  if (queue->queue_id != msg_queue->queue_id)
  {
    return rc;
  }

  queue_size = msg_queue->info.queue_size;

  if (single_receiver == 0)
  {
    QUEUE_TAIL_LOCK(msg_queue);
  }
  do  /* Receive Loop */
  {
    current_queue_size = atomic_load(&msg_queue->info.current_queue_size);

    if ((current_queue_size & QUEUE_SIZE_MASK) == 0)
    {
      if (wait_ms == QUEUE_NO_WAIT)
      {
        rc = -2;
        break;
      } else
      {
        receive_retry_count++;
        if (receive_retry_count <= QUEUE_RECEIVE_RETRY_COUNT)
        {
          sched_yield();
          continue;
        }

        queue_is_empty = 1;
        (void) atomic_fetch_add (&msg_queue->info.total_queue_empty_receive_blocks, 1); 
      }
    }

    while (queue_is_empty)
    {
      if (timed_wait && first_time)
      {
        first_time =  0;
        (void) clock_gettime (CLOCK_MONOTONIC_COARSE, &start_time);
        remaining_ms = wait_ms;
      }

      if (atomic_compare_exchange_strong (&msg_queue->info.current_queue_size,
                              &current_queue_size,
                              current_queue_size | QUEUE_STATUS_RECEIVER_BLOCKED))
      {
        if (single_receiver == 0)
        {
          QUEUE_TAIL_UNLOCK(msg_queue);
        }
        /* Wait until there is a message in the queue, or the timeout expires.
        */
        if (!timed_wait)
        {
          (void) bsdlic_futex (&msg_queue->info.current_queue_size, 
                        msg_queue->shared?FUTEX_WAIT:FUTEX_WAIT_PRIVATE,
                        current_queue_size | QUEUE_STATUS_RECEIVER_BLOCKED, 
                        0, 0, 0);
        } else
        {
          timeout.tv_sec = remaining_ms / 1000;
          timeout.tv_nsec = (remaining_ms % 1000) * 1000000;
          rv = bsdlic_futex (&msg_queue->info.current_queue_size, 
                        msg_queue->shared?FUTEX_WAIT:FUTEX_WAIT_PRIVATE,
                        current_queue_size | QUEUE_STATUS_RECEIVER_BLOCKED, 
                        &timeout, 
                        0, 0);
          if ((rv < 0) && (errno == ETIMEDOUT))
          {
            timeout_expired = 1;
          } else
          {
            (void) clock_gettime (CLOCK_MONOTONIC_COARSE, &current_time);
  
            /* Adjust wait time to subtract the time we already waited.
            */
            expired_ms = (unsigned int) (current_time.tv_sec - start_time.tv_sec) * 1000;
            expired_ms += (current_time.tv_nsec - start_time.tv_nsec) / 1000000;
  
            if (expired_ms >= wait_ms)
            {
              timeout_expired = 1;
            } else
            {
              remaining_ms = wait_ms - expired_ms;
            }
          }
        }
        if (single_receiver == 0)
        {
          QUEUE_TAIL_LOCK(msg_queue);
        }
      }

      current_queue_size = atomic_load(&msg_queue->info.current_queue_size);
      if ((current_queue_size & QUEUE_SIZE_MASK) != 0)
      {
        break;
      }

      if (timeout_expired)
      {
        break;
      }
    }
    if (timeout_expired)
    {
      rc = -2;
      break;
    }

    /* Retrieve the message size from the queue.
    ** Keep in mind that queue size and message size is divisible by 4, 
    ** so the 'tail' is always aligned on a 4-byte boundary.
    */
    rx_msg_size = *(unsigned int *) &msg_queue->queue_msg_buf[msg_queue->tail];
    *msg_size = rx_msg_size;

    msg_queue->tail += 4;

    if (msg_queue->tail >= queue_size)
    {
      msg_queue->tail = 0;
    }

    /* The actual message size in the queue is always a multiple of
    ** four.
    */
    msize = rx_msg_size;
    if (msize & 0x03)
    {
      msize = ((msize >> 2) + 1) << 2;
    }

    /* Copy the message from the queue into the message buffer.
    */
    if ((msg_queue->tail + rx_msg_size) <= queue_size)
    {
      memcpy (msg, &msg_queue->queue_msg_buf[msg_queue->tail],
                     ((rx_msg_size) <= msg_buf_size)?rx_msg_size:msg_buf_size);
      msg_queue->tail += msize;
      if (msg_queue->tail >= queue_size)
      {
        msg_queue->tail = 0;
      }
    } else
    {
      unsigned long long segment_size;

      segment_size = queue_size - msg_queue->tail;

      memcpy (msg, &msg_queue->queue_msg_buf[msg_queue->tail],
                   ((segment_size) <= msg_buf_size)?segment_size:msg_buf_size);
     if (msg_buf_size > segment_size)
      {
        msg_buf_size -= segment_size;
        segment_size = rx_msg_size -
                    (queue_size - msg_queue->tail);
        memcpy (&msg[queue_size - msg_queue->tail],
                   msg_queue->queue_msg_buf,
                   ((segment_size) <= msg_buf_size)?segment_size:msg_buf_size);
      }
      msg_queue->tail = msize -
                     (queue_size - msg_queue->tail);
    }

    /* Update queue size and wake up the sender if needed.
    */
   current_queue_size =  atomic_fetch_sub (&msg_queue->info.current_queue_size,
                                                          (msize + 4));
    if (current_queue_size & QUEUE_STATUS_SENDER_BLOCKED)
    {
      (void) atomic_fetch_sub (&msg_queue->info.current_queue_size,
                                            QUEUE_STATUS_SENDER_BLOCKED);
      (void) bsdlic_futex (&msg_queue->info.current_queue_size,
                msg_queue->shared?FUTEX_WAKE:FUTEX_WAKE_PRIVATE,
                1, 0, 0, 0);
    }

    rc = 0;
    break;
  } while (1);
  if (single_receiver == 0)
  {
    QUEUE_TAIL_UNLOCK(msg_queue);
  }

  return rc;
}

/******************************************************************************
** Get the information about the queue.
******************************************************************************/
int bsdlicQueueInfoGet (bsdlicQueue_t *queue,
                bsdlicQueueInfo_t *info,
                unsigned int  clear_stats)
{
  int rc = -1;
  msgQueue_t *msg_queue;

  if ((queue == 0) || (info == 0))
  {
    return rc;
  }
  if (queue->msg_queue == 0) 
  {
    return rc;
  }

  msg_queue = queue->msg_queue;

  if (queue->queue_id != msg_queue->queue_id)
  {
    return rc;
  }

  info->queue_size = msg_queue->info.queue_size;
  info->current_queue_size = atomic_load (&msg_queue->info.current_queue_size);
  info->current_queue_size &= QUEUE_SIZE_MASK;
  info->total_queue_full_send_blocks = 
	  atomic_load (&msg_queue->info.total_queue_full_send_blocks);
  info->total_queue_empty_receive_blocks = 
	  atomic_load (&msg_queue->info.total_queue_empty_receive_blocks);

  if (clear_stats)
  {
    /* Reset statistics to 0.
    */
    atomic_store (&msg_queue->info.total_queue_full_send_blocks, 0);
    atomic_store (&msg_queue->info.total_queue_empty_receive_blocks, 0);
  }

  return 0;
}

/*******************************************************************************
*******************************************************************************
** Debug Functions
*******************************************************************************
******************************************************************************/
static void queueInfoShow (msgQueue_t *msg_queue)
{
  printf ("Name:%s\n", msg_queue->name);
  printf ("Queue ID:%u\n", msg_queue->queue_id);
  printf ("Shared:%u\n", msg_queue->shared);
  printf ("Single Sender:%u\n", msg_queue->single_sender);
  printf ("Single Receiver:%u\n", msg_queue->single_receiver);
  printf ("Max Queue Size:%u\n", msg_queue->info.queue_size);
  printf ("Current Queue Size:%u\n", msg_queue->info.current_queue_size);
  printf ("Total Queue Full Send Blocks:%llu\n", msg_queue->info.total_queue_full_send_blocks);
  printf ("Total Queue Empty Receive Blocks:%llu\n", 
                                        msg_queue->info.total_queue_empty_receive_blocks);
  printf ("Memory Address:%p\n", msg_queue->queue_msg_buf);
}

/******************************************************************************
** Show information about the queue control structure.
******************************************************************************/
void bsdlicDebugQueueControlShow (unsigned int shared, unsigned int queue_info)
{
  controlQueue_t *queue_control;
  int i;
  msgQueue_t *msg_queue;

  if (shared)
  {
    if (0 == shared_queue_control)
    {
      printf ("Shared Queues are not supported.\n");
      return;
    } else
    {
      printf ("Displaying information for Shared Queues.\n\n");
      queue_control = shared_queue_control;
    }
  }
  if (shared == 0)
  {
    if (0 == private_queue_control)
    {
      printf ("Private Queues are not supported.\n");
      return;
    } else
    {
      printf ("Displaying information for Private Queues.\n\n");
      queue_control = private_queue_control;
    }
  }
 
  QUEUE_CONTROL_LOCK;
  printf ("Max Queues:%u\n", queue_control->max_queues); 
  if (shared)
  {
    printf ("Queue Prefix:%s\n", queue_control->queue_prefix);
  }
  printf ("Number of Queues:%u\n", queue_control->num_queues);
  printf ("Control Structure Size:%u\n", queue_control->queue_control_size);
  printf ("Next Queue ID:%u\n", queue_control->queue_id);
  printf ("\n\n");
  if (queue_info)
  {
    msg_queue = (msgQueue_t *) ((unsigned char *) queue_control + sizeof(controlQueue_t));
    for (i = 0; i < (int) queue_control->max_queues; i++)
    {
      if (msg_queue[i].info.queue_size)
      {
        printf ("Queue Index:%d\n", i);
        queueInfoShow (&msg_queue[i]);
        printf ("\n");
      }
    }
  }

  QUEUE_CONTROL_UNLOCK;
}
