/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#ifndef MCPERFT_DEFS_H_INCLUDED
#define MCPERFT_DEFS_H_INCLUDED

#include <pthread.h>
#include <semaphore.h>
#include "mcperft.h"
#include "mcperft_api.h"

/* Help the optimizer create better code.
*/
#if 1
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely
#define unlikely
#endif


#define ONE_GB ((size_t)0x40000000LLU)

/* Maximum number of plies in the board database.
** This just needs to be some large value that we will never reach. 
*/
#define MAX_BRD_PLIES 12

/* Maximum number of workload files for deep search.
*/
#define MAX_SEARCH_WORKLOADS 10'000

/* The position database is mapped to a file. 
** These are the file names for various databases.
*/
#define DIR_NAME "board-db"
#define WORK_DIRECTORY_NAME "./" DIR_NAME "/"
#define POSITION_DB_FILE WORK_DIRECTORY_NAME "position_db"

/* The FEN database file prefix. 
** Note that the FEN database is only for debugging. The FEN database is 
** not used in the perft counting.
*/
#define FEN_DB_FILE_PREFIX \
                    WORK_DIRECTORY_NAME "fen_db_ply_"

/* This directory holds files to be distributed to perft position
** counter machines.
*/
#define DEEP_SEARCH_WORKLOAD_DIR_NAME "workload-files"
#define DEEP_SEARCH_WORKLOAD_DIR \
                    WORK_DIRECTORY_NAME  DEEP_SEARCH_WORKLOAD_DIR_NAME 
#define DEEP_SEARCH_WORKLOAD_FILE_PREFIX \
            DEEP_SEARCH_WORKLOAD_DIR "/workload_"
 

#define DEEP_SEARCH_RESULT_DIR_NAME "result-files"
#define DEEP_SEARCH_RESULT_DIR \
                    WORK_DIRECTORY_NAME  DEEP_SEARCH_RESULT_DIR_NAME 
#define DEEP_SEARCH_RESULT_FILE_PREFIX \
            DEEP_SEARCH_RESULT_DIR "/result_"

#define DEEP_SEARCH_RESULT_TEMP_FILE "temp_result"

/* The deep search algorithm reads work loads from a file
** using the number of entries specified below.
** Since each workload entry is 40 bytes and search result is 8 bytes, 
** setting the chunk size to 1 million requires about 48MB of DRAM per core. 
** 
** When all entries in the chunk are processed, the deep search threads
** shut down and the chunk is written to the search results file.
** This continues until the whole workload file is processed.
**
** Increasing this value uses more DRAM, but reduces how often the 
** data is written to a file, which improves performance.
**
** The actual allocated memory for each chunk is determined
** by the numer of cores and the search depth. Larger number of cores
** increases the memory requirements. 
** Deeper search reduces the memory requirements, since each workload 
** takes more time, we use smaller chunks in order to target results
** file update about every 10 minutes.
*/
#define MAX_DEEP_SEARCH_PER_CORE_CHUNK_SIZE 1'000'000LLU

typedef union
{
  brdCtrlInfo_t brd_info;
  unsigned short  brd_info_mem;
} brdCtrl_u;

/* This structure is at the top of workload and result files.
*/
typedef struct
{
  unsigned int search_result; // 0 - Contains Workloads, 1 - Contains Results.
  unsigned int depth; // Original Search Depth
  unsigned int workload_depth; // Depth to which workloads in this file are searched.
  unsigned int position_db_depth; // Database depth used to construct the workload.
  unsigned int split_factor; // The number of files into which the workload was split.
  unsigned int workload_factor; // The file number for this workload. Starts with 1.
  unsigned long long num_workloads; // Workloads in file. Could be 0.
  unsigned long long start_workload_number; // Starting workload in file. Can be 0.  
  unsigned long long end_workload_number; // Ending workload in file. Can be 0.
} workloadHeader_t;

