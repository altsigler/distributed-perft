/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#ifndef MCPERFT_DEFS_H_INCLUDED
#define MCPERFT_DEFS_H_INCLUDED

#include "mcperft.h"
#include "mcperft_api.h"

/* Maximum number of plies in the board database.
** This just needs to be some large value that we will never reach. 
** This is used for storing per-ply statistics, which is a small
** structure.
*/
#define MAX_BRD_PLIES 100

/* Maximum number of workload files for deep search.
*/
#define MAX_SEARCH_WORKLOADS 10'000

/* The position database is mapped to a file. 
** These are the file names for various databases.
*/
#define DIR_NAME "board-db"
#define WORK_DIRECTORY_NAME "./" DIR_NAME "/"
#define POSITION_DB_DIRECTORY WORK_DIRECTORY_NAME "position_db/"

/* The position database is comprised of two files for each ply except the deepest ply.
** These files are a list of positions for the ply and the list of moves leading 
** to the next ply. At the deepest ply only the list of positions file is present.
**
** The plies are counted starting with index 0, which the the initial position. 
** The file ply_0_positions contains exactly one position, which is the standard
** starting position.
** The file ply_0_moves contains 20 moves, which is the number of legal moves from 
** the standard starting position. 
** The ply_1_positions contains 20 positions, and so on.
** Only unique positions are recorded in the ply position file. Each position entry 
** contains the position and the index of the first move entry from this position.
** The index is relative to the start of the ply move file, so the first index is always 0.
**
** The ply move file contains an index of the position in the next ply position file. 
** These indexes are relative to the start of the file, so the first position is always 0.
**
*/
#define PLY_FILE_PREFIX POSITION_DB_DIRECTORY "ply_"

/* While generating positions for a ply, the code creates temporary files 
** of positions. The positions contained in each files are sorted, but can have 
** duplicates. These files are called sort blocks. The files are deleted after the 
** ply position file is generated. 
** The file names start with "position_block_0". The first file is always created,
** even when there is only one position in the ply. The subsequent files are called 
** position_block_2, position_block_3, and so on until all ply positions are generated.
*/
#define SORT_BLOCK_PREFIX WORK_DIRECTORY_NAME "position_block_"

/* While generating moves for a ply, the code creates temporary files
** of moves. The moves contained in each file are sorted by move index and 
** can NOT have duplicates. 
** The sort block files are deleted after the final move file is generated.
** The sort blocks are named move_block_0, move_block_1, and so on.
*/
#define SORT_MOVE_BLOCK_PREFIX WORK_DIRECTORY_NAME "move_block_"


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
** by the number of cores and the search depth. Larger number of cores
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

 
typedef struct __attribute__((packed))
{ 
  unsigned char high;
  unsigned int low;
} moveEntry_t;

__attribute__((always_inline))
inline moveEntry_t moveIndexToEntry (const unsigned long long index)
{ 
  const moveEntry_t move_entry = {(unsigned char) (index >> 32), (unsigned int) index};
  return move_entry; 
}
  
__attribute__((always_inline))
inline unsigned long long moveEntryToIndex (const moveEntry_t move_entry)
{ 
  return ((unsigned long long) move_entry.high << 32) | (unsigned long long) move_entry.low;
} 
  
/* This structure is used for the temporary sort block move files.
*/
typedef struct
{
  unsigned long long position_index; // The actual move record.
  unsigned long long move_index; // Position of the move record in the final move file.
} sortBlockMoveEntry_t __attribute__ ((aligned (8)));

/* This structure is used for the temporary sort block position files.
*/
typedef struct 
{
  unsigned long long position[4]; /* 32-byte Board Position */
  brdCtrl_u brd_info;  /* 2-byte board control information */
  moveEntry_t move_entry; /* 5-byte move entry which points to this position */
  unsigned char pad; /* Pad to align to 8-byte boundary */
} sortBlockEntry_t __attribute__ ((aligned (8)));

/* Buffered file context.
*/
typedef struct
{
    unsigned char *buffer;
    unsigned long long buffer_size_in_bytes;
    unsigned long long entry_size_in_bytes;
    unsigned long long max_entries_in_buffer;


    unsigned long long num_entries_in_buffer; 


    int fd;
    unsigned long file_block_size;
    unsigned long long total_bytes_written_in_file;

} bufferedFile_t;


