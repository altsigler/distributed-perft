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
  printf ("Number of Hash Collisions:%'llu\n", stats->num_hash_collisions);
  printf ("\n");
  printf ("Position Database Full:%s\n", (stats->position_database_full)?"Yes":"No");
  printf ("Move Database Full:%s\n", (stats->move_database_full)?"Yes":"No");
  printf ("\n");

}

static void mcperftPlyInfoPrint(const brdDb_t *const board_db,
                        unsigned int ply_number)
{
  const chessStat_t *const stats = &board_db->ply_table[ply_number].stats;

  printf ("****************************************************\n");
  printf ("Depth:%u\n", ply_number + 1);
  statsPrint(stats);
}

/*******************************************************************
** Print global board database information.
*******************************************************************/
static void mcperftBoardInfoPrint(const brdDb_t *const board_db)
{
  const chessStat_t *const stats = &board_db->stats;
  char buf[256];

  if (0 == board_db->highest_ply_with_positions)
  {
    printf ("Board Database is Empty\n");
    return;
  }

  printf ("****************************************************\n");
  printf ("Aggregate Statistics.\n");
  printf ("Position Database Creation Time:%'llumsec (%'llusec)\n\n",
          board_db->position_db_run_time,
          board_db->position_db_run_time / 1000);
  printf ("Highest Ply With Positions:%u\n", board_db->highest_ply_with_positions);
  printf ("Number of positions in ply %u is:%'llu\n",
                    board_db->highest_ply_with_positions,
                    board_db->ply_table[board_db->highest_ply_with_positions].num_boards_in_ply);
  {
    const unsigned long long num_brds = board_db->ply_table[board_db->highest_ply_with_positions - 1].num_boards_in_ply;
    const unsigned long long proc_brds = board_db->ply_table[board_db->highest_ply_with_positions - 1].num_boards_processed;
    const unsigned int percent_brds = (unsigned int) ((num_brds == proc_brds)?100:
                            (num_brds < 1000)?(proc_brds * 100)/num_brds:
                            (proc_brds / (num_brds/100)));
    printf ("Number of positions processed in ply %u is:%'llu of %'llu (%u%%)\n",
                    board_db->highest_ply_with_positions - 1,
                    proc_brds, num_brds, percent_brds);

  }
  printf ("\n");
  printf ("Deep Search Run Time:%'llums (%'llusec)\n",
                        board_db->deep_search_run_time,
                        board_db->deep_search_run_time / 1000);
  printf ("Number of positions found during deep search:%s (%'llu)\n",
                        int128ToStr(board_db->deep_search_positions, buf, sizeof(buf)),
                        (unsigned long long) board_db->deep_search_positions);
  if (board_db->deep_search_run_time > 1000)
  {
    printf ("Deep Search Positions Per Second:%'llu\n",
       (unsigned long long) (board_db->deep_search_positions / (board_db->deep_search_run_time / 1000)));
  }

  printf ("\n");
  statsPrint(stats);
}