/* Workload Record. 
** The structure is padded to make sure that the size is divisible by 8.
** The structure size is 40 bytes.
*/
typedef struct
{
  unsigned long long position[4]; /* 32-byte Board Position */
  brdCtrl_u brd_info;  /* 2-byte board control information */
  unsigned short pad1;
  unsigned int pad2;
} workloadRecord_t;

 
/* Ply and Database Statistics.
** All counters in this structure must be defined as "unsigned long long".
*/
typedef struct 
{
  unsigned long long unique_positions_added; /* Unique Positions in the Database */
  unsigned long long duplicate_positions_detected; /* Tried to add to DB, but found a duplicate */
  unsigned long long total_moves_added; /* Total moves added to the move database */ 

  /* Number of hash collisions. This means that multiple positions hashed into the 
  ** same hash index.
  */
  unsigned long long num_hash_collisions;

  /* These counters record how many positions were not added to the 
  ** database.
  */
  unsigned long long position_database_full;
  unsigned long long move_database_full;

  /* Performance Measurement Parameters.
  */
  unsigned long long board_db_insert_time_msec;

  /* Place Holders in case we want to add more statistics without 
  ** changing the database format.
  */
  unsigned long long place_holder[16];

} chessStat_t;

/* Ply information structure. 
*/
typedef struct
{
  /* All boards for the same ply have consecutive indexes, so we only need to know
  ** the starting index and the number of boards in the ply.
  */
  unsigned long long first_board_in_ply_index;
  unsigned long long num_boards_in_ply;
  color_e whose_move;

  /* The number of boards that have been successfully processed in this ply.
  ** This value is 0 for the highest_ply_with_positions ply.
  ** This value is equal to num_boards_in_ply for plies 0 to highest_ply_with_positions - 2.
  ** For the (highest_ply_with_positions - 1) this value is the number of positions that 
  ** have been handled before the database ran out of memory. If the database didn't run out
  ** of memory then the value is equal to num_boards_in_ply.
  */
  unsigned long long num_boards_processed;

  /* This counter indicates the next board to be deep searched in this ply.
  ** The counter is incremented atomically by deep search threads.
  */
  unsigned long long next_deep_search_board;

  chessStat_t stats; /* Ply Statistics */

  /* Place holder in case we want to add additional ply information witout
  ** changing the datbase format.
  */
  unsigned long long place_holder [16];

} plyInfo_t;


typedef struct
{
  /* This flag indicates that the position_at_depth counter has been 
  ** computed for this entry.
  */
  unsigned short positions_at_depth_computed:1;

  /* Number of legal moves from this position.
  ** If this value is 0 then there are either no legal moves,
  ** or we ran out of memory for storing moves for next board
  ** positions, or the game is over due to lack of material for a win on 
  ** both sides.
  */
  unsigned short num_legal_moves:9;

  /* Reserved for future use.
  */
  unsigned short place_holder:6;
} brdStatusInfo_t;

typedef struct
{
  /* Start of hash key.
  */
  unsigned long long position[4]; /* 32-byte Board Position */
  unsigned short ply_number;
  brdCtrl_u brd_info;  /* 2-byte board control information */
  /* End of hash key.
  */

  /* Reserved for future use.
  ** These two bytes have no effect on the structure size, so its OK
  ** to use them in the future.
  */
  unsigned short place_holder;

  /* The status of this entry.
  */
  brdStatusInfo_t status; /* 2-byte structure */

  /* When multiple boards hash into the same index, the next_brd_in_cache
  ** indictes the next board entry with the same hash index.
  ** If this value is set to 0 then there is no next board with the 
  ** same hash index.
  */
  unsigned long long next_brd_in_cache;


  /* The location in the global move table where moves
  ** for this position are recorded. 
  ** Note that 0 is a valid move location.
  */
  size_t legal_move_index;

} brdDbEntry_t __attribute__ ((aligned (8)));

