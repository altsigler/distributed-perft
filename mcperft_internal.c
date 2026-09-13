/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <pthread.h>
#include <assert.h>
#include <sched.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#include "bytebrd_api.h"
#include "onecore_api.h"
#include "mcperft_defs.h"
#include "mcperft.h"

/*********************************************************************
** Display status and statistic for specified ply number.
**
*********************************************************************/
static void statsPrint(const chessStat_t *const stats)
{
  const unsigned long long insert_time = stats->board_db_insert_time_msec;
  const unsigned long long total_positions_examined = stats->unique_positions_added +
                                                        stats->duplicate_positions_detected;

  printf ("Run Time During Position Insertions:%'llums (%'llusec)\n",
          insert_time,
          insert_time /1000);
  printf ("Positions Per Second Rate During Database Insertion:");
  if ((insert_time/1000) && total_positions_examined)
  {
    printf ("%'llu\n", (total_positions_examined /
            (insert_time / 1000)));
  } else
  {
    printf ("Not Enough Data\n");
  }


  printf ("\n");
  printf ("Unique Positions Added:%'llu\n", stats->unique_positions_added);
  printf ("Duplicate Positions Detected:%'llu\n", stats->duplicate_positions_detected);
  printf ("Total Moves Added:%'llu\n", stats->total_moves_added);
  printf ("\n");

}

static void mcperftPlyInfoPrint(const brdDb_t *const board_db,
                        unsigned int ply_number)
{
  const chessStat_t *const stats = &board_db->ply_table[ply_number].stats;

  printf ("****************************************************\n");
  printf ("Depth:%u\n", ply_number);
  statsPrint(stats);
}

/*******************************************************************
** Print global board database information.
*******************************************************************/
static void mcperftBoardInfoPrint(const brdDb_t *const board_db)
{
  const chessStat_t *const stats = &board_db->stats;

  printf ("****************************************************\n");
  printf ("Aggregate Statistics.\n");
  printf ("Position Database Creation Time:%'llumsec (%'llusec)\n\n",
          board_db->position_db_run_time,
          board_db->position_db_run_time / 1000);
  printf ("Highest Ply With Positions:%u\n", board_db->ply_depth);
  printf ("Number of positions in ply %u is:%'llu\n",
                    board_db->ply_depth,
                    board_db->ply_table[board_db->ply_depth].num_boards_in_ply);

  printf ("\n");
  statsPrint(stats);
}


/********************************************************************
** Get system up time in milliseconds.
** The up time is based on when the process started running.
** The time wraps approximately every 49 days.
**
** Return Value:
** Time in milliseconds.
********************************************************************/
unsigned long long sysUpTimeMillisecondsGet(void)
{
  __time_t time_ms;
  struct timespec time;
  int rc;
  static int first_time = 1;
  static __time_t first_time_ms;

  rc = clock_gettime (CLOCK_MONOTONIC, &time);
  if (rc < 0)
  {
    perror ("clock_gettime CLOCK_MONOTONIC");
    exit (0);
  }
  time_ms = time.tv_sec * 1000;
  time_ms += time.tv_nsec / 1000000;

  if (first_time)
  {
    first_time = 0;
    first_time_ms = time_ms;
  }
  time_ms -=  first_time_ms;

  return (unsigned long long) time_ms;
}

/******************************************************************************
** Ply Position and Ply Move File Functions
**
** These functions are NOT thread safe.
** Only one thread can create ply positions and ply moves files.
**
** The reason this code doesn't support multi-threading is because the 
** position database creation is bound by DRAM capacity and disk access times. 
** There is no point in starting multiple threads that create the database, 
** since CPU capacity is not a major factor in the database creation time.
**
******************************************************************************/

/* To reduce file I/O we buffer positions in memory.
** When the buffer fills up, the postions are written to the file.
*/
constexpr unsigned int positionBufferMaxEntries = 1'000'000;
static plyPositionEntry_t positionBuffer[positionBufferMaxEntries];

static unsigned long long numEntriesInPositionFileBuffer;
static off_t positionFileBlockStart;
static ssize_t positionFileBlockSize;

static unsigned long long currentPositionReadIndex;
static unsigned long long lastPositionReadIndex;


/* File descriptors for position and move files. 
** The value of -1 means that the files are closed.
*/
static int positionFd = -1;
static int moveFd = -1;