/*********************************************************************
** Display information about the board database.
**
*********************************************************************/
static void mcperftBoardDbInfoPrint(const brdDb_t *const board_db)
{               
  printf ("\n\nBoard Database Information\n");
  
  printf ("Total Table Size in Bytes:%'zu (%'zuGB)\n",
          board_db->db_size_in_bytes + board_db->move_size_in_bytes + board_db->hash_size_in_bytes,
      (board_db->db_size_in_bytes + board_db->move_size_in_bytes + board_db->hash_size_in_bytes)
        / ONE_GB);
  printf ("  Maximum Positions:%'llu", board_db->max_db_entries);
  printf (" - Size in Bytes:%'zu - Entry Size:%zu\n",
            board_db->db_size_in_bytes, sizeof (brdDbEntry_t));
  printf ("  Max Moves:%'zu - Move DB Size:%'zu\n",
          board_db->max_move_entries,
          board_db->move_size_in_bytes);
  printf ("  Position Hash Entries:%'zu - Hash Table Size in Bytes:%'zu",
          board_db->max_hash_entries,
          board_db->hash_size_in_bytes);
      
  printf ("\n\n"); 
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
** Create the new position database. 
** If the work directory containing the position database already exists then
** this function terminates the program with an error message. 
**
** max_positions - Maximum number of positions to analyze.
** chess_max_moves - Maximum number of move database entries.
**
** Return Values:
******************************************************************************/
static brdDb_t * brdDbCreate(const unsigned int ply_depth,
                            const unsigned long long max_positions, 
                            const unsigned long long chess_max_moves)
{
  int rc;
  int position_db_fd;

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


  /* We must create an empty file whose size exactly matches the board database.
  */
  position_db_fd = open (POSITION_DB_FILE, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (position_db_fd < 0)
  {
    perror ("open - position database");
    exit (-1);
  }

  unsigned long long position_db_size = sizeof(brdDb_t);

  /* Make sure that the db_entry database starts on a 64-byte boundary.
  */
  if (position_db_size % 64)
  {
    position_db_size += position_db_size % 64;
  } 
  const unsigned long long start_of_db_entry = position_db_size;

  position_db_size += max_positions * sizeof(brdDbEntry_t);

  /* Make sure that the move_entry database starts on 64 byte boundary.
  */
  if (position_db_size % 64)
  {
    position_db_size += position_db_size % 64;
  } 
  const unsigned long long start_of_move_entry = position_db_size;

  position_db_size += chess_max_moves * sizeof (unsigned long long);

  rc = ftruncate (position_db_fd, (__off_t) position_db_size);
  if (rc != 0)
  {
    perror ("truncate - position database");
    exit (-1);
  }

  unsigned char *const addr = mmap(0, position_db_size, 
          PROT_READ | PROT_WRITE,
                  MAP_SHARED,
                  position_db_fd,0);
  close (position_db_fd); // Note that the mapping is still valid after the fd is closed.

  if (addr == MAP_FAILED)
  {
    perror ("mmap - position database");
    exit (-1);
  }

  madvise (addr, position_db_size, MADV_HUGEPAGE);

  brdDb_t *board_db = (brdDb_t *) addr;

  board_db->position_database_size = position_db_size;
  board_db->start_of_db_entry = start_of_db_entry;
  board_db->start_of_move_entry = start_of_move_entry;

  board_db->ply_depth = ply_depth;
  board_db->db_state = BRD_DB_INCOMPLETE;
  board_db->max_db_plies = ply_depth + 1;
  board_db->max_db_entries = max_positions;
  board_db->max_hash_entries = max_positions; // Hash table could be increased to reduce collisions
  board_db->max_move_entries = chess_max_moves;
  board_db->hash_size_in_bytes = (board_db->max_hash_entries) * sizeof(unsigned long long);
  board_db->db_size_in_bytes = ((size_t) board_db->max_db_entries) * sizeof(brdDbEntry_t);
  board_db->move_size_in_bytes = ((size_t) board_db->max_move_entries) * sizeof(unsigned long long);

  (void) pthread_mutex_init (&board_db->brd_mutex, 0);

  void *const hash_addr = mmap(0, board_db->hash_size_in_bytes, 
          PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS,
                  -1,0);
  madvise (hash_addr, board_db->hash_size_in_bytes, MADV_HUGEPAGE);
  board_db->db_hash = (unsigned long long *) hash_addr;

  board_db->db_entry = (brdDbEntry_t *) (addr + start_of_db_entry);
  board_db->db_move = (unsigned long long *) (addr + start_of_move_entry);

  mcperftBoardDbInfoPrint(board_db);

#if 0 // HACK
  printf ("sizeof(brdDb_t):%lu\n", sizeof(brdDb_t));
  printf ("board_db:          %p\n", board_db);
  printf ("board_db->db_hash: %p\n", board_db->db_hash);
  printf ("board_db->db_entry:%p\n", board_db->db_entry);
  printf ("board_db->db_move: %p\n", board_db->db_move);
  exit (0);
#endif

  return board_db;
}

/******************************************************************************
** Compute the hash index for the board entry.
**
** db_entry - Entry in the board database.
**
** Return Values:
** 32-bit hash incdex.
******************************************************************************/
static unsigned long long hashCompute (const unsigned long long max_hash_entries,
                                        const brdDbEntry_t* const db_entry) 
{
  if (max_hash_entries > 0xFFFF'FFFFLLU)
  {
#define FNV_64_PRIME 0x100000001B3ULL
#define FNV_64_OFFSET_BASIS 0xcbf29ce484222325LLU

    unsigned char *start = (unsigned char *) db_entry;
    unsigned long long hash_index = FNV_64_OFFSET_BASIS;

    for (int i = 0; i < 36; i++)
    {
      hash_index ^= (unsigned int)start[i];
      hash_index *= FNV_64_PRIME;
    }

    return  hash_index % max_hash_entries;
  } else
  {
    unsigned int hash_index = 0x811c9dc5;

    hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, db_entry->position[0]);
    hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, db_entry->position[1]);
    hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, db_entry->position[2]);
    hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, db_entry->position[3]);
    hash_index = __builtin_ia32_crc32hi(hash_index, db_entry->brd_info.brd_info_mem);
    hash_index = __builtin_ia32_crc32hi(hash_index, db_entry->ply_number);

    return ((unsigned long long) hash_index) % max_hash_entries;
  }
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
** Add discovered positions to the board database.
**
**
** Return Values:
** 0 - Positions added.
** -1 - Out of memory.
******************************************************************************/
static int positionsAdd (brdDb_t *const board_db,
                         const unsigned int num_moves,
                         const brdDbEntry_t *const next_db_entry,
                         const unsigned int ply_number,
                         const unsigned long long *const hash_index_list,
                         chessStat_t *const local_ply_stats,
                         size_t *const legal_move_index)
{
  /* Out of room in the move database.
  */
  if ((board_db->next_free_move_index + num_moves) > board_db->max_move_entries)
  {
    local_ply_stats->move_database_full = 1;
    return -1;
  }

  *legal_move_index = board_db->next_free_move_index;

  for (unsigned int i = 0; i < num_moves; i++)
  {
    const unsigned long long hash_index = hash_index_list[i];
    unsigned long long ex_entry_index = board_db->db_hash[hash_index];
    unsigned int dup_detected = (ex_entry_index)?1:0;

    while (ex_entry_index &&
        ((0 != memcmp(board_db->db_entry[ex_entry_index].position, 
         next_db_entry[i].position, 32)) ||
         (next_db_entry[i].brd_info.brd_info_mem != 
          board_db->db_entry[ex_entry_index].brd_info.brd_info_mem) ||
         (next_db_entry[i].ply_number != board_db->db_entry[ex_entry_index].ply_number)))
    {
        local_ply_stats->num_hash_collisions++;
        ex_entry_index = board_db->db_entry[ex_entry_index].next_brd_in_cache;
    }
    if (0 == ex_entry_index)
    {
      dup_detected = 0;
    }

    if (dup_detected)
    {
      board_db->db_move[board_db->next_free_move_index++] = ex_entry_index;
      local_ply_stats->duplicate_positions_detected++;
      local_ply_stats->total_moves_added++;
    } else
    {
      /* If the position database doesn't have any more room then return an error.
      */
      if (board_db->next_free_index >= board_db->max_db_entries)
      {
        local_ply_stats->position_database_full = 1;
        return -1;
      }

      /* Add the new entry to the hash.
      */
      unsigned long long next_brd_in_cache = board_db->db_hash[hash_index];
      board_db->db_hash[hash_index] = board_db->next_free_index;

      /* Point the move database to the new board entry.
      */
      board_db->db_move[board_db->next_free_move_index++] = board_db->next_free_index;

      /* Store the position in the database.
      */
      memcpy (&board_db->db_entry[board_db->next_free_index],
                &next_db_entry[i], sizeof(brdDbEntry_t));

      board_db->db_entry[board_db->next_free_index].next_brd_in_cache = next_brd_in_cache;

      board_db->next_free_index++,
      board_db->ply_table[ply_number+1].num_boards_in_ply++;

      local_ply_stats->total_moves_added++;
      local_ply_stats->unique_positions_added++;
    }
  }


  return 0;
}