/* Board Database Control Structure.
*/
typedef struct 
{
  /* Number of plies with positions.
  */
  unsigned int ply_depth;

  /* Total number of bytes allocated for the position database.
  ** This doesn't include the hash table.
  */
  unsigned long long position_database_size;

  /* Offsets of board_db and movce_db databases from the start of the 
  ** position database file.
  */
  unsigned long long start_of_db_entry;
  unsigned long long start_of_move_entry;

  /* Flag indicating the state of the database.
  **
  ** INCOMPLETE means that the position creation was not done. This may happen 
  ** if the program is terminated before the position tree is fully created. 
  **
  ** POSITIONS_CREATED means that the database is ready for action.
  */
#define BRD_DB_INCOMPLETE 1   
#define BRD_DB_POSITIONS_CREATED 2
  unsigned int db_state;

  /* Mutual exclusion lock for the board database.
  ** The mutex is only used for controlling access to the ply statistics structure.
  ** The board positions, moves, and hash table entries are updated using atomic
  ** instructions, so the mutex is not needed for that.
  */
  pthread_mutex_t brd_mutex;

  unsigned long long max_db_entries;
  size_t       db_size_in_bytes;
  brdDbEntry_t *db_entry;

  size_t       max_hash_entries;
  size_t       hash_size_in_bytes;
  unsigned long long *db_hash;

  size_t max_move_entries;
  size_t move_size_in_bytes;
  unsigned long long *db_move;

  /* Maximum number of plies in the board database.
  */
  unsigned int max_db_plies;

  plyInfo_t ply_table[MAX_BRD_PLIES];

  /* The highest ply number that contains positions. 
  */
  unsigned int highest_ply_with_positions;

  /* Next Available Board Entry Index (db_entry database).
  ** We skip the index 0, and start adding boards at index 1.
  */
  unsigned long long next_free_index;

  /* Next Available Entry in the Move Database.
  */
  size_t next_free_move_index;

  /* Aggregate Statistics for All Plies.
  */
  chessStat_t stats; /* Ply Statistics */

  /* Total run time for the most recent board creation in milliseconds.
  */
  unsigned long long position_db_run_time;

  /* Total run time for the deep search for all positions.
  */
  unsigned long long deep_search_run_time;

  /* Total number of positions found during deep search.
  */
  unsigned _BitInt(128) deep_search_positions;

  /* Place holder in case we want to add additional board information witout
  ** changing the datbase format.
  */
  unsigned long long place_holder [16];

} brdDb_t;

/* Structure for storing piece position.
*/
typedef struct
{
  unsigned int column; /* a-h */
  unsigned int row;    /* 1-8 */
} piece_location_t;


/******************************************************************************
** Generate the board database from the given position.
** The assumption is that the start position is legal.
** Note that the database may run out of resources before reaching the 
** requested number of plies. 
**
** brd - Initial Position
** info - Initial Position Info.
**
** Return Values:
******************************************************************************/
void brdDbGenerate(
                   const unsigned int ply_depth,
                   const unsigned long long max_positions,
                   const unsigned long long max_moves,
                   const brd_t *const brd,
                   const brdCtrlInfo_t *const info);

/******************************************************************************
** Convert 32-byte array for storing in the board database into 8x8 board.
**
** brd - (output) 8x8 Board
** position - (input) 32-byte position data block.
**
** Return Values:
******************************************************************************/
void brdUnpackBoard (brd_t* const brd, const unsigned long long * const position);

/********************************************************************
** Create files containing fen positions for each ply in the database.
**
** Return Codes
**  None
**
********************************************************************/
void brdDbFenGenerate (void);


/******************************************************************************
** Generate Deep Search Workload Files.
**
** depth - Search Depth.
** split_factor - Number of workload files.
**
** Return Values:
******************************************************************************/
void brdDbCountSetup (const unsigned int depth,
                      const unsigned int split_factor);

/********************************************************************
** Perform deep search on the specified workload.
**
** workload_file -  Work load file name.
**
** Return Codes
**  Position Status
**
********************************************************************/
void brdDbCount (const char *workload_file);

/********************************************************************
** Analyze all the result files and compute the final perft value.
**
** Return Codes 
**  None 
**
********************************************************************/
void brdDbAggregate (unsigned int *depth,
                     unsigned _BitInt(128) *perft_result,
                     unsigned _BitInt(128) *ply1_perft_result);

/********************************************************************
** Get system up time in milliseconds.
** The up time is based on when the process started running.
** The time wraps approximately every 49 days.
**
** Return Value:
** Time in milliseconds.
********************************************************************/
unsigned long long sysUpTimeMillisecondsGet(void);


#endif /* MCPERFT_DEFS_H_INCLUDED */