/* Structure for holding information about sorted blocks while
** merging these blocks into the ply position file.
*/
typedef struct
{
    sortBlockEntry_t *buffer;
    unsigned long long buffer_index;
    int fd;
    unsigned long file_block_size;
    unsigned int file_is_open;
    unsigned int file_is_empty; /* No More Positions in this file */

    unsigned long long max_elements_in_block; 

    /* Number of positions in the current block. 
    */
    unsigned long long num_elements_in_block; 

    char file_name[1024];
} mergeBlock_t;

/* Structure for holding information about sorted move blocks while
** merging these blocks into the ply position file.
*/
typedef struct
{
    sortBlockMoveEntry_t *buffer;
    unsigned long long buffer_index;
    int fd;
    unsigned int file_is_open;
    unsigned int file_is_empty; /* No More Positions in this file */

    /* Number of positions in the current block. 
    */
    unsigned long long num_elements_in_block; 

    char file_name[1024];
} mergeMoveBlock_t;

/* This structure is used for ply position files.
*/
typedef struct
{
  unsigned long long position[4]; /* 32-byte Board Position */
  brdCtrl_u brd_info;  /* 2-byte board control information */
  unsigned char num_moves; /* Number of moves from this position (1 byte) */
  moveEntry_t first_move_index; /* First move for this position in ply moves file (5 bytes) */  
} plyPositionEntry_t __attribute__ ((aligned (8)));

/* Ply and Database Statistics.
** All counters in this structure must be defined as "unsigned long long".
*/
typedef struct 
{
  unsigned long long unique_positions_added; /* Unique Positions in the Database */
  unsigned long long duplicate_positions_detected; /* Tried to add to DB, but found a duplicate */
  unsigned long long total_moves_added; /* Total moves added to the move database */ 

  /* Performance Measurement Parameters.
  */
  unsigned long long board_db_insert_time_msec;

} chessStat_t;

/* Ply information structure. 
*/
typedef struct
{
  unsigned long long num_boards_in_ply;
  color_e whose_move;

  chessStat_t stats; /* Ply Statistics */
} plyInfo_t;


/* Board Database Control Structure.
** This structure is used during board database creation to keep track 
** of creation statistics. 
*/
typedef struct 
{
  void *sort_block;
  sortBlockEntry_t *sort_block_position;
  unsigned long long max_sortblock_positions;

  sortBlockMoveEntry_t *sort_block_move;
  unsigned long long max_sortblock_moves;

  /* Number of plies with positions.
  */
  unsigned int ply_depth;

  plyInfo_t ply_table[MAX_BRD_PLIES];

  /* Aggregate Statistics for All Plies.
  */
  chessStat_t stats; /* Ply Statistics */

  /* Total run time for the most recent board creation in milliseconds.
  */
  unsigned long long position_db_run_time;

} brdDb_t;

/* Board Database Generation Thread Status.
*/
typedef struct
{
  /* Number of positions in the current ply for which next positions have been 
  ** generated.
  */
  unsigned long long positions_processed;

  /* Number of blocks that have been sorted and stored in a file.
  ** This counter is the number of temporary files that have been created
  ** to store positions.
  */
  unsigned int sort_blocks_created;

  /* There are three phases while creating positions for the ply.
  ** 1 - Generating next positions.
  ** 2 - Sorting positions.
  ** 3 - Detecting Duplicate Positions.
  **
  ** The phases 1 and 2 may repeat several times depending on how many sort blocks 
  ** are needed to process all ply positions.
  **
  ** Phase 3 is when all sort blocks are merged together into one ply_n_positions
  ** file and ply_n_moves file is updated to remove references to duplicate positions.
  */
  unsigned int ply_processing_phase;


  /* These counters refer to the number of positions processed in phase 3.
  */
  /* How many total positions are in the new ply.
  */
  unsigned long long total_new_ply_positions;

  /* How many positions from total_new_ply_positions have been processed.
  */
  unsigned long long processed_new_ply_positions;
} brdGenThreadStatus_t;

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