/******************************************************************************
** Create all positions that can be reached from the current position.
**
**
** Return Values:
******************************************************************************/
static void nextPositionsCreate (const brdDb_t *const board_db,
                                 const brd_t *const brd,
                                 const bytebrdMove_t *const dest,
                                 const unsigned int num_moves,
                                 const brdDbEntry_t *const db_entry,
                                 brdDbEntry_t *const next_db_entry,
                                 unsigned long long *const hash_index_list,
                                 const unsigned int ply_number)
{
  /* Create next database entry for every move.
  */
  for (unsigned int i = 0; i < num_moves; i++)
  {
    brd_t next_brd = *brd;
    brdDbEntry_t *const ndb_entry = &next_db_entry[i];

    ndb_entry->brd_info.brd_info = db_entry->brd_info.brd_info;

    ndb_entry->next_brd_in_cache = 0;
    ndb_entry->legal_move_index = 0;
    memset (&ndb_entry->status, 0, sizeof(brdStatusInfo_t));
    ndb_entry->ply_number = (unsigned short) ply_number;

    bytebrdUtilMoveMake (&next_brd, &dest[i], 
                            &db_entry->brd_info.brd_info, &ndb_entry->brd_info.brd_info);

    brdPack (&next_brd, ndb_entry->position);
    hash_index_list[i] = hashCompute (board_db->max_hash_entries, ndb_entry);
  }
}

