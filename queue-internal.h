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
#ifndef QUEUE_INTERNAL_H_INCLUDED
#define QUEUE_INTERNAL_H_INCLUDED
#include <sys/types.h>
#include "queue-lib.h"
#include "mutex-lib.h"

/* Message Header
*/
typedef struct
{
  /* Number of user bytes in the message. The actual message is 
  ** always rounded to the next multiple of four.
  */
  unsigned int msg_length;
} msgHeader_t;

/* The Queue Status flags. These status flags are ORed into the 
** current_queue_size global variable. Since the queue size is 
** always a multiple of 4, the two least significant bits are always
** set to zero, so we can use these bits to keep track whether 
** any sender tasks/processes are blocked on a full queue or 
** whether any receiver tasks/processes are blocked on an empty
** queue.
*/
#define QUEUE_STATUS_SENDER_BLOCKED    0x00000001
#define QUEUE_STATUS_RECEIVER_BLOCKED  0x00000002
#define QUEUE_SIZE_MASK                0xFFFFFFFC

/* Message Queue Structure.
*/
typedef struct
{
  /* Unique queue ID assigned to this queue.
  */
  unsigned int queue_id;

  bsdlicQueueInfo_t info; 

  unsigned int single_sender;
  unsigned int single_receiver;

  unsigned int head;
  unsigned int tail;

  /* Flag indicating whether this queue is shared or private.
  */
  unsigned int shared;

  /* The pointer to the queue message buffer.
  */
  unsigned char *queue_msg_buf;

  bsdlicMutex_t head_mutex; /* Protect Send Function */
  bsdlicMutex_t tail_mutex; /* Protect Receive Function */

  char name[MAX_QUEUE_NAME_SIZE];

  /* Make sure that this structure aligns on the 8-byte boundary.
  ** This must be the last element in the structure.
  */
  unsigned long long alignment;
} msgQueue_t;

/* Queue Control Structure.
*/
typedef struct 
{
  unsigned int max_queues; /* Maximum number of queues */
  unsigned int shared_supported; /* These queues can be shared */

  char queue_prefix[MAX_QUEUE_PREFIX_SIZE];

  unsigned int queue_control_size;/* The size of the allocated memory region. */
  unsigned int num_queues; /* Current number of Queues. */
  bsdlicMutex_t  mutex; /* Control access to this structure */

  /* Unique queue ID. This is used to detect stale handles.
  */
  unsigned int queue_id;

  /* Alignment word to make sure that this structure size aligns on 
  ** an 8-byte boundary. This must be the last field in the structure.
  */
  unsigned long long alignment;

} controlQueue_t;

#endif /* QUEUE_INTERNAL_H_INCLUDED */
