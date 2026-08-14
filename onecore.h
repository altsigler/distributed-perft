/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#ifndef ONECORE_H_INCLUDED
#define ONECORE_H_INCLUDED

#include "brdutil_api.h"
#include "movegen.h"

/* This constant defines the maximum number of moves we expect from 
** each position.
** Note that a legal chess position can have up to 218 moves. However,
** to conserve memory we pick a smaller number that is likely to work 
** for most positions. If the number of positions is insufficient then 
** the code abandons the database creation and invokes simple move search. 
*/
//#define MAX_ONECORE_BRD_MOVES 60
#define MAX_ONECORE_BRD_MOVES 50

/* The number of plies for which the position database is created.
** The purpose of the database is to eliminate duplicate positions from the 
** search. 
** The first duplicate positions are found at ply 3, so the database must 
** be created for at least the first three plies of the search. 
** Creating the database for 4 plies works well for "perft 7", but is slower
** than 3 plies for "perft 6". Therefore even when the MAX_ONECORE_BRD_PLIES
** is set to 4, the program only generates database for 3 plies unless the 
** depth is 7 or higher.
** For depth 1, 2, 3, and 4 the program doesn't generate the database.
*/
#define MAX_ONECORE_BRD_PLIES 4

/* The current implementation supports only values 3 or 4 for the MAX_ONECORE_BRD_PLIES.
*/
#if ((MAX_ONECORE_BRD_PLIES != 3) && (MAX_ONECORE_BRD_PLIES != 4))
#error Please set the MAX_ONECORE_BRD_PLIES to 3 or 4.
#endif

/* Ply information structure.
*/
typedef struct
{
  /* All boards for the same ply have consecutive indexes, so we only need to know
  ** the starting index and the number of boards in the ply.
  */
  unsigned int first_board_in_ply_index;
  unsigned int num_boards_in_ply;
  color_e whose_move;

  unsigned int num_duplicates;

} ocPlyInfo_t __attribute__ ((aligned (8)));

typedef struct
{
  /* This is a perft counter to report the number of positions at perft depth.
  */
  unsigned long long  positions_at_depth;

} ocPerftStatusInfo_t;


typedef struct
{
  /* Start of hash key.
  */
  union
  {
    /* After the board database is created, the position is no longer needed.
    ** Therefore the position and perft_status are combined together to save memory.
    */
    struct {
      bitBrd_t bit_brd;
      castleEligibility_t castle_eligibility;
      unsigned int en_passant_eligible_pawn;
      unsigned int ply_number;
    } key __attribute__ ((aligned (8)));
    ocPerftStatusInfo_t perft_status;
  } u;
  /* End of hash key.
  */

  /* Number of moves for this entry.
  */
  unsigned int num_legal_moves;

  /* The location in the global move table where moves
  ** for this position are recorded.
  ** Note that 0 is a valid move location.
  */
  unsigned int legal_move_index;

  /* When multiple entries hash into the same hash index, this is the index
  ** of the next DB entry in the hash list.
  */
  unsigned int next_in_hash;

} ocBrdDbEntry_t __attribute__ ((aligned (8)));


/* Board Database Control Structure.
*/
typedef struct
{
  ocBrdDbEntry_t *db_entry;
  unsigned int *db_hash;
  unsigned int *db_move;

  /* Number of entries in the db_hash table.
  */
  unsigned long long onecore_hash_entries;
  unsigned long long onecore_db_size;
  unsigned long long onecore_move_db_size;

  /* The information for the initial position is stored in ply_table[0].
  */
  ocPlyInfo_t ply_table[MAX_ONECORE_BRD_PLIES + 1];

  /* Next Available Board Entry Index (db_entry database).
  */
  unsigned int next_free_index;

  /* Next Available Entry in the Move Database.
  */
  unsigned int next_free_move_index;

  /* Number of hash collisions while checking for duplicate positions.
  */
  unsigned int hash_collisions;

} ocBrdDb_t;



#endif // ONECORE_H_INCLUDED