/******************************************************************************
** Start new position file for the specified ply.
** The ply numbers start with 0 and go up.
**
** The function opens the ply position file. If file already exists then 
** it is deleted. The file is opened in write-only mode.
** The function also sets up the position file context parameters.
** These parameters are in global variables, so only one ply 
** position file can be opened at any one time.
******************************************************************************/
static void plyPositionFileWriteStart (const unsigned int ply)
{
  char pos_file_name[1024];

  if (positionFd != -1)
  {
    printf ("ERROR: Ply Position File is already open.\n");
    exit (-1);
  }
  sprintf (pos_file_name, "%s%u_positions", PLY_FILE_PREFIX, ply); 
  positionFd = open (pos_file_name, O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (positionFd < 0)
  {
    perror ("create ply positions file");
    exit (-1);
  }

  numEntriesInPositionFileBuffer = 0;
}

/******************************************************************************
** Start new position file for the specified ply.
** The ply numbers start with 0 and go up.
**
** The function opens the ply position file in read/only mode. 
******************************************************************************/
static void plyPositionFileReadOnlyStart (const unsigned int ply)
{
  char pos_file_name[1024];

  if (positionFd != -1)
  {
    printf ("ERROR: Ply Position File is already open.\n");
    exit (-1);
  }
  sprintf (pos_file_name, "%s%u_positions", PLY_FILE_PREFIX, ply);
  positionFd = open (pos_file_name, O_RDONLY);
  if (positionFd < 0)
  {
    perror ("open RDONLY ply positions file");
    exit (-1);
  }

  numEntriesInPositionFileBuffer = 0;
  currentPositionReadIndex = 0;
  lastPositionReadIndex = 0;
  positionFileBlockStart = 0;
  positionFileBlockSize = 0;
}


/******************************************************************************
** Open existing position file for the specified ply in preparation for 
** reading and updating position entries.
** The ply numbers start with 0 and go up.
**
** If the ply file doesn't exist then this is an error and the code exits.
** The file is opened in read-write mode.
** The function also sets up the position file context parameters.
** These parameters are in global variables, so only one ply 
** position file can be opened at any one time for reading/updating or writing.
******************************************************************************/
static void plyPositionFileReadUpdateStart (const unsigned int ply)
{
  char pos_file_name[1024];

  if (positionFd != -1)
  {
    printf ("ERROR: Ply Position File is already open.\n");
    exit (-1);
  }
  sprintf (pos_file_name, "%s%u_positions", PLY_FILE_PREFIX, ply); 
  positionFd = open (pos_file_name, O_RDWR);
  if (positionFd < 0)
  {
    perror ("open ply positions file");
    exit (-1);
  }

  numEntriesInPositionFileBuffer = 0;
  currentPositionReadIndex = 0;
  lastPositionReadIndex = 0;
  positionFileBlockStart = 0;
  positionFileBlockSize = 0;
}

/******************************************************************************
** Get next position from the open ply position file.
** The function returns a pointer of type plyPositionEntry_t.
** The pointer becomes invalid when the next call to plyPositionFileRead() 
** is made.
**
** If the ply file is not open then this is an error and the code exits.
** If the end of file is reached then the function returns 0 and the 
** ply position file is closed. 
**
** In order to reduce disk I/O, the positions are read in blocks. When the 
** end of block is reached then next block is read.
**
******************************************************************************/
static plyPositionEntry_t * plyPositionFileRead (void)
{
  if (currentPositionReadIndex != lastPositionReadIndex)
  {
    return &positionBuffer[currentPositionReadIndex++];
  }
  
  unsigned char *const buffer = (unsigned char *) positionBuffer;

  /* Read the next block from file.
  */
  positionFileBlockStart += positionFileBlockSize;
  size_t total_bytes_read = 0;
  size_t read_request_size = sizeof(positionBuffer);
  do 
  {
    const ssize_t bytes_read = read (positionFd, &buffer[total_bytes_read], 
                                            read_request_size - total_bytes_read);
    if (bytes_read < 0)
    {
      perror ("read from ply position file");
      exit (-1);
    }

    if (bytes_read == 0)
    {
      /* We reached the end of file. If we failed to read anything from the 
      ** file then close the file and return 0.
      */
      if (0 == total_bytes_read)
      {
        close (positionFd);
        positionFd = -1;
        return 0;
      }

      /* We assume that remaining entries in the file don't fill up the buffer.
      */
      break;
    }
    total_bytes_read += (size_t) bytes_read;
  } while (total_bytes_read < read_request_size);

  /* Make sure that we read a multiple of record size.
  */
  if (0 != (total_bytes_read % sizeof(plyPositionEntry_t)))
  {
    printf ("ERROR: The ply position file seems to be corrupted.\n");
    exit (-1);
  }

  positionFileBlockSize = (ssize_t) total_bytes_read;
  currentPositionReadIndex = 0;
  lastPositionReadIndex = total_bytes_read / sizeof(plyPositionEntry_t);


  return &positionBuffer[currentPositionReadIndex++];
}


/******************************************************************************
** Get next position from the open ply position file.
** The function returns a pointer of type plyPositionEntry_t.
** The caller may modify the value of the position table record by writing 
** directly into the data pointed to by the pointer. 
** The pointer becomes invalid when the next call to plyPositionFileNextGet() 
** is made.
**
** If the ply file is not open then this is an error and the code exits.
** If the end of file is reached then the function returns 0 and the 
** ply position file is closed. 
**
** In order to reduce disk I/O, the positions are read in blocks. When the 
** end of block is reached then the block is written back to the disk and the
** next block is read.
**
** This function is NOT intended to be used for simply reading the position
** file, but is intended for reading and updating the position file. 
******************************************************************************/
static plyPositionEntry_t * plyPositionFileNextGet (void)
{
  if (currentPositionReadIndex != lastPositionReadIndex)
  {
    return &positionBuffer[currentPositionReadIndex++];
  }
  
  unsigned char *const buffer = (unsigned char *) positionBuffer;

  /* If we already read a block from the file then we need
  ** to write this block back into the file.
  */
  if (0 != positionFileBlockSize)
  {
    if (positionFileBlockStart != lseek ( positionFd, positionFileBlockStart, SEEK_SET))
    {
      perror ("lseek on position file");
      exit (-1);
    }
    ssize_t total_bytes_written = 0;

    while (total_bytes_written < positionFileBlockSize)
    {
      ssize_t bytes_written = write (positionFd, &buffer[total_bytes_written],
                                        (size_t) (positionFileBlockSize - total_bytes_written));
      if (bytes_written < 0)
      {
        perror ("write to ply position file");
        exit (-1);
      }
      total_bytes_written += bytes_written;
    }
  }

  /* Read the next block from file.
  */
  positionFileBlockStart += positionFileBlockSize;
  size_t total_bytes_read = 0;
  size_t read_request_size = sizeof(positionBuffer);
  do 
  {
    const ssize_t bytes_read = read (positionFd, &buffer[total_bytes_read], 
                                            read_request_size - total_bytes_read);
    if (bytes_read < 0)
    {
      perror ("read from ply position file");
      exit (-1);
    }

    if (bytes_read == 0)
    {
      /* We reached the end of file. If we failed to read anything from the 
      ** file then close the file and return 0.
      */
      if (0 == total_bytes_read)
      {
        close (positionFd);
        positionFd = -1;
        return 0;
      }

      /* We assume that remaining entries in the file don't fill up the buffer.
      */
      break;
    }
    total_bytes_read += (size_t) bytes_read;
  } while (total_bytes_read < read_request_size);

  /* Make sure that we read a multiple of record size.
  */
  if (0 != (total_bytes_read % sizeof(plyPositionEntry_t)))
  {
    printf ("ERROR: The ply position file seems to be corrupted.\n");
    exit (-1);
  }

  positionFileBlockSize = (ssize_t) total_bytes_read;
  currentPositionReadIndex = 0;
  lastPositionReadIndex = total_bytes_read / sizeof(plyPositionEntry_t);


  return &positionBuffer[currentPositionReadIndex++];
}

/******************************************************************************
** Add new entry to the ply positions file. 
** The new entries are buffered until the buffer is full. Once the buffer
** is full the entries are written to file. 
**
******************************************************************************/
static void plyPositionFileWrite (plyPositionEntry_t *position)
{
  memcpy (&positionBuffer[numEntriesInPositionFileBuffer++],
            position, sizeof(plyPositionEntry_t));
  if (positionBufferMaxEntries == numEntriesInPositionFileBuffer)
  {
    ssize_t bytes_written;
    ssize_t total_bytes_written = 0;
    size_t write_request_size = sizeof(positionBuffer);
    const unsigned char *write_buffer = (unsigned char *) positionBuffer;

    do 
    {
      bytes_written = write (positionFd, &write_buffer[total_bytes_written], write_request_size);
      if (bytes_written < 0)
      {
        perror ("Write to ply position file");
        exit (-1);
      }
      total_bytes_written += bytes_written;
      write_request_size -= (size_t) bytes_written;
    } while (write_request_size);
    
    numEntriesInPositionFileBuffer = 0;
  }

}

/******************************************************************************
** Write any buffered position to the ply positions file, and close the file.
**
******************************************************************************/
static void plyPositionFileFinish (void)
{
  if (positionFd < 0)
  {
    printf ("ERROR: Attempting to close position file when it is already closed.\n");
    exit (-1);
  }

  if (0 != numEntriesInPositionFileBuffer)
  {
    ssize_t bytes_written;
    ssize_t total_bytes_written = 0;
    size_t write_request_size = sizeof(plyPositionEntry_t) * numEntriesInPositionFileBuffer;
    const unsigned char *write_buffer = (unsigned char *) positionBuffer;

    do
    {
      bytes_written = write (positionFd, &write_buffer[total_bytes_written], write_request_size);
      if (bytes_written < 0)
      {
        perror ("Write to ply position file");
        exit (-1);
      }
      total_bytes_written += bytes_written;
      write_request_size -= (size_t) bytes_written;
    } while (write_request_size);

  }

  numEntriesInPositionFileBuffer = 0;

  close (positionFd);
  positionFd = -1;
}

/******************************************************************************
** Start new move file for the specified ply.
** The ply numbers start with 0 and go up.
**
** The function opens the ply move file. If file already exists then 
** it is deleted. The file is opened in write-only mode.
**
** This move file is written in random locations, so must be placed
** on a solid state drive as opposed to HDD. 
******************************************************************************/
static void plyMoveFileStart (const unsigned int ply,
                              const unsigned long long num_entries)
{
  char move_file_name[1024];

  if (moveFd != -1)
  {
    printf ("ERROR: Ply Move File is already open.\n");
    exit (-1);
  }
  sprintf (move_file_name, "%s%u_moves", PLY_FILE_PREFIX, ply);
  moveFd = open (move_file_name, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (moveFd < 0)
  {
    perror ("create ply moves file");
    exit (-1);
  }

  if (0 != ftruncate (moveFd, (__off_t) (num_entries * sizeof(moveEntry_t))))
  {
    perror ("truncate - move file");
    exit (-1);
  }
}

/******************************************************************************
** Add new entry to the ply moves file.
** The new entries are buffered until the buffer is full. Once the buffer
** is full the entries are written to file.
**
******************************************************************************/
static void plyMoveFileWrite (const moveEntry_t *const move,
                              const unsigned long long entry_index)
{
  if (sizeof(moveEntry_t) != pwrite (moveFd, move, 
         sizeof(moveEntry_t), (__off_t) (entry_index * sizeof(moveEntry_t))))
  {
    perror ("Write to ply moves file");
    exit (-1);
  }
}

/******************************************************************************
** Close the move file.
**
******************************************************************************/
static void plyMoveFileFinish (void)
{
  if (moveFd < 0)
  {
    printf ("ERROR: Attempting to close move file when it is already closed.\n");
    exit (-1);
  }

  close (moveFd);
  moveFd = -1;
}



/******************************************************************************
** Create the new position database directory. 
**
** Return Values:
******************************************************************************/
static void brdDbCreate(void)
{
  int rc;

  rc = mkdir (WORK_DIRECTORY_NAME, 0777);
  if (0 != rc)
  {
    if (errno != EEXIST)
    {
      perror ("mkdir");
      exit (-1);
    }

    printf ("The position database work directory \"%s\" already exists.\n", WORK_DIRECTORY_NAME);
    printf ("Please remove this directory before creating the new position database.\n");
    exit (0);
  } 

  rc = mkdir (POSITION_DB_DIRECTORY, 0777);
  if (0 != rc)
  {
    if (errno != EEXIST)
    {
      perror ("mkdir position database directory");
      exit (-1);
    }

    exit (-1);
  } 
}

/******************************************************************************
** Read existing board database and figure out the maximum database 
** ply depth.
** If the database doesn't exist then the code exits.
**
** Return Values:
** ply_depth - The highest ply for which there are positions in this database.
******************************************************************************/
static unsigned int brdDbPlyDepthGet(void)
{
  DIR *dir;
  struct dirent *entry;

  dir = opendir (POSITION_DB_DIRECTORY);
  if (0 == dir)
  {
    perror ("Can't open position database directory.");
    printf ("Makes sure that this directory exists:%s\n", POSITION_DB_DIRECTORY);
    exit (-1);
  }

  unsigned int highest_ply = 0;
  while (0 != (entry = readdir(dir)))
  {
    if (0 == strncmp (entry->d_name, "ply_", 4))
    {
      unsigned int detected_ply = (unsigned int) (entry->d_name[4] - '0');
      if (entry->d_name[5] != '_')
      {
        detected_ply *= 10;
        detected_ply += (unsigned int) (entry->d_name[5] - '0');
      }

      if (detected_ply > highest_ply)
      {
        highest_ply = detected_ply;
      }
    }
  }
  closedir (dir);

  if ((highest_ply < 7) || (highest_ply > 10))
  {
    printf ("ERROR: Detected ply:%u. Expected ply values are between 7 and 10.\n", highest_ply);
    exit (-1);
  }

  return highest_ply;
}

/******************************************************************************
** Convert 8x8 board into 32-byte array for storing in the board database.
**
** brd - (input) 8x8 Board
** position - (output) 32-byte position data block.
**
** Return Values:
******************************************************************************/
static void brdPack (const brd_t* const brd, unsigned long long * const position)
{
  /* Convert the current board position into the cache format. 
  ** A piece uses only 4 bits in each byte of the brd_t structure, 
  ** so we fold the board on itself to generate a unique key.
  */
  position[0] = ((unsigned long *)brd)[0] | (((unsigned long *)brd)[4] << 4);
  position[1] = ((unsigned long *)brd)[1] | (((unsigned long *)brd)[5] << 4);
  position[2] = ((unsigned long *)brd)[2] | (((unsigned long *)brd)[6] << 4);
  position[3] = ((unsigned long *)brd)[3] | (((unsigned long *)brd)[7] << 4);
}

/******************************************************************************
** Convert 32-byte array for storing in the board database into 8x8 board.
**
** brd - (output) 8x8 Board
** position - (input) 32-byte position data block.
**
** Return Values:
******************************************************************************/
static void brdUnpack (brd_t* const brd, const unsigned long long * const position)
{
  ((unsigned long *)brd)[0] = position[0] & 0x0f0f0f0f0f0f0f0fLLU;
  ((unsigned long *)brd)[4] = (position[0] >> 4) & 0x0f0f0f0f0f0f0f0fLLU;
  ((unsigned long *)brd)[1] = position[1] & 0x0f0f0f0f0f0f0f0fLLU;
  ((unsigned long *)brd)[5] = (position[1] >> 4) & 0x0f0f0f0f0f0f0f0fLLU;
  ((unsigned long *)brd)[2] = position[2] & 0x0f0f0f0f0f0f0f0fLLU;
  ((unsigned long *)brd)[6] = (position[2] >> 4) & 0x0f0f0f0f0f0f0f0fLLU;
  ((unsigned long *)brd)[3] = position[3] & 0x0f0f0f0f0f0f0f0fLLU;
  ((unsigned long *)brd)[7] = (position[3] >> 4) & 0x0f0f0f0f0f0f0f0fLLU;
}
/* Externally callable version of brdUnpack()
*/
void brdUnpackBoard (brd_t* const brd, const unsigned long long * const position)
{
  brdUnpack(brd, position);
}

/******************************************************************************
** Compare two positions.
** We use a simple byte compare for the two positions. 
******************************************************************************/
static int positionCompare (const void *const p1, 
                            const void *const p2)
{
  const sortBlockEntry_t *const pos1 = p1;
  const sortBlockEntry_t *const pos2 = p2;

  const int comp = memcmp (pos1->position, pos2->position, sizeof(pos1->position));
  if (comp)
  {
    return comp;
  }
       
  if (pos1->brd_info.brd_info_mem < pos2->brd_info.brd_info_mem)
  {
    return -1;
  }
  if (pos1->brd_info.brd_info_mem > pos2->brd_info.brd_info_mem)
  {
    return 1;
  }

  return 0;

}

/******************************************************************************
** Parallel sort.
** This function can be used to sort different data types.
** The sorted block is written to the specified file.
**
** The Parallel Sort splits the block into multiple parts and starts a 
** thread to sort each part using the GNU qsort() function.
** After all parts are sorted, they are merged together and written
** into the specified file.
**
** If the number of entries is relatively small then the Parallel Sort 
** skips starting the threads and simply uses qsort() in the caller thread to 
** sort the data.
**
******************************************************************************/
typedef struct
{
  unsigned char *block;
  unsigned long long start_index;
  unsigned long long num_entries;
  size_t element_size;
  int (*compar)(const void *, const void *);
} sortParms_t;

static void * parallelSortThread (void *arg)
{
 sortParms_t *sort_parms = arg;

 qsort (sort_parms->block, sort_parms->num_entries, 
        sort_parms->element_size, sort_parms->compar);

 return 0;
}

static void parallelSort (const char *const file_name,
                          void *const block, 
                          const unsigned long long num_elements,
                          const size_t element_size,
                          int (*compar)(const void *, const void *))
{
  const int fd = open (file_name, O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd < 0)
  {
    perror ("create sort block file");
    exit (-1);
  }

  /* When the number of elements is relatively small, it is not worth the effort 
  ** to create threads. Simply call the qsort() and write results to file.
  */
  if (num_elements < 100'000)
  {
    qsort (block, num_elements, element_size, compar);

    const size_t write_size = num_elements * element_size;
    size_t total_bytes_written = 0;
    unsigned char *sort_block_buffer =  block;

    do 
    {
      const ssize_t bytes_written = write (fd, &sort_block_buffer[total_bytes_written], 
                                            write_size - total_bytes_written);
      if (bytes_written < 0)
      {
        perror ("Writing sort block file");
        exit (-1);
      }
      total_bytes_written += (size_t) bytes_written;
    } while (total_bytes_written < write_size);
    close (fd);

    return;
  }

  /* The sort algorithm uses DRAM extensively, so ultimately the performance is
  ** limited by the DRAM access speed as opposed to CPU speed. Therefore there is 
  ** no point in starting the sort thread on every available core.
  ** The below number of threads is optimized for "Intel(R) Core(TM) Ultra 7 265K"
  */
  constexpr unsigned int num_sort_threads = 10;
  sortParms_t sort_parms[num_sort_threads];
  unsigned long long start_index = 0;
  pthread_t sort_thread[num_sort_threads];

  for (unsigned int i = 0; i < num_sort_threads; i++)
  {
    sort_parms[i].start_index = start_index;
    sort_parms[i].num_entries = num_elements / num_sort_threads;
    if (i < (num_elements % num_sort_threads))
    {
      sort_parms[i].num_entries += 1;
    }
    sort_parms[i].element_size = element_size;
    sort_parms[i].compar = compar;
    sort_parms[i].block = ((unsigned char *) block) + (start_index * element_size);

    start_index += sort_parms[i].num_entries;

    if (0 != pthread_create (&sort_thread[i], 0, parallelSortThread, &sort_parms[i]))
    {
      perror ("pthread_create: - Sort Thread ");
      exit (-1);
    }
  }

  /* Wait for all sort threads to exit.
  */
  for (unsigned int i = 0; i < num_sort_threads; i++)
  {
    if (0 != pthread_join (sort_thread[i], 0))
    {
      perror ("pthread_join - Sort Thread");
      exit (-1);
    }
  }

  /* Merge sll sort blocks into the output file.
  */
  unsigned long long sort_block_index[num_sort_threads] = {};

  /* To reduce the number of I/O operations we store data in a buffer.
  ** When the buffer fills up then we write it to disk.
  */
  constexpr unsigned int max_elements_in_buffer = 100000;
  unsigned char output_buffer [max_elements_in_buffer * element_size];
  unsigned int num_elements_in_buffer = 0;

  for (unsigned long long i = 0; i < num_elements; i++)
  {
    /* Find smallest element in sll sort buffers.
    */
    unsigned char smallest_element[element_size];
    memset (smallest_element, 0xff, element_size);
    unsigned int min_position_stream;

    for (unsigned int j = 0; j < num_sort_threads; j++)
    {
      if (sort_block_index[j] == sort_parms[j].num_entries)
      {
        /* This sort block is already empty, so skip it.
        */
        continue;
      }
      unsigned char *next_block = sort_parms[j].block + 
                      (sort_block_index[j] * element_size);

      if (0 < compar(smallest_element, next_block))
      {
        memcpy (smallest_element, next_block,element_size);
        min_position_stream = j;
      }
    }

    /* Extract the smallest element from the sort buffer and put it into
    ** the output buffer.
    */
    memcpy (&output_buffer[num_elements_in_buffer * element_size],
             sort_parms[min_position_stream].block + (sort_block_index[min_position_stream] * element_size),
             element_size);
    num_elements_in_buffer++;
    sort_block_index[min_position_stream]++;

    /* If the output buffer is full or we are processing the last element then 
    ** write the output buffer to file.
    */
    if ((num_elements_in_buffer == max_elements_in_buffer) ||
        (i == (num_elements - 1)))
    {
      const unsigned int output_buffer_size = (unsigned int) (num_elements_in_buffer * element_size);
      unsigned int total_bytes_written = 0;
      do 
      {
        const ssize_t bytes_written = write (fd, &output_buffer[total_bytes_written], 
                            (output_buffer_size - total_bytes_written));
        if (bytes_written < 0)
        {
          perror ("writing sort block");
          exit (-1);
        }
        total_bytes_written += bytes_written;
      } while (total_bytes_written < output_buffer_size);

      num_elements_in_buffer = 0;
    }
  }

  close (fd);

}

/******************************************************************************
** Sort the position entries and create a temporary file.
**
**
******************************************************************************/
static void positionsTempFileCreate (void *const sort_block,
                                     const unsigned int sort_block_number,
                                     const unsigned long long sort_block_entries)
{
  char file_name[1024];

  sprintf (file_name, "%s%u", SORT_BLOCK_PREFIX, sort_block_number);
                
  parallelSort (file_name, sort_block, sort_block_entries, 
                        sizeof (sortBlockEntry_t), positionCompare);

}

/******************************************************************************
** Create all positions that can be reached from the current position.
**
**
** Return Values:
******************************************************************************/
static void nextPositionsCreate (const brd_t *const brd,
                                 const bytebrdMove_t *const dest,
                                 const unsigned int num_moves,
                                 const plyPositionEntry_t *const position_entry,
                                 sortBlockEntry_t *const next_position)
{
  /* Create next database entry for every move.
  */
  for (unsigned int i = 0; i < num_moves; i++)
  {
    brd_t next_brd = *brd;
    sortBlockEntry_t *const ndb_entry = &next_position[i];

    ndb_entry->brd_info.brd_info = position_entry->brd_info.brd_info;

    memset (&ndb_entry->move_entry, 0, sizeof(moveEntry_t));

    bytebrdUtilMoveMake (&next_brd, &dest[i], 
                            &position_entry->brd_info.brd_info, &ndb_entry->brd_info.brd_info);

    brdPack (&next_brd, ndb_entry->position);

    ndb_entry->pad = 0;
  }
}

/******************************************************************************
** Find the next entry in the specified sorted positions file.
** If the third parameter is 0 then the entry remains in the file. 
** If the third parameter is not zero then the entry is removed.
** 
** To improve performance the entries are read in groups from the file
** into memory, so most calls to this function don't access the file.
**
** Return Values:
** 0 - No entries in the specified sort block.
**   - Pointer to the next entry. The pointer is valid until next call to 
**     this function for the specified block.
******************************************************************************/
static sortBlockEntry_t *mergeBlockNextGet (mergeBlock_t *const merge_block,
                                            const unsigned int block_number,
                                            const unsigned int remove_entry)
{
  constexpr unsigned int buffered_elements = 100'000;

  mergeBlock_t *const merge_entry = &merge_block[block_number];

  if (0 != merge_entry->file_is_empty)
  {
    return 0;
  }

  /* If the file hasn't been open yet then open it now.
  */
  if (0 == merge_entry->file_is_open)
  {
    merge_entry->file_is_open = 1;
    merge_entry->buffer = malloc (buffered_elements * sizeof(sortBlockEntry_t));
    assert (merge_entry->buffer);

    merge_entry->buffer_index = 0;

    sprintf (merge_entry->file_name, "%s%u", SORT_BLOCK_PREFIX, block_number);

    merge_entry->fd = open (merge_entry->file_name, O_RDONLY);
    if (merge_entry->fd < 0)
    {
      perror ("open temporary position file");
      exit (-1);
    }
  }

  /* If we have read all elements in the current block then get the next block.
  ** Note that when the file has just been opened and nothing has been read then 
  ** buffer_index and num_elements_in_block are both 0, which triggers the 
  ** next read from file.
  */
  if (merge_entry->buffer_index == merge_entry->num_elements_in_block)
  {
    merge_entry->buffer_index = 0;

    unsigned long long total_bytes_read = 0;
    const unsigned long long read_request_size = buffered_elements * sizeof(sortBlockEntry_t);
    unsigned char *buffer = (unsigned char *) merge_entry->buffer;

    do
    {
      const ssize_t bytes_read = read (merge_entry->fd, &buffer[total_bytes_read], read_request_size);
      if (bytes_read < 0)
      {
        perror ("Read merge buffer");
        exit (-1);
      }
      if (bytes_read == 0)
      { 
        break;
      }
      total_bytes_read += (unsigned long long) bytes_read;

    } while (total_bytes_read < read_request_size);
    if (0 == total_bytes_read)
    {
      merge_entry->file_is_empty = 1;
      free (merge_entry->buffer);
      close (merge_entry->fd);
      unlink (merge_entry->file_name);
      return 0;
    }
    merge_entry->num_elements_in_block = total_bytes_read / sizeof(sortBlockEntry_t);
  }

  sortBlockEntry_t *entry = &merge_entry->buffer[merge_entry->buffer_index];

  if (remove_entry)
  {
    merge_entry->buffer_index++;
  }

  return entry;
}

/******************************************************************************
** Generate the board database from the given position.
**
** Return Values:
******************************************************************************/
static void * brd_db_generate (void *arg)
{
  void **ch_arg = (void **) arg;
  brdDb_t *const board_db = ch_arg[0];
  const unsigned int ply_number = *(unsigned int *) ch_arg[1]; 
  brdGenThreadStatus_t *gen_status = (brdGenThreadStatus_t *) ch_arg[2];
  brd_t brd; 

  bytebrdMove_t dest[MAX_BRD_MOVES];

  plyPositionEntry_t *position_entry;
  sortBlockEntry_t next_position[MAX_BRD_MOVES];
  unsigned int num_moves;
  unsigned int mover_lost;

  const unsigned long long start_of_task_msec = sysUpTimeMillisecondsGet();
  unsigned long long end_of_task_msec;

  unsigned long long sort_block_index = 0;
  plyInfo_t *const ply = &board_db->ply_table[ply_number + 1];


  plyPositionFileReadUpdateStart (ply_number);

  while (0 != (position_entry = plyPositionFileNextGet ()))
  {
    gen_status->ply_processing_phase = 1;  
    position_entry->first_move_index = moveIndexToEntry(ply->num_boards_in_ply);

    brdUnpack (&brd, position_entry->position);

    /* Find all legal moves for this position.
    */
    num_moves = bytebrdNextMoveGet (&brd, &position_entry->brd_info.brd_info, dest, &mover_lost);

    /* If there are no moves available for this position then the mover is 
    ** either in a checkmate or stalemate. 
    */
    if (0 == num_moves)
    {
      gen_status->positions_processed += 1;
      continue;
    }


    nextPositionsCreate (&brd, dest, num_moves, position_entry,
                            next_position);

    /* Copy discovered moves into the sort block.
    */
    for (unsigned int i = 0; i < num_moves; i++)
    {
     next_position[i].move_entry = moveIndexToEntry (ply->num_boards_in_ply);
     memcpy (&board_db->sort_block_position[sort_block_index], &next_position[i], sizeof(sortBlockEntry_t)); 
     ply->num_boards_in_ply++;
     sort_block_index++;
    }

    /* If the sort block is almost full then sort it and write it to a temporaty file.
    */
    if ((sort_block_index + MAX_BRD_MOVES) >= board_db->max_sortblock_positions)
    {
      gen_status->ply_processing_phase = 2;  
      positionsTempFileCreate (board_db->sort_block, 
                                    gen_status->sort_blocks_created, sort_block_index);
      sort_block_index = 0;
      gen_status->sort_blocks_created++;
    }

    position_entry->num_moves = (unsigned char) num_moves;

    gen_status->positions_processed += 1;
  }

  /* If there are any positions in the sort block then write these 
  ** positions to a temporary file.
  */
  if (0 != sort_block_index)
  {
    gen_status->ply_processing_phase = 2;  
    positionsTempFileCreate (board_db->sort_block, 
                                    gen_status->sort_blocks_created, sort_block_index);
    gen_status->sort_blocks_created++;
  }

  gen_status->total_new_ply_positions = ply->num_boards_in_ply;
  gen_status->processed_new_ply_positions = 0;
  gen_status->ply_processing_phase = 3;  

  /* Create the file for storing moves for this ply.
  */
  plyMoveFileStart (ply_number, ply->num_boards_in_ply);

  /* Open position file for the next ply.
  */
  plyPositionFileWriteStart (ply_number + 1);

  /* Merge all sorted blocks into a single position database.
  ** The move database is generated as part of this merge. 
  **
  ** The code opens all block files at the same time. 
  ** For each block file allocate a buffer so that we don't 
  ** need to do a read() call for each entry. 
  */
  mergeBlock_t sort_table[gen_status->sort_blocks_created] = {};

  /* Read until all positions from all blocks are read.
  */
  unsigned int more_positions;

  /* Number of ply positions written for this ply.
  */
  unsigned long long ply_position_index = 0;

  do 
  {
    sortBlockEntry_t min_position;
    memset (&min_position, 0xff, sizeof (sortBlockEntry_t));
    unsigned int min_position_stream;

    more_positions = 0;

    for (unsigned int i = 0; i < gen_status->sort_blocks_created; i++)
    {
      /* Get pointer to next position from specified block without removing
      ** the position from the block.
      ** Note that some of the blocks may become empty before other blocks,
      ** so we need to handle that.
      */
      const sortBlockEntry_t *const next_block = mergeBlockNextGet (sort_table, i, 0);

      if (0 != next_block)
      {
        if (0 < positionCompare(&min_position, next_block))
        {
          memcpy (&min_position, next_block,
                                    sizeof (sortBlockEntry_t));
          min_position_stream = i;
        }
        more_positions = 1;
      }
    }

    if (0 == more_positions)
    {
      /* No more positions available. We are done.
      */
      break;
    }

    /* Remove the smallest entry from the sorted block where it was found.
    */
    if (0 == mergeBlockNextGet (sort_table, min_position_stream, 1))
    {
      printf ("ERROR: Unexpected end of temp file data stream.\n");
      exit (-1);
    }
    gen_status->processed_new_ply_positions++;

    /* Create the ply position entry for the smallest position.
    */
    plyPositionEntry_t ply_position;
    memcpy (ply_position.position, min_position.position, sizeof (ply_position.position));
    ply_position.brd_info = min_position.brd_info;
    ply_position.num_moves = 0;
    ply_position.first_move_index = moveIndexToEntry(0);

    plyPositionFileWrite (&ply_position);
    ply->stats.unique_positions_added++;

    /* Update the move file for the current ply to point to the 
    ** newly added position.
    */
    const moveEntry_t next_move = moveIndexToEntry (ply_position_index++);

    plyMoveFileWrite (&next_move, moveEntryToIndex(min_position.move_entry));
    board_db->ply_table[ply_number].stats.total_moves_added++;

    /* We need to read all duplicate positions and set the moves for those 
    ** positions to point to the first unique position we found.
    */

    for (unsigned int i = 0; i < gen_status->sort_blocks_created; i++)
    {
      /* Get pointer to next position from specified block without removing
      ** the position from the block.
      ** Note that some of the blocks may become empty before other blocks,
      ** so we need to handle that.
      */
      sortBlockEntry_t *next_block;

      while ((0 != (next_block = mergeBlockNextGet(sort_table, i, 0))) &&
             (0 == positionCompare(&min_position, next_block)))
      {
        /* Create move entry for this position.
        */
        plyMoveFileWrite (&next_move, moveEntryToIndex(next_block->move_entry));
        board_db->ply_table[ply_number].stats.total_moves_added++;
        ply->stats.duplicate_positions_detected++;

        /* Remove this position. 
        */
        (void) mergeBlockNextGet (sort_table, i, 1);
        gen_status->processed_new_ply_positions++;
      }
    }

  } while (1);


  /* Close the ply move file.
  */
  plyMoveFileFinish ();

  /* Close the ply position file.
  */
  plyPositionFileFinish ();


  chessStat_t *const ply_stats = &board_db->ply_table[ply_number].stats;

  end_of_task_msec = sysUpTimeMillisecondsGet();

  ply->stats.board_db_insert_time_msec = end_of_task_msec - 
                start_of_task_msec;

  /* Update Global Statistics.
  */
  board_db->stats.unique_positions_added += ply_stats->unique_positions_added;
  board_db->stats.duplicate_positions_detected += ply_stats->duplicate_positions_detected;
  board_db->stats.total_moves_added += ply_stats->total_moves_added;
  board_db->stats.board_db_insert_time_msec += ply_stats->board_db_insert_time_msec;

  /* If we are processing the last ply then add the last ply statistics 
  ** to the global statistics.
  */
  if ((ply_number + 1) == board_db->ply_depth)
  {
    chessStat_t *const last_ply_stats = &board_db->ply_table[ply_number + 1].stats;

    board_db->stats.unique_positions_added += last_ply_stats->unique_positions_added;
    board_db->stats.duplicate_positions_detected += last_ply_stats->duplicate_positions_detected;
    board_db->stats.total_moves_added += last_ply_stats->total_moves_added;
    board_db->stats.board_db_insert_time_msec += last_ply_stats->board_db_insert_time_msec;
  }

  return 0;
}

/******************************************************************************
** Generate the board database from the given position.
** The assumption is that the start position is legal.
** Note that the database may run out of resources before reaching the 
** requested number of plies. 
**
** board_db - Board Database.
** brd - Initial Position
** info - Initial Position Info.
** max_plies - Maximum plies to generate in the board dtabase.
** perft - Perft mode enabled.
**
** Return Values:
******************************************************************************/
void brdDbGenerate(
                   const unsigned int ply_depth,
                   const brd_t *const brd, 
                   const brdCtrlInfo_t *const info)
{
  brdDb_t board_db = {};
  const unsigned long long start_of_test = sysUpTimeMillisecondsGet();
  const color_e whose_move = (info->next_move)?MOVE_WHITE:MOVE_BLACK;


  /* Create board_db directory.
  */
  brdDbCreate();

  /* Add the initial position to the ply 0 positions database.
  */
  plyPositionEntry_t position_entry = {};
  memcpy (&position_entry.brd_info.brd_info, info, sizeof(brdCtrlInfo_t));
  brdPack (brd, position_entry.position);

  plyPositionFileWriteStart (0);
  plyPositionFileWrite (&position_entry);
  plyPositionFileFinish ();

  board_db.ply_depth = ply_depth;
  board_db.ply_table[0].whose_move = whose_move;
  board_db.ply_table[0].num_boards_in_ply = 1;
  board_db.ply_table[0].stats.unique_positions_added = 1;

  board_db.max_sortblock_positions = SORT_BLOCK_SIZE / sizeof(sortBlockEntry_t);
  board_db.sort_block = malloc (SORT_BLOCK_SIZE);
  board_db.sort_block_position = board_db.sort_block;
  assert (board_db.sort_block);

  /* Start the board generator thread for each ply.
  ** The code exits when there are no more moves to be 
  ** generated.
  */
  for (unsigned int i = 0; i < board_db.ply_depth; i++)
  {
    pthread_t ch_thread;
    void *ch_arg[3];
    brdGenThreadStatus_t gen_status = {};
    unsigned int ply_number;
    int rc;

    /* If there are no boards created in this ply then exit the loop.
    */
    if (0 == board_db.ply_table[i].num_boards_in_ply)
                            break;


    /* When procesing the specified ply number, the code will generate new positions
    ** in the current ply+1. We need to set up the next ply table before starting the tasks.
    ** Keep in mind that the ply table has ply_depth+1 entries.
    */
    board_db.ply_table[i+1].num_boards_in_ply = 0;
    board_db.ply_table[i+1].whose_move = 
          (board_db.ply_table[i].whose_move == MOVE_WHITE)?MOVE_BLACK:MOVE_WHITE;

    ply_number = i;

    ch_arg[0] = &board_db;
    ch_arg[1] = &ply_number;
    ch_arg[2] = &gen_status;
    rc = pthread_create (&ch_thread, 0, brd_db_generate, ch_arg);
    if (rc)
    {
      perror ("pthread_create: ");
      exit (-1);
    }

    /* Wait until the ply position generation thread is done.
    */
    unsigned long long prev_brd_processed = 0;
    unsigned long long prev_ply_positions = 0;
    unsigned int wait_time_sec = 0;
    unsigned int wait_message_interval_sec = 60;

    do
    {
      struct timespec ts;

      if (-1 == clock_gettime (CLOCK_REALTIME, &ts))
      {
        perror ("clock_gettime");
        exit (-1);
      }

      ts.tv_sec += wait_message_interval_sec;
      wait_time_sec += wait_message_interval_sec;

      rc = pthread_timedjoin_np (ch_thread, 0, &ts);
      if ((0 != rc) && (ETIMEDOUT != rc))
      {
        perror ("pthread_join: ");
        exit (-1);
      }
      if (rc == ETIMEDOUT)
      {
        if (gen_status.ply_processing_phase == 1)
        {
          printf ("Inserting... %'u seconds - Ply:%u Entry:%'llu/%'llu  (%'llu Entries/s) - Block:%u\n",
                    wait_time_sec, 
                    i,
                    gen_status.positions_processed, 
                    board_db.ply_table[i].num_boards_in_ply,
                     ((gen_status.positions_processed - prev_brd_processed) / 
                                                        wait_message_interval_sec),
                    gen_status.sort_blocks_created);
                    
        } else if (gen_status.ply_processing_phase == 2)
        {
          printf ("Sorting... %'u seconds - Ply:%u - Block:%u\n",
                    wait_time_sec, 
                    i, gen_status.sort_blocks_created);
        } else if (gen_status.ply_processing_phase == 3)
        {
          printf ("Eliminating Duplicates... %'u seconds - Ply:%u - Processed:%'llu/%'llu (%'llu Pos/Sec - %u%%)\n",
                    wait_time_sec, 
                    i, gen_status.processed_new_ply_positions,
                    gen_status.total_new_ply_positions,
                    ((gen_status.processed_new_ply_positions - prev_ply_positions) / 
                                                        wait_message_interval_sec),
                    (unsigned int) (gen_status.processed_new_ply_positions / (gen_status.total_new_ply_positions / 100))
                    );
        }
        prev_brd_processed = gen_status.positions_processed;
        prev_ply_positions = gen_status.processed_new_ply_positions;
      }
    } while (rc == ETIMEDOUT);

    mcperftPlyInfoPrint(&board_db, ply_number); 

  }
  mcperftPlyInfoPrint(&board_db, board_db.ply_depth); 

  board_db.position_db_run_time = sysUpTimeMillisecondsGet() - start_of_test;

  mcperftBoardInfoPrint(&board_db);

  free (board_db.sort_block);

  printf ("SUCCESS! Created the position database with depth of %u plies.\n", ply_depth);
}


/* This variable keeps track of which workload is being processed.
** The varible is set to 0 prior to starting the search threads and
** is incremented atomically by each thread.
*/
volatile unsigned long long workload_index;

/******************************************************************************
** Search every leaf position.
**
** Return Values:
******************************************************************************/
static void * brd_db_deep_search (void *arg)
{
  void **ch_arg = (void **) arg;
  const unsigned int search_depth = *(unsigned int *) ch_arg[0]; 
  const unsigned long long num_entries = *(unsigned long long *) ch_arg[1];
  const workloadRecord_t *const work_load_buffer = ch_arg[2];
  unsigned long long *const search_result_buffer = ch_arg[3];
  const unsigned int task_number = *(unsigned int *) ch_arg[4]; 
  unsigned long long *num_boards_processed = ch_arg[5];
  unsigned _BitInt(128) *total_positions = ch_arg[6];

#if 1
  /* Pin this thread to one core.
  */
  {
    cpu_set_t cpu_set;
    const unsigned int num_cores = (unsigned int) sysconf(_SC_NPROCESSORS_ONLN);
    const unsigned int my_core_number = task_number % num_cores;

    CPU_ZERO (&cpu_set);
    CPU_SET (my_core_number, &cpu_set);
    (void) pthread_setaffinity_np(pthread_self(),sizeof(cpu_set),&cpu_set);

  }
#else
  (void) task_number;
#endif

  do 
  {
    brd_t brd; 
    brdCtrlInfo_t brd_info;

    const unsigned long long next_board = 
           __atomic_fetch_add (&workload_index, 1, __ATOMIC_RELAXED);
                        

    /* Iterate through all boards.
    */
    if (next_board >= num_entries)
                                 break;

    const workloadRecord_t *const work_load = &work_load_buffer [next_board];

    brdUnpack (&brd, work_load->position);

    brd_info = work_load->brd_info.brd_info;


#if 0 // HACK
    const unsigned long long positions_at_depth =
            bytebrdPerft(search_depth, &brd, &brd_info, 1);
#else
    const unsigned long long positions_at_depth =
            onecorePerft(search_depth, &brd, &brd_info, 1);
#endif

    search_result_buffer[next_board] = positions_at_depth;

    /* We don't need atomic operations here because these counters are only used for 
    ** debugging, so its OK if the counter is a little bit inaccurate.
    */
    *total_positions += positions_at_depth;
    *num_boards_processed += 1;

  } while (1);

  return 0;
}


/******************************************************************************
** For each leaf position, perform a serch down to the specified depth.
**
** Return Values:
******************************************************************************/
static void  brdDbDeepSearch (const unsigned int num_cores, 
                              const unsigned int depth, 
                              const unsigned long long total_workloads,
                              const int workload_fd, 
                              const int result_fd,
                              workloadRecord_t *const work_load_buffer, 
                              unsigned long long *const search_result_buffer,
                              const unsigned long long resolved_workloads,
                              const unsigned long long chunk_size)
{
  constexpr unsigned long long wait_message_interval_sec = 60;

  unsigned long long wait_time_sec = 0;
  unsigned long long last_event_time_sec = sysUpTimeMillisecondsGet() / 1000;
  const unsigned long long test_start_time_sec = last_event_time_sec;
  unsigned _BitInt(128) prev_total_positions = 0;
  unsigned long long prev_num_boards = 0;
  unsigned long long num_boards_processed[num_cores] = {};
  unsigned _BitInt(128) total_positions_per_thread[num_cores] = {};
  unsigned long long num_writes_to_file = 0;

  do 
  {
    const long long num_bytes = read (workload_fd, work_load_buffer, chunk_size);
    if (num_bytes == 0)
    {
      // Detected End of File.
      break;
    }
    if (num_bytes < 0)
    {
      perror ("read workload");
      exit (-1);
    }
    /* If the number of read bytes is not an exact multiple of workload record
    ** size then we have a problem.
    */
    if (0 != ((unsigned long long) num_bytes % sizeof(workloadRecord_t)))
    {
      printf ("ERROR: Workload Read detected corruped file.\n");
      exit (-1);
    }
    unsigned long long num_entries = (unsigned long long) num_bytes / sizeof(workloadRecord_t);
                    
    /* Start search threads.
    */
    pthread_t ch_thread[num_cores];
    void *ch_arg[num_cores][7];
    unsigned int task_number[num_cores];
    int rc;
    unsigned int search_depth = depth;

    /* Set global variable that control which workload is being processed.
    */
    workload_index = 0;

    for (unsigned int i = 0; i < num_cores; i++)
    {
      task_number[i] = i;

      ch_arg[i][0] = &search_depth;
      ch_arg[i][1] = &num_entries;
      ch_arg[i][2] = work_load_buffer;
      ch_arg[i][3] = search_result_buffer;
      ch_arg[i][4] = &task_number[i];
      ch_arg[i][5] = &num_boards_processed[i];
      ch_arg[i][6] = &total_positions_per_thread[i];
      rc = pthread_create (&ch_thread[i], 0, brd_db_deep_search, ch_arg[i]);
      if (rc)
      {
        perror ("pthread_create: ");
        exit (-1);
      }
    } 

    /* Wait until all the board generation threads are done.
    */

    for (unsigned int j = 0; j < num_cores; j++)
    {
      do 
      {
        struct timespec ts;
        unsigned long long current_time_sec = sysUpTimeMillisecondsGet() / 1000;

        if (-1 == clock_gettime (CLOCK_REALTIME, &ts))
        {
          perror ("clock_gettime");
          exit (-1);
        }
        if ((current_time_sec - last_event_time_sec) < wait_message_interval_sec) 
        {
          ts.tv_sec += (__time_t) (wait_message_interval_sec - (current_time_sec - last_event_time_sec));
        }

        rc = pthread_timedjoin_np (ch_thread[j], 0, &ts);
        if ((0 != rc) && (ETIMEDOUT != rc))
        {
          perror ("pthread_join deep search thread ");
          exit (-1);
        }

        current_time_sec = sysUpTimeMillisecondsGet() / 1000;
        const unsigned long long actual_wait_time = current_time_sec - last_event_time_sec;

        if (actual_wait_time >= wait_message_interval_sec)
        {
          unsigned long long brd_processed = 0;
          unsigned _BitInt(128) total_positions = 0;

          for (unsigned int k = 0; k < num_cores; k++)
          {
            brd_processed += num_boards_processed[k];
            total_positions += total_positions_per_thread[k];
          }
          char buf[256];

          wait_time_sec += actual_wait_time;

          printf ("Searching... %'llu sec - Searched %'llu/%'llu (%llu%%, %'u Wl/s) - Positions:%s (%u BP/s) - Writes:%'llu\n",
                    wait_time_sec, brd_processed + resolved_workloads, 
                    total_workloads,
                    ((total_workloads / 100) > 0)?(brd_processed + resolved_workloads) / (total_workloads / 100):0,
                    (unsigned int) ((brd_processed - prev_num_boards) / actual_wait_time),
                    int128ToStr(total_positions, buf, sizeof(buf)),
                    (unsigned int) (((total_positions - prev_total_positions) / 
                                                        actual_wait_time) / 1000000000),
                    num_writes_to_file
                    );
          prev_total_positions = total_positions;
          prev_num_boards = brd_processed;
          last_event_time_sec = current_time_sec;
        }
      } while (rc == ETIMEDOUT);
    }

    num_writes_to_file++;

    /* Write the current chunk into the results file.
    */
    const unsigned long long write_bytes = sizeof(unsigned long long) * num_entries;

    if (write_bytes != (unsigned long long) write (result_fd, search_result_buffer, write_bytes))
    {
      perror ("write to result file");
      exit (-1);
    }

  } while (1);

  const unsigned long long test_run_time_sec = (sysUpTimeMillisecondsGet() / 1000) - test_start_time_sec;
  const unsigned long long num_days = ((test_run_time_sec / 60) / 60) / 24;

  unsigned long long remaining_sec = test_run_time_sec - (num_days * 24 * 60 * 60);

  const unsigned long long num_hours = (remaining_sec / 60) / 60;

  remaining_sec = remaining_sec - (num_hours * 60 * 60);

  const unsigned long long num_minutes = remaining_sec / 60;

  remaining_sec = remaining_sec - (num_minutes * 60);

  printf ("Search Complete in %'llu seconds (%'llud:%lluh:%llum:%llus)\n",
         test_run_time_sec, 
         num_days, num_hours, num_minutes, remaining_sec);
}

/********************************************************************
** Perform deep search on the specified workload.
**
** workload_file -  Work load file name.
**
** Return Codes
**  Position Status
**
********************************************************************/
void brdDbCount (const char *workload_file)
{
  char workload_file_name[1024];

  sprintf (workload_file_name, "%s/%s", DEEP_SEARCH_WORKLOAD_DIR, 
                workload_file);

  const int workload_fd = open (workload_file_name, O_RDONLY);
  if (workload_fd < 0)
  {
    perror ("open - workload file");
    exit (-1);
  }

  workloadHeader_t workload_header;
  if (sizeof(workloadHeader_t) != read (workload_fd, &workload_header, sizeof(workloadHeader_t)))
  {
    perror ("read - workload file");
    exit (-1);
  }

  workloadHeader_t search_header;

  /* If results file already exists then let the user know this. Don't proceed
  ** until the user deletes the existing results file.
  */
  char results_file_name[1024];
  sprintf (results_file_name, "%s%u", DEEP_SEARCH_RESULT_FILE_PREFIX,
                        workload_header.workload_factor);
  int result_fd = open (results_file_name, O_RDONLY);
  if (result_fd >= 0)
  {
    printf ("ERROR: Search result file %s already exists.\n", results_file_name);
    /* Results file already exist. Check if it mtches the workload file.
    ** In either case, notify the user and stop the code.
    */
    if (sizeof(workloadHeader_t) != read (result_fd, &search_header, sizeof(workloadHeader_t)))
    {
      perror ("read existing results file header");
      exit (-1);
    }
    search_header.search_result = 0;
    if (0 == memcmp(&search_header, &workload_header, sizeof(workloadHeader_t)))
    {
      printf ("       The search result file seems to match the %s workload file.\n", 
                                                    workload_file_name);
    } else
    {
      printf ("       The search result file does NOT match the %s workload file.\n", 
                                                    workload_file_name);
    }
    printf ("       Please erase the %s file and try again.\n", results_file_name);
    close (result_fd);
    exit (-1);
  }

  unsigned long long resolved_workloads = 0;
  char temp_file_name[1024];
  sprintf (temp_file_name, "%s/%s", DEEP_SEARCH_RESULT_DIR,
                        DEEP_SEARCH_RESULT_TEMP_FILE);
  result_fd = open (temp_file_name, O_RDONLY);
  if (result_fd < 0)
  {
    if (errno != ENOENT)
    {
      printf ("open temporary results file");
      exit (-1);
    }

    /* Temporary search file doesn't exist, so create it
    */
    memcpy (&search_header, &workload_header, sizeof (workloadHeader_t));
    search_header.search_result = 1;

    result_fd = open (temp_file_name, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
    if (result_fd < 0)
    {
      perror ("create result file");
      exit (-1);
    }

    if (sizeof(workloadHeader_t) != write (result_fd, &search_header, sizeof(workloadHeader_t)))
    {
      perror ("write header into result file");
      exit (-1);
    }
  } else
  {
    /* Temporary results file already exists. This means that 
    ** the previous move search was interrupted.
    ** Try to resume searching where we left off.
    */
    if (sizeof(workloadHeader_t) != read (result_fd, &search_header, sizeof(workloadHeader_t)))
    {
      perror ("read existing results file header");
      exit (-1);
    }

    /* Verify that the result file is for the same search. If not, then exit with an error.
    */
    search_header.search_result = 0;
    if (0 != memcmp (&search_header, &workload_header, sizeof(workloadHeader_t)))
    {
      printf ("ERROR: Found existing search results file %s, but it doesn't match workload.\n",
                        temp_file_name);
      printf ("       Please erase the temporary search results file.\n");
      exit (-1);
    }
    
    /* Reopen the search results file in write-only mode.
    */
    (void) close (result_fd);
    result_fd = open (temp_file_name, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
    if (result_fd < 0)
    {
      perror ("open result file");
      exit (-1);
    }

    struct stat statbuf;
    if (0 > fstat (result_fd, &statbuf))
    {
      perror ("stat result file");
      exit (-1);
    }

    const unsigned long long num_entries = 
                    ((unsigned long long) statbuf.st_size - 
                                sizeof(workloadHeader_t)) / sizeof (unsigned long long);

    /* If the results file contains some entries then advance the workload file
    ** descriptor to skip the already computed entries.
    */
    if (num_entries)
    {
      if (0 > lseek (workload_fd, (__off_t) (num_entries * sizeof (workloadRecord_t)), SEEK_CUR))
      {
        perror ("sleek workload file");
        exit (-1);
      }

      resolved_workloads = num_entries;
    }
  }

  /* Determine how many cores are available. 
  ** We will use all cores to do the search.
  */
  const unsigned int num_cores = (unsigned int) sysconf(_SC_NPROCESSORS_ONLN);

  /* Allocate memory for reading workloads from NVRAM and for the 
  ** search results.
  */
  const unsigned long long max_chunk_size = num_cores * sizeof(workloadRecord_t) * MAX_DEEP_SEARCH_PER_CORE_CHUNK_SIZE;
  const unsigned long long min_chunk_size = num_cores * sizeof(workloadRecord_t) * 2; 
  
  unsigned long long chunk_size = max_chunk_size;

  if (search_header.workload_depth > 8)
  {
    chunk_size = min_chunk_size;
  } else if (search_header.workload_depth == 7)
  {
    chunk_size = num_cores * sizeof(workloadRecord_t) * 100LLU;
  } else if (search_header.workload_depth == 6)
  {
    chunk_size = num_cores * sizeof(workloadRecord_t) * 2'000LLU;
  } else if (search_header.workload_depth == 5)
  {
    chunk_size = num_cores * sizeof(workloadRecord_t) * 50'000LLU;
  } else if (search_header.workload_depth == 4)
  {
    chunk_size = num_cores * sizeof(workloadRecord_t) * 1'000'000LLU;
  }

  if (chunk_size > max_chunk_size)
  {
    chunk_size = max_chunk_size;
  }

  unsigned long long chunk_entries = chunk_size / sizeof(workloadRecord_t);

  workloadRecord_t *const work_load_buffer = malloc (chunk_size);
  unsigned long long *const search_result_buffer = 
                    malloc (sizeof(unsigned long long)  * chunk_entries);
  if (!work_load_buffer || !search_result_buffer)
  {
    perror ("malloc chunks");
    exit (-1);
  }


  /* Display some useful info.
  */
  printf ("\n");
  printf ("Work Load File:       %s\n", workload_file_name);
  printf ("Search Result File:   %s\n", results_file_name);
  printf ("Overall Search Depth: %u\n", workload_header.depth);
  printf ("Deep Search Depth:    %u\n", workload_header.workload_depth);
  printf ("Number of Work Loads: %'llu\n", workload_header.num_workloads);
  printf ("Resolved Work Loads:  %'llu\n", resolved_workloads);
  printf ("Deep Search Cores:    %u\n", num_cores); 
  printf ("Workloads Per Write:  %'llu\n",chunk_entries);
  printf ("\n");


  /* Optimize the single core position hash database size.
  ** Since we are starting 9 plies away from the standard staring position,
  ** the database size doesn't need to be as big as we would normally need for a 
  ** developed middle game position. Using less memory is better suited, 
  ** for 1GB memory footpring search machines. 
  */
  if (workload_header.workload_depth <= 6)
  {
    onecoreScalingOverride (3, 80'000, 160'000, 80'000); // 90BP/s at 6
  } else
  {
    onecoreScalingOverride (4, 1'600'000, 3'200'000, 1'600'000); // 144BP/s at 7
  }

  brdDbDeepSearch (num_cores, search_header.workload_depth, 
                    search_header.num_workloads,
                    workload_fd, result_fd,
                    work_load_buffer, search_result_buffer,
                    resolved_workloads,
                    chunk_size);

  free (work_load_buffer);
  free (search_result_buffer);
  (void) close (result_fd);
  (void) close (workload_fd);

  /* Double check that the search results file has the expected number of entries.
  */
  {
    struct stat statbuf;
    if (0 > stat (temp_file_name, &statbuf))
    {
      perror ("stat result file");
      exit (-1);
    }
    unsigned long long num_results = ((unsigned long long) statbuf.st_size - 
                                        sizeof(workloadHeader_t)) / sizeof (unsigned long long);

    if (num_results != workload_header.num_workloads)
    {
      printf ("ERROR: Search result file %s doesn't seem to match workload file %s\n",
                    temp_file_name, workload_file_name);
      exit (-1);
    }
  }

  /* Rename the temporary file to permanent results file.
  */
  if (0 != rename (temp_file_name, results_file_name))
  {
    perror ("rename temporary results file");
    exit (-1);
  }

}

/********************************************************************
** Get the number of positions in the specified ply.
********************************************************************/
static unsigned long long brdPlyNumPositionsGet (unsigned int ply)
{
  char buf[1024];
  sprintf (buf, "%s%u_positions", 
                    PLY_FILE_PREFIX,
                    ply);

  struct stat statbuf;

  if (0 != stat (buf, &statbuf))
  {
    perror ("fstat - position database");
    printf ("        File:%s\n", buf);
    exit (-1);
  }

  return (unsigned long long) statbuf.st_size / sizeof(plyPositionEntry_t);
}

/********************************************************************
** Create files containing fen positions for each ply in the database.
**
** Return Codes
**  None
**
********************************************************************/
void brdDbFenGenerate (void)
{
  const unsigned int ply_depth = brdDbPlyDepthGet();

  for (unsigned int ply = 0; ply <= ply_depth; ply++)
  {
    const unsigned long long num_boards_in_ply = brdPlyNumPositionsGet (ply);

    char buf[1024];
    sprintf (buf, "%s%u_positions", 
                    PLY_FILE_PREFIX,
                    ply);

    const int position_db_fd = open (buf, O_RDONLY);

    unsigned int write_buf_size = 10000;
    unsigned int num_bytes_in_buf = 0;
    char write_buf[write_buf_size];

    char fen_db_file_name[64];

    sprintf (fen_db_file_name, "%s%d", FEN_DB_FILE_PREFIX, ply);
    const int fen_fd = open (fen_db_file_name, O_WRONLY | O_APPEND | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fen_fd < 0)
    {
      perror ("open fen file");
      exit (-1);
    }

    for (unsigned int i = 0; i < num_boards_in_ply; i++)
    {
      plyPositionEntry_t db_entry;
      if (sizeof(plyPositionEntry_t) != read (position_db_fd, &db_entry, sizeof(plyPositionEntry_t)))
      {
        perror ("Error reading position database.");
        exit (-1);
      }

      brd_t brd;

      brdUnpack (&brd, db_entry.position);

      char fen[128];
      
      brdutilBrdToFenConvert (&brd, &db_entry.brd_info.brd_info, fen);

      const unsigned int size = (unsigned int) strlen (fen);

      if ((num_bytes_in_buf + size) >= write_buf_size)
      {
        if (num_bytes_in_buf != write (fen_fd, write_buf, num_bytes_in_buf))
        {
          perror ("write fen file");
          exit (-1);
        }
        num_bytes_in_buf = 0;
      }
      memcpy (&write_buf[num_bytes_in_buf], fen, size);
      num_bytes_in_buf += size;
    }
    if (num_bytes_in_buf)
    {
      if (num_bytes_in_buf != write (fen_fd, write_buf, num_bytes_in_buf))
      {
        perror ("write fen file");
        exit (-1);
      }
    }
    (void) close (fen_fd);
    (void) close (position_db_fd);
  }

  printf ("FEN files are ready!\n");
}

/******************************************************************************
** Generate Deep Search Workload Files.
**
** depth - Search Depth.
** split_factor - Number of workload files.
**
** Return Values:
******************************************************************************/
void brdDbCountSetup (const unsigned int depth,
                      const unsigned int split_factor)
                      
{
  char buf[1024];
  int rc;
  const unsigned int ply_depth = brdDbPlyDepthGet();

  sprintf (buf, "%s%u_positions", 
                    PLY_FILE_PREFIX,
                    ply_depth);

  const int position_db_fd = open (buf, O_RDONLY);
  if (position_db_fd < 0)
  {
    perror ("open - position database");
    printf ("Can't find file: %s\n", buf);
    exit (-1);
  }

  const unsigned long long num_boards_in_ply = brdPlyNumPositionsGet (ply_depth);

  printf ("Found existing board database with ply depth %u.\n", ply_depth);

  printf ("Erasing existing workload and move count result directories if present...\n");

  sprintf (buf, "rm -rf %s", DEEP_SEARCH_WORKLOAD_DIR);
  printf ("%s\n", buf);
  rc = system (buf);
  if (rc < 0)
  {
    perror ("erasing workload directory");
    exit (-1);
  }

  sprintf (buf, "rm -rf %s", DEEP_SEARCH_RESULT_DIR);
  printf ("%s\n", buf);
  rc = system (buf);
  if (rc < 0)
  {
    perror ("erasing results directory");
    exit (-1);
  }

  rc = mkdir (DEEP_SEARCH_WORKLOAD_DIR, 0777);
  if (0 != rc)
  {
    perror ("mkdir - workload directory");
    exit (-1);
  }

  rc = mkdir (DEEP_SEARCH_RESULT_DIR, 0777);
  if (0 != rc)
  {
    perror ("mkdir - workload directory");
    exit (-1);
  }


  if ((depth <= ply_depth) && (split_factor > 1))
  {
    printf ("ERROR: Split Factor must be 1 for depth less or equal to %u\n",
                    ply_depth);
    exit (-1);
  }



  printf ("Writing workload files...\n");
  unsigned long long start_workload_number = 0;

  for (unsigned int i = 0; i < split_factor; i++)
  {
    unsigned long long num_workloads;

    if (depth <= ply_depth)
    {
      /* When perft depth is smaller than position database depth then there is no 
      ** work to do for search machines. 
      */
      num_workloads = 0;
    } else
    {
      num_workloads = num_boards_in_ply / split_factor;
      if (i < (num_boards_in_ply % split_factor))
      {
        num_workloads += 1;
      }
    }

    workloadHeader_t workload_header = {
     .search_result = 0,
     .depth = depth,
     .workload_depth = (depth > ply_depth)?depth - ply_depth:0,
     .position_db_depth = ply_depth,
     .split_factor = split_factor,
     .workload_factor = i + 1,
     .num_workloads = num_workloads,
     .start_workload_number = (num_workloads)?start_workload_number:0,
     .end_workload_number = (num_workloads)?start_workload_number + num_workloads - 1:0
     };

     start_workload_number += num_workloads;

#if 0 // HACK
     printf ("depth:%u workload_depth:%u split_factor:%u workload_factor:%u num_workloads:%'llu start:%'llu end:%'llu\n",
               workload_header.depth, workload_header.workload_depth, workload_header.split_factor,
               workload_header.workload_factor, workload_header.num_workloads,
               workload_header.start_workload_number, workload_header.end_workload_number);
#endif
     
     /* Create the workload file.
     */
     char file_name[1024];
     sprintf (file_name, "%s%u", DEEP_SEARCH_WORKLOAD_FILE_PREFIX, i + 1);

     const int fd = open (file_name, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
     if (fd < 0)
     {
       perror ("open workload file");
       exit (-1);
     }

     if (sizeof(workloadHeader_t) != write (fd, &workload_header, sizeof(workloadHeader_t)))
     {
       perror ("write workload file");
       exit (-1);
     }

     for (unsigned long long j = 0; j < workload_header.num_workloads; )
     {
       constexpr unsigned int block_size = 100;
       workloadRecord_t workload_record[block_size];
       const unsigned long long j_inc = ((j + block_size) < workload_header.num_workloads)?block_size:
                                        workload_header.num_workloads - j;

       plyPositionEntry_t db_entry[j_inc];
       if ((j_inc * sizeof(plyPositionEntry_t)) != 
            (unsigned long long) read (position_db_fd, db_entry, j_inc * sizeof(plyPositionEntry_t)))
       {
         perror ("Error reading position file.");
         exit (-1);
       }

       for (unsigned long long k = 0; k < j_inc; k++)
       {
         memcpy (workload_record[k].position, 
               db_entry[workload_header.start_workload_number + k].position, 32);
         workload_record[k].brd_info = db_entry[workload_header.start_workload_number + k].brd_info;
         workload_record[k].pad1 = 0;
         workload_record[k].pad2 = 0;
       }

       if ((j_inc * sizeof(workloadRecord_t)) != 
                 (unsigned long long) write (fd, &workload_record, j_inc * sizeof(workloadRecord_t)))
       {
         perror ("write workload file.");
         exit (-1);
       }

       j += j_inc;
     }
     (void) close (fd);
  }


  (void) close (position_db_fd);

  printf ("Workload files are ready!\n");
}

/********************************************************************
** Add all the deep search positions into the aggregation table
** for previous ply.
**
** Return Codes
**  None
**
********************************************************************/
static void brdDbLastPlyMovesCount (const unsigned int search_depth, 
                            unsigned _BitInt(128) *const position_count_space)
{
  const unsigned long long num_boards_in_ply = brdPlyNumPositionsGet (search_depth - 1);

#if 1 // HACK
  printf ("%s %d - search_depth:%u num_board_in_ply:%'llu\n",
                    __FUNCTION__, __LINE__,
                    search_depth, 
                    num_boards_in_ply);
#endif

  plyPositionFileReadOnlyStart (search_depth - 1);
  for (unsigned long long i = 0; i < num_boards_in_ply; i++)
  {
    plyPositionEntry_t *position = plyPositionFileRead ();
    position_count_space[i] = position->num_moves;
  }
  (void) plyPositionFileRead(); // Close the position database file.
}

/********************************************************************
** Add all the deep search positions into the aggregation table
** for previous ply.
**
** Return Codes
**  None
**
********************************************************************/
static void brdDbDeepSearchAggregate (
                               const unsigned int position_db_depth,
                               const unsigned long long *const deep_search_result,
                               unsigned _BitInt(128) *const position_count_space)
{
  const unsigned long long num_boards_in_ply = brdPlyNumPositionsGet (position_db_depth - 1);

  char move_file_name[1024];
  int  fd;

  sprintf (move_file_name, "%s%u_moves", PLY_FILE_PREFIX, position_db_depth - 1);
  fd = open (move_file_name, O_RDONLY);
  if (fd < 0)
  {
    perror ("open ply moves file");
    exit (-1);
  }

#if 1 // HACK
  printf ("%s %d - position_db_depth:%u num_board_in_ply:%'llu\n",
                    __FUNCTION__, __LINE__,
                    position_db_depth,
                    num_boards_in_ply);
#endif

  plyPositionFileReadOnlyStart (position_db_depth - 1);
  for (unsigned long long i = 0; i < num_boards_in_ply; i++)
  {
    position_count_space[i] = 0;
    plyPositionEntry_t *position = plyPositionFileRead ();
    for (unsigned int j = 0; j < position->num_moves; j++)
    {
      const unsigned long long move_index = moveEntryToIndex(position->first_move_index) + j;
      moveEntry_t move_entry;

      if (sizeof(moveEntry_t) != pread (fd, &move_entry, sizeof(moveEntry_t),
                                           (__off_t) (move_index * sizeof(moveEntry_t))))
      {
        perror ("can't pread() move entry file");
        exit (-1);
      }

      const unsigned long long next_node_index = moveEntryToIndex(move_entry);
                        
      position_count_space[i] += deep_search_result[next_node_index];
    }
  }
  (void) plyPositionFileRead(); // Close the position database file.
  close (fd); // Close Move File 
}

/********************************************************************
** Aggregate all the move counts in the position tree.
** When this function is invoked, the deepest ply in the position
** tree has already been counted, so the counting needs to start with
** deepest ply minus 1.
**
** Return Codes
**  None
**
********************************************************************/
static void brdDbPositionTreeAggregate (const unsigned int search_depth,
                               const unsigned int position_db_depth,
                               unsigned _BitInt(128) **const position_count_space)
{
  const unsigned int tree_search_depth = (search_depth > position_db_depth)?
                                                position_db_depth - 2:
                                                search_depth - 2;
                                                        
  for (int ply_number =  (int) tree_search_depth; ply_number >= 0; ply_number--)
  {
    const unsigned long long num_boards_in_ply = brdPlyNumPositionsGet ((unsigned int) ply_number);

    char move_file_name[1024];
    int  fd;

    sprintf (move_file_name, "%s%u_moves", PLY_FILE_PREFIX, ply_number);
    fd = open (move_file_name, O_RDONLY);
    if (fd < 0)
    {
      perror ("open ply moves file");
      exit (-1);
    }

    plyPositionFileReadOnlyStart ((unsigned int) ply_number);
#if 1 // HACK
    printf ("%s %d - search_depth:%u position_db_depth:%u ply_number:%d num_board_in_ply:%'llu\n",
                    __FUNCTION__, __LINE__,
                    search_depth, position_db_depth,
                    ply_number,
                    num_boards_in_ply);
#endif
    for (unsigned long long i = 0; i < num_boards_in_ply; i++)
    {
      position_count_space[ply_number][i] = 0;
      plyPositionEntry_t *position = plyPositionFileRead ();
      unsigned int num_legal_moves = position->num_moves;

      for (unsigned int j = 0; j < num_legal_moves; j++)
      {
        const unsigned long long move_index = moveEntryToIndex(position->first_move_index) + j;
        moveEntry_t move_entry;

        if (sizeof(moveEntry_t) != pread (fd, &move_entry, sizeof(moveEntry_t),
                                           (__off_t) (move_index * sizeof(moveEntry_t))))
        {
          perror ("can't pread() move entry file");
          exit (-1);
        }

        const unsigned long long next_node_index = moveEntryToIndex(move_entry);

        position_count_space[ply_number][i] += 
                        position_count_space[ply_number + 1][next_node_index];
      }
    }

    (void) plyPositionFileRead(); // Close the position database file.
    close (fd);
  }
}

/********************************************************************
** Analyze all the result files and compute the final perft value.
**
** Return Codes
**  None
**
********************************************************************/
void brdDbAggregate (unsigned int *depth,
                     unsigned _BitInt(128) *perft_result,
                     unsigned _BitInt(128) *ply1_perft_result)
{
  DIR *dir;
  struct dirent *entry;

  dir = opendir (DEEP_SEARCH_RESULT_DIR);
  if (0 == dir)
  {
    perror ("Can't open results directory.");
    exit (-1);
  }

  const unsigned int max_db_plies = brdDbPlyDepthGet() + 1;

  unsigned _BitInt(128) *position_count_space[max_db_plies - 1];
  unsigned long long position_count_size[max_db_plies];
  const unsigned int position_db_depth = max_db_plies - 1;


  /* We need to allocate memory for position counters for every ply in 
  ** the position database, except the last ply. 
  ** For example if a position database has a depth of 8 then we need to allocate
  ** counter space for ply 0 to ply 7.
  */
  for (unsigned int i = 0; i < (max_db_plies - 1); i++)
  {
    const unsigned long long num_boards_in_ply = brdPlyNumPositionsGet (i);
    position_count_size[i] = num_boards_in_ply * sizeof(_BitInt(128));
    if (0 == position_count_size[i])
    {
      printf ("ERROR: Unexpected 0 positions in ply %u\n", i);
      exit (-1);
    }
    position_count_space[i] = malloc (position_count_size[i]);
#if 0 // HACK
    printf ("position_count_size[%u] = %'llu\n", i, position_count_size[i]);
#endif
  }


  /* Read in all result_n files and aggregate them into 
  ** an array of position counts. The indexes in this array correspond to the 
  ** positions in the last ply of the position database.
  ** Note that to count the moves we don't need to read the positions from the
  ** last ply of the position database, so we don't need 1TB of DRAM in order 
  ** to compute the perft count on position database of depth 9. The 128GB DRAM
  ** is sufficient because each perft result is only 8 bytes, and we need about 10
  ** billion positions, which is 80GB of DRAM. The 80GB is still a lot, so we will
  ** probably end up using swap space, but I am hoping that things will not be 
  ** too slow.
  */
  const unsigned long long position_count = brdPlyNumPositionsGet (position_db_depth);

  position_count_size[position_db_depth] = position_count * sizeof(unsigned long long);

  printf ("Position Database Contains %'llu positions in ply:%u\n",
                    position_count, position_db_depth);

  unsigned long long *const deep_search_result = mmap(0, position_count_size[position_db_depth],
          PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS,
                  -1,0);
  madvise (deep_search_result, position_count_size[position_db_depth], MADV_HUGEPAGE);

  printf ("Allocated memory region for results. Size:%'llu bytes.\n", 
                    position_count_size[position_db_depth]);

  unsigned long long num_found_results = 0;
  unsigned int search_depth = 0;

  while (0 != (entry = readdir(dir)))
  {
    if (0 == strncmp (entry->d_name, "result_", 7))
    {
      int fd;
      char file_name[1024];

      printf ("%s\n", entry->d_name);

      sprintf (file_name, "%s/%s", DEEP_SEARCH_RESULT_DIR, entry->d_name);
      fd = open (file_name, O_RDONLY);
      if (fd < 0)
      {
        perror ("open results file");
        exit (-1);
      }

      workloadHeader_t workload;

      if (sizeof(workloadHeader_t) != read (fd, &workload, sizeof(workloadHeader_t)))
      {
        perror ("read workload header");
        exit (-1);
      }

#if 1 // HACK
      printf ("search_result:         %u\n", workload.search_result);
      printf ("depth:                 %u\n", workload.depth);
      printf ("workload_depth:        %u\n", workload.workload_depth);
      printf ("position_db_depth:     %u\n", workload.position_db_depth);
      printf ("split_factor:          %'u\n", workload.split_factor);
      printf ("workload_factor:       %'u\n", workload.workload_factor);
      printf ("num_workloads:         %'llu\n", workload.num_workloads);
      printf ("start_workload_number: %'llu\n", workload.start_workload_number);
      printf ("end_workload_number:   %'llu\n", workload.end_workload_number);
#endif

      /* Verify that the result was constructed using a position table with the same 
      ** depth as the current table.
      */
      if (workload.position_db_depth != position_db_depth)
      {
        printf ("ERROR: The results file position database depth %u doesn't match detected depth %u\n",
                    workload.position_db_depth, position_db_depth);
        exit (-1);
      }
      search_depth = workload.depth;

      /* If we detected a special case where the search depth is not greater than the 
      ** position tree depth then we don't need to read the results file.
      ** All the information needed to compute perft is in the search database.
      */
      if (search_depth <= position_db_depth)
      {
        break;
      }

      unsigned long long read_request_size = workload.num_workloads * sizeof (unsigned long long);
      unsigned long long bytes_read = 0;
      while (1)
      {
        unsigned long long w_index = workload.start_workload_number  
                                             + (bytes_read / sizeof(unsigned long long));
                                             
 #if 1 // HACK
        printf ("bytes_read:%'llu read_request_size:%'llu Next Index:%'llu\n", 
                    bytes_read, read_request_size, w_index); 
 #endif

        ssize_t read_size = 
             read (fd, &deep_search_result[w_index], read_request_size);
                                       
        if (read_size < 0)
        {
          perror ("read workload data");
          exit (-1);
        }

        if (read_size & 0x7)
        {
          printf ("ERROR: read_size:%'zd is not divisible by 8.\n", read_size);
          exit (-1);
        }

        bytes_read += (unsigned long long) read_size;
        read_request_size -= (unsigned long long) read_size;

        if (0 == read_request_size)
        {
          break;
        }

        if (read_size == 0)
        {
          printf ("ERROR: Unexpected end of results file. Read %'llu bytes, expected additional %'llu bytes.\n",
                        bytes_read, read_request_size);
          exit (-1);
        }
        
      }
      num_found_results += workload.num_workloads;
    }
  }

  if (search_depth > position_db_depth)
  {
    printf ("Read %'llu results from all result files.\n", num_found_results);
    if ((num_found_results != position_count) && 
      (search_depth > position_db_depth))
    {
      printf ("ERROR: Expected %'llu results.\n", position_count);
      exit (-1);
    }

    /* Aggregate deep search results into ply-1 counter array.
    ** This is needed only when the search depth is greater than the 
    ** position database depth.
    */
    printf ("Aggregating deep search results...\n");
    brdDbDeepSearchAggregate (position_db_depth,
                              deep_search_result,
                              position_count_space[position_db_depth - 1]);
  } else
  {
    /* The search depth is the same or smaller than the position tree depth.
    ** In this case we count number of moves in the ply one smaller than the 
    ** lowest ply. 
    */
    printf ("Search depth %u is smaller than position database depth %u. Counting Last Ply Moves...\n",
                search_depth, position_db_depth);
    brdDbLastPlyMovesCount (search_depth, 
                            position_count_space[search_depth - 1]);
  }

  /* We don't need the deep search results anymore.
  */
  (void) munmap (deep_search_result, position_count_size[position_db_depth]);

  if (search_depth > 1)
  {
    brdDbPositionTreeAggregate (search_depth,
                               position_db_depth,
                               position_count_space);
  }

  *depth = search_depth;
  *perft_result = position_count_space[0][0];

  /* Report perft counts for ply 1. 
  */
  const unsigned long long num_boards_in_ply_1 = brdPlyNumPositionsGet (1);
  for (unsigned long long i = 0; i < num_boards_in_ply_1; i++)
  {
    ply1_perft_result[i] = position_count_space[1][i];
  }

  for (unsigned int i = 0; i < (max_db_plies - 1); i++)
  {
    free (position_count_space[i]); 
  }
  (void) munmap (deep_search_result, position_count_size[position_db_depth]);
  closedir (dir);
}