/******************************************************************************
** Generate the board database from the given position.
**
** board_db - Board Database.
** brd - Initial Position
** info - Initial Position Info.
**
** Return Values:
******************************************************************************/
static void * brd_db_generate (void *arg)
{
  void **ch_arg = (void **) arg;
  brdDb_t *const board_db = ch_arg[0];
  const unsigned int ply_number = *(unsigned int *) ch_arg[1]; 
  unsigned long long *positions_created = (unsigned long long *) ch_arg[2];
  const unsigned long long entry_count = board_db->ply_table[ply_number].num_boards_in_ply;
  brd_t brd; 

  bytebrdMove_t dest[MAX_BRD_MOVES];

  unsigned long long i;
  brdDbEntry_t *db_entry;
  brdDbEntry_t next_db_entry[MAX_BRD_MOVES];
  unsigned long long hash_index_list[MAX_BRD_MOVES];
  unsigned int num_moves;
  unsigned int mover_lost;

  chessStat_t local_ply_stats;

  const unsigned long long start_of_task_msec = sysUpTimeMillisecondsGet();
  unsigned long long end_of_task_msec;



  memset (&local_ply_stats, 0, sizeof(local_ply_stats));

  for (i = 0; i < entry_count; i++)
  {
    /* Note that we don't need to lock the board database mutex while 
    ** accesing the data pointed to by the *db_entry because that data 
    ** is not accessed by other threads while this thread is running.
    */
    db_entry = &board_db->db_entry[board_db->ply_table[ply_number].first_board_in_ply_index + i];

    brdUnpack (&brd, db_entry->position);

    /* Find all legal moves for this position.
    */
    num_moves = bytebrdNextMoveGet (&brd, &db_entry->brd_info.brd_info, dest, &mover_lost);

    /* If there are no moves available for this position then the mover is 
    ** either in a checkmate or stalemate. 
    */
    if (0 == num_moves)
    {
      board_db->ply_table[ply_number].num_boards_processed++;
      *positions_created += 1;
      continue;
    }

    nextPositionsCreate (board_db, &brd, dest, num_moves, db_entry,
                            next_db_entry, hash_index_list, ply_number);

    if (0 !=  positionsAdd (board_db, num_moves, next_db_entry, ply_number,
                           hash_index_list, &local_ply_stats, &db_entry->legal_move_index))
    {
      /* We ran out of memory in the board database, so exit the 
      ** position insertion thread.
      */
      break;
    }

    *positions_created += 1;

    db_entry->status.num_legal_moves =  num_moves & 0x1ff;
    board_db->ply_table[ply_number].num_boards_processed++;
  }

  chessStat_t *const ply_stats = &board_db->ply_table[ply_number].stats;

  end_of_task_msec = sysUpTimeMillisecondsGet();

  local_ply_stats.board_db_insert_time_msec = end_of_task_msec - 
                start_of_task_msec;

  /* Update Ply Statistics.
  */
  for (int k = 0; k < (int) (sizeof(chessStat_t) / sizeof(unsigned long long)); k++)
  {
    ((unsigned long long *) ply_stats)[k] += 
        ((unsigned long long *) &local_ply_stats)[k];
    ((unsigned long long *) &board_db->stats)[k] += 
        ((unsigned long long *) &local_ply_stats)[k];
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
                   const unsigned long long max_positions,
                   const unsigned long long max_moves,
                   const brd_t *const brd, 
                   const brdCtrlInfo_t *const info)
{
  brdDb_t *board_db;
  brdDbEntry_t db_entry;
  unsigned long long entry_index;
  const unsigned long long start_of_test = sysUpTimeMillisecondsGet();
  unsigned long long hash_index;
  const color_e whose_move = (info->next_move)?MOVE_WHITE:MOVE_BLACK;


  board_db = brdDbCreate(ply_depth, max_positions, max_moves);

  /* If the position database is already created then we have nothing to do.
  */
  if (board_db->db_state != BRD_DB_INCOMPLETE)
  {
    (void) munmap (board_db->db_hash, board_db->hash_size_in_bytes);
    (void) munmap (board_db, board_db->position_database_size);
    return;
  }

  /* Create the board database entry from the initial position.
  */
  memset (&db_entry, 0, sizeof(db_entry));
  memcpy (&db_entry.brd_info.brd_info, info, sizeof(brdCtrlInfo_t));
  brdPack (brd, db_entry.position);
  db_entry.ply_number = 0;
  hash_index = hashCompute (board_db->max_hash_entries, &db_entry);

  /* Add the board entry to the board database.
  */
  entry_index = board_db->next_free_index++;
  memcpy (&board_db->db_entry[entry_index], 
          &db_entry, sizeof(brdDbEntry_t));
  board_db->db_hash[hash_index] = entry_index;

  memset (&board_db->ply_table[0], 0, sizeof (plyInfo_t));
  board_db->ply_table[0].first_board_in_ply_index = entry_index;
  board_db->ply_table[0].num_boards_in_ply = 1;
  board_db->ply_table[0].whose_move = whose_move;

  /* Start the board generator thread for each ply.
  ** The code exits when we run out of memory or when there are no more moves to be 
  ** generated.
  */
  for (unsigned int i = 0; i < (board_db->max_db_plies - 1); i++)
  {
    pthread_t ch_thread;
    void *ch_arg[3];
    unsigned long long positions_created = 0;
    unsigned int ply_number;
    int rc;

    /* If there are no boards created in this ply then exit the loop.
    */
    if (0 == board_db->ply_table[i].num_boards_in_ply)
                            break;


    /* When procesing the specified ply number, the code will generate new positions
    ** in the current ply+1. We need to set up the next ply table before starting the tasks.
    ** Keep in mind that the ply table has max_db_plies+1 entries.
    */
    if (i < board_db->max_db_plies)
    {
      board_db->ply_table[i+1].first_board_in_ply_index = board_db->next_free_index;
      board_db->ply_table[i+1].num_boards_in_ply = 0;
      board_db->ply_table[i+1].whose_move = 
          (board_db->ply_table[i].whose_move == MOVE_WHITE)?MOVE_BLACK:MOVE_WHITE;
    }

    ply_number = i;

    ch_arg[0] = board_db;
    ch_arg[1] = &ply_number;
    ch_arg[2] = &positions_created;
    rc = pthread_create (&ch_thread, 0, brd_db_generate, ch_arg);
    if (rc)
    {
      perror ("pthread_create: ");
      exit (-1);
    }

    /* Wait until the ply position generation thread is done.
    */
    unsigned long long prev_brd_processed = 0;
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
        printf ("Inserting... %'u seconds - Ply:%u Entry:%'llu/%'llu  (%'llu Entries/s)\n",
                    wait_time_sec, 
                    i,
                    positions_created, 
                    board_db->ply_table[i].num_boards_in_ply,
                     (((positions_created - prev_brd_processed) / 
                                                        wait_message_interval_sec))
                    );
        prev_brd_processed = positions_created;
      }
    } while (rc == ETIMEDOUT);

#if 1 // HACK
    mcperftPlyInfoPrint(board_db, ply_number); 
#endif

    if (0 != board_db->ply_table[i+1].num_boards_in_ply)
                board_db->highest_ply_with_positions = i + 1;
  }
  board_db->position_db_run_time = sysUpTimeMillisecondsGet() - start_of_test;

  /* Mark the database as created. 
  */
  board_db->db_state = BRD_DB_POSITIONS_CREATED;

  /* We no longer need the hash table. 
  ** Free the memory.
  */
  (void) munmap (board_db->db_hash, board_db->hash_size_in_bytes);
  board_db->db_hash = 0;

  /* Push the datbase to non-volatile storage.
  */
  {
    int rc;

    printf ("\n");
    printf ("Writing position database to disk...\n");

    rc = msync (board_db, board_db->position_database_size, MS_SYNC);
    if (rc < 0)
    {
      perror ("msync on position database");
      exit (-1);
    }

    printf ("Database Write Done.\n\n");
  }

  mcperftBoardInfoPrint(board_db);

  const unsigned int db_error = 
   ((board_db->highest_ply_with_positions != ply_depth) ||
      (board_db->ply_table[board_db->highest_ply_with_positions - 1].num_boards_in_ply !=
      board_db->ply_table[board_db->highest_ply_with_positions - 1].num_boards_processed))?1:0;

  (void) munmap (board_db, board_db->position_database_size);

  if (db_error)
  {
    printf ("ERROR: Insufficient positions or moves for database with depth of %u plies.\n", ply_depth);
    printf ("       Erasing the position database...\n");
    printf ("       Consider increasing CHESS_MAX_POSITIONS or CHESS_MAX_MOVES.\n");
    printf ("\n");
    (void) unlink (POSITION_DB_FILE);
    (void) rmdir (WORK_DIRECTORY_NAME);
    exit (-1);
  }
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
** Create files containing fen positions for each ply in the database.
**
** Return Codes
**  None
**
********************************************************************/
void brdDbFenGenerate (void)
{
  const int position_db_fd = open (POSITION_DB_FILE, O_RDONLY);
  if (position_db_fd < 0)
  {
    printf ("ERROR: Can't open %s\n", POSITION_DB_FILE);
    printf ("       Please create a new position database.\n");
    exit (-1);
  }

  brdDb_t board_db;
  if (sizeof(brdDb_t) != read (position_db_fd, &board_db, sizeof(brdDb_t)))
  {
    perror("read - position datbase");
    exit (-1);
  }

  if (board_db.db_state != BRD_DB_POSITIONS_CREATED)
  {
    printf ("ERROR: The position database seems to be corrupted.\n");
    printf ("       Please create a new position database.\n");
    exit (-1);
  }

  unsigned char *const addr = mmap(0, board_db.position_database_size,
          PROT_READ,
                  MAP_SHARED,
                  position_db_fd,0);
  (void) close (position_db_fd); // Note that the mapping is still valid after the fd is closed.

  if (addr == MAP_FAILED)
  {
    perror ("mmap - position database");
    exit (-1);
  }
  madvise (addr, board_db.position_database_size, MADV_HUGEPAGE);

  board_db.db_entry = (brdDbEntry_t *) (addr + board_db.start_of_db_entry);


  for (unsigned int ply = 0; ply < board_db.max_db_plies; ply++)
  {
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

    for (unsigned int i = 0; i < board_db.ply_table[ply].num_boards_in_ply; i++)
    {
      const brdDbEntry_t *db_entry = 
            &board_db.db_entry[board_db.ply_table[ply].first_board_in_ply_index + i];

      brd_t brd;

      brdUnpack (&brd, db_entry->position);

      char fen[128];
      
      brdutilBrdToFenConvert (&brd, &db_entry->brd_info.brd_info, fen);

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
  }

  (void) munmap (addr, board_db.position_database_size);

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

  const int position_db_fd = open (POSITION_DB_FILE, O_RDONLY);
  if (position_db_fd < 0)
  {
    perror ("open - position database");
    exit (-1);
  }

  brdDb_t board_db;
  if (sizeof(brdDb_t) != read (position_db_fd, &board_db, sizeof(brdDb_t)))
  {
    perror("read - position datbase");
    exit (-1);
  }

  if (board_db.db_state != BRD_DB_POSITIONS_CREATED)
  {
    printf ("ERROR: The position database seems to be corrupted.\n");
    printf ("       Please create a new position database.\n");
    exit (-1);
  }

  if ((depth <= board_db.ply_depth) && (split_factor > 1))
  {
    printf ("ERROR: Split Factor must be 1 for depth less or equal to %u\n",
                    board_db.ply_depth);
    exit (-1);
  }

  unsigned char *const addr = mmap(0, board_db.position_database_size, 
          PROT_READ,
                  MAP_SHARED,
                  position_db_fd,0);
  (void) close (position_db_fd); // Note that the mapping is still valid after the fd is closed.

  if (addr == MAP_FAILED)
  {
    perror ("mmap - position database");
    exit (-1);
  }
  madvise (addr, board_db.position_database_size, MADV_HUGEPAGE);

  board_db.db_entry = (brdDbEntry_t *) (addr + board_db.start_of_db_entry);

  printf ("Writing workload files...\n");
  const plyInfo_t *const ply = &board_db.ply_table[board_db.ply_depth];
  unsigned long long start_workload_number = ply->first_board_in_ply_index;

  for (unsigned int i = 0; i < split_factor; i++)
  {
    unsigned long long num_workloads;

    if (depth <= board_db.ply_depth)
    {
      /* When perft depth is smaller than position database depth then there is no 
      ** work to do for search machines. 
      */
      num_workloads = 0;
    } else
    {
      num_workloads = ply->num_boards_in_ply / split_factor;
      if (i < (ply->num_boards_in_ply % split_factor))
      {
        num_workloads += 1;
      }
    }

    workloadHeader_t workload_header = {
     .search_result = 0,
     .depth = depth,
     .workload_depth = (depth > board_db.ply_depth)?depth - board_db.ply_depth:0,
     .position_db_depth = board_db.ply_depth,
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

       for (unsigned long long k = 0; k < j_inc; k++)
       {
         memcpy (workload_record[k].position, 
               board_db.db_entry[workload_header.start_workload_number + j + k].position, 32);
         workload_record[k].brd_info = board_db.db_entry[workload_header.start_workload_number + j + k].brd_info;
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


  (void) munmap (addr, board_db.position_database_size);

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
                            const brdDb_t *const board_db, 
                            unsigned _BitInt(128) *const position_count_space)
{
  const plyInfo_t *ply = &board_db->ply_table[search_depth - 1];
  const unsigned long long num_boards_in_ply = ply->num_boards_in_ply;

#if 0 // HACK
  printf ("%s %d - search_depth:%u num_board_in_ply:%'llu\n",
                    __FUNCTION__, __LINE__,
                    search_depth, 
                    num_boards_in_ply);
#endif

  for (unsigned long long i = 0; i < num_boards_in_ply; i++)
  {
    brdDbEntry_t *position = &board_db->db_entry[ply->first_board_in_ply_index + i];
    position_count_space[i] = position->status.num_legal_moves;
  }
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
                               const brdDb_t *const board_db, 
                               const unsigned long long *const deep_search_result,
                               unsigned _BitInt(128) *const position_count_space)
{
  const unsigned long long index_offset = 
                board_db->ply_table[position_db_depth].first_board_in_ply_index;


  const plyInfo_t *ply = &board_db->ply_table[position_db_depth - 1];
  const unsigned long long num_boards_in_ply = ply->num_boards_in_ply;

#if 0 // HACK
  printf ("%s %d - position_db_depth:%u index_offset:%'llu num_board_in_ply:%'llu\n",
                    __FUNCTION__, __LINE__,
                    position_db_depth,
                    index_offset, num_boards_in_ply);
#endif

  for (unsigned long long i = 0; i < num_boards_in_ply; i++)
  {
    position_count_space[i] = 0;
    brdDbEntry_t *position = &board_db->db_entry[ply->first_board_in_ply_index + i];
    for (unsigned int j = 0; j < position->status.num_legal_moves; j++)
    {
      const unsigned long long next_node_index = board_db->db_move[position->legal_move_index + j];
      position_count_space[i] += deep_search_result[next_node_index - index_offset];
    }
  }
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
                               const brdDb_t *const board_db,
                               unsigned _BitInt(128) **const position_count_space)
{
  const unsigned int tree_search_depth = (search_depth > position_db_depth)?
                                                position_db_depth - 2:
                                                search_depth - 2;
                                                        
  for (int ply_number = (int) tree_search_depth; ply_number >= 0; ply_number--)
  {
    const plyInfo_t *ply = &board_db->ply_table[ply_number];
    const unsigned long long num_boards_in_ply = ply->num_boards_in_ply;

#if 0 // HACK
    printf ("%s %d - search_depth:%u position_db_depth:%u ply_number:%d num_board_in_ply:%'llu\n",
                    __FUNCTION__, __LINE__,
                    search_depth, position_db_depth,
                    ply_number,
                    num_boards_in_ply);
#endif
    for (unsigned long long i = 0; i < num_boards_in_ply; i++)
    {
      position_count_space[ply_number][i] = 0;
      brdDbEntry_t *position = &board_db->db_entry[ply->first_board_in_ply_index + i];
      unsigned int num_legal_moves = position->status.num_legal_moves;

      for (unsigned int j = 0; j < num_legal_moves; j++)
      {
        const unsigned long long next_node_index = board_db->db_move[position->legal_move_index + j];
        const unsigned long long index_offset_2 = board_db->ply_table[ply_number + 1].first_board_in_ply_index;

        position_count_space[ply_number][i] += 
                        position_count_space[ply_number + 1][next_node_index - index_offset_2];
      }
    }
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

  /* Open the position database.
  */
  const int position_db_fd = open (POSITION_DB_FILE, O_RDONLY);
  if (position_db_fd < 0)
  {
    perror ("open - position database");
    exit (-1);
  }

  brdDb_t board_db;
  if (sizeof(brdDb_t) != read (position_db_fd, &board_db, sizeof(brdDb_t)))
  {
    perror("read - position datbase");
    exit (-1);
  }

  if (board_db.db_state != BRD_DB_POSITIONS_CREATED)
  {
    printf ("ERROR: The position database seems to be corrupted.\n");
    printf ("       Please create a new position database.\n");
    exit (-1);
  }

  unsigned char *const addr = mmap(0, board_db.position_database_size, 
          PROT_READ,
                  MAP_SHARED,
                  position_db_fd,0);
  (void) close (position_db_fd); // Note that the mapping is still valid after the fd is closed.

  if (addr == MAP_FAILED)
  {
    perror ("mmap - position database");
    exit (-1);
  }
  madvise (addr, board_db.position_database_size, MADV_HUGEPAGE);

  board_db.db_entry = (brdDbEntry_t *) (addr + board_db.start_of_db_entry);
  board_db.db_move = (unsigned long long *) (addr + board_db.start_of_move_entry);

  unsigned _BitInt(128) *position_count_space[board_db.max_db_plies - 1];
  unsigned long long position_count_size[board_db.max_db_plies];
  const unsigned int position_db_depth = board_db.max_db_plies - 1;


  /* We need to allocate memory for position counters for every ply in 
  ** the position database, except the last ply. 
  ** For example if a position database has a depth of 8 then we need to allocate
  ** counter space for ply 0 to ply 7.
  */
  for (unsigned int i = 0; i < (board_db.max_db_plies - 1); i++)
  {
    position_count_size[i] = board_db.ply_table[i].num_boards_in_ply * sizeof(_BitInt(128));
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
  **
  ** TODO: 
  **  If memory becomes an issue then we may be able to reduce memory usage
  **  by storing results in 4-byte integers. Since for perft 15 with database
  **  depth of 9 the deep search depth is only 6, the 4 bytes should be sufficient 
  **  to store the move count. 
  */
  const unsigned long long position_count = board_db.ply_table[position_db_depth].num_boards_in_ply;

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

#if 0 // HACK
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
                    workload.position_db_depth, board_db.max_db_plies);
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
        unsigned long long w_index = (workload.start_workload_number - 
                                              board_db.ply_table[position_db_depth].first_board_in_ply_index)
                                             + (bytes_read / sizeof(unsigned long long));
                                             
 #if 0 // HACK
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
                              &board_db, deep_search_result,
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
                            &board_db, 
                            position_count_space[search_depth - 1]);
  }

  /* We don't need the deep search results anymore.
  */
  (void) munmap (deep_search_result, position_count_size[position_db_depth]);

  if (search_depth > 1)
  {
    brdDbPositionTreeAggregate (search_depth,
                               position_db_depth,
                               &board_db,
                               position_count_space);
  }

  *depth = search_depth;
  *perft_result = position_count_space[0][0];

  /* Report perft counts for ply 1. 
  */
  for (unsigned long long i = 0; i < board_db.ply_table[1].num_boards_in_ply; i++)
  {
    ply1_perft_result[i] = position_count_space[1][i];
  }

  for (unsigned int i = 0; i < (board_db.max_db_plies - 1); i++)
  {
    free (position_count_space[i]); 
  }
  (void) munmap (deep_search_result, position_count_size[position_db_depth]);
  (void) munmap (addr, board_db.position_database_size);
  closedir (dir);


}

