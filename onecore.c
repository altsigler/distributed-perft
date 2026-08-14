/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <sys/mman.h>

#include "bytebrd_api.h"
#include "onecore_api.h"
#include "movegen.h"
#include "onecore.h"

/* These global variables can be set to override default scaling
** parameters and operating mode. 
** These parameters affect all threads that use the onecorePerft()
** function and should be set only one time during users initialization.
*/

/* Valid alues are:
** 0 - Disable position caching.
** 3 - Enable Caching positions at ply 1, 2, 3.
** 4 - Enable Caching Positions at ply 1, 2, 3, 4.
*/
unsigned int max_onecore_brd_plies = MAX_ONECORE_BRD_PLIES;

/* Nuber of positions in the cache.
** Set to 0 in order to use the default number of positions.
*/
unsigned long long override_onecore_db_entries = 0;

/* Number of moves in the cache. 
** Set to 0 in order to use the default number of moves.
*/
unsigned long long override_onecore_move_db_entries = 0;

/* Number of entries in the hash table.
** Set to 0 to in order to use the default number of hash entries.
*/
unsigned long long override_hash_entries = 0;


/********************************************************************
** Get system up time in milliseconds.
** The up time is based on when the process started running.
** The time wraps approximately every 49 days.
**
** Return Value:
** Time in milliseconds.
********************************************************************/
static unsigned long long sysUpTimeMillisecondsGet(void)
{
  unsigned long long time_ms;
  struct timespec time;
  int rc;
  static unsigned int first_time = 1;
  static unsigned long long first_time_ms;

  rc = clock_gettime (CLOCK_MONOTONIC, &time);
  assert (rc == 0);




  time_ms = (unsigned long long) time.tv_sec * 1000;
  time_ms += (unsigned long long) (time.tv_nsec / 1000000);

  if (first_time)
  {
    first_time = 0;
    first_time_ms = time_ms;
  }
  time_ms -=  first_time_ms;

  return time_ms;
}


/******************************************************************************
** Show Move Results.
******************************************************************************/
__attribute__((noinline))
static void moveResultsShow (const oneMove_t *const one_move,
                            const unsigned long long per_move_total_at_depth)
{
   unsigned char promoted_pawn;
   constexpr char piece_name[] = {0,0,'n','b','r','q',0,0};

   printf ("%c%c", 
                colName[one_move->from_c],
                rowName[one_move->from_r]);
   printf ("%c%c", 
                colName[one_move->to_c],
                rowName[one_move->to_r]);
   if ((PIECE_GET(one_move->p1) == S_PAWN) &&
        (((COLOR_GET(one_move->p1) == S_WHITE) && (one_move->to_r == (BRDS-1))) ||
         ((COLOR_GET(one_move->p1) == S_BLACK) && (one_move->to_r == 0))))
   {
     if (one_move->num_masks == 3)
                        promoted_pawn = one_move->p3;
            else
                        promoted_pawn = one_move->p2;
     printf ("%c", piece_name[PIECE_GET(promoted_pawn)]);
   }
   printf (": %'llu\n", per_move_total_at_depth);
}

/******************************************************************************
** Set up the bit board and game status flags.
**
******************************************************************************/
static void searchSetup (const brd_t *const brd,
                         const brdCtrlInfo_t *const info,
                         bitBrd_t *restrict bit_brd,
                         castleEligibility_t *castle_eligibility,
                         unsigned int *en_passant_eligible_pawn)
{
  /* Set up the bit board.
  */
  {
    bitbrdClear (bit_brd);
    for (unsigned int row = 0; row < BRDS; row++)
    {
      for (unsigned int column = 0; column < BRDS; column++)
      {
        if (S_EMPTY != brd->rc[row][column])
           bitbrdPieceSet(bit_brd, brd->rc[row][column], row, column);
      }
    }
  }
  castle_eligibility->white_short_ineligible =
          (info->white_short_castle_eligible)?0:1;
  castle_eligibility->white_long_ineligible =
          (info->white_long_castle_eligible)?0:1;
  castle_eligibility->black_short_ineligible =
          (info->black_short_castle_eligible)?0:1;
  castle_eligibility->black_long_ineligible =
            (info->black_long_castle_eligible)?0:1;

  if (info->en_passant_capture_eligible)
  {
    *en_passant_eligible_pawn =  (unsigned int) (info->en_passant_row << 3) | info->en_passant_column;
  } else
  {
    *en_passant_eligible_pawn = 0;
  }
}

/******************************************************************************
** Count the number of legal moves from this position.
** The assumption is that the start position is legal.
**
** depth - How deep to search.
** brd - Initial Position
** info - Initial Position Info.
** silent - Don't print on console.
**
** Return Values:
** Number of legal moves discovered for this position at specified depth.
******************************************************************************/
static unsigned long long positionSearch(const unsigned int depth,
                ocBrdDb_t *brd_db, 
                const unsigned int silent,
                const unsigned int onecore_brd_plies)
{
  const ocPlyInfo_t *last_ply = &brd_db->ply_table[onecore_brd_plies];
  unsigned long long total_positions_at_depth;

  for (unsigned int i = 0; i < last_ply->num_boards_in_ply; i++)
  {
    unsigned long long num_positions_at_depth;

    ocBrdDbEntry_t *brd_entry = &brd_db->db_entry[last_ply->first_board_in_ply_index + i];

    /* Compute the number of positions at depth from the current position.
    */
    unsigned int new_depth = depth - onecore_brd_plies;

    /* Note that the position tables are not built unless the depth is more than 
    ** one greater than the onecore_brd_plies. Therefore we don't need to 
    ** invoke allMoveCandidateLastPlyFindApi. 
    */
#if 0
    if (new_depth == 1)
    {
      num_positions_at_depth = 
                allMoveCandidatesLastPlyFindApi (last_ply->whose_move, 
                                            &brd_entry->u.key.bit_brd, 
                                            brd_entry->u.key.en_passant_eligible_pawn, 
                                            brd_entry->u.key.castle_eligibility);
    } else
#endif
    {
      num_positions_at_depth = allMovePerft (last_ply->whose_move, 
                                        &brd_entry->u.key.bit_brd, 
                                        brd_entry->u.key.en_passant_eligible_pawn, 
                                        brd_entry->u.key.castle_eligibility, 
                                        new_depth - 1, 0);
    }

    /* Store the computed number of positions in the database entry. We overwrite 
    ** the position information with status information.
    */
    brd_entry->u.perft_status.positions_at_depth = num_positions_at_depth;
  }

  for (int ply = (int) (onecore_brd_plies - 1); ply >= 0; ply--)
  {
    const ocPlyInfo_t *ply_ptr = &brd_db->ply_table[ply];

    for (unsigned int i = 0; i < ply_ptr->num_boards_in_ply; i++)
    {
      ocBrdDbEntry_t *brd_entry = &brd_db->db_entry[ply_ptr->first_board_in_ply_index + i];
      unsigned long long num_positions_at_depth = 0;

      /* The number of positions in this node is the sum of all positions in the 
      ** child nodes.
      */
      for (unsigned int j = 0; j < brd_entry->num_legal_moves; j++)
      {
        unsigned int child_node_index = brd_db->db_move[brd_entry->legal_move_index + j];
        const ocBrdDbEntry_t *child_brd_entry = &brd_db->db_entry[child_node_index];

        num_positions_at_depth += child_brd_entry->u.perft_status.positions_at_depth;
      }

      brd_entry->u.perft_status.positions_at_depth = num_positions_at_depth;
    }
  }

  /* The root node now cotains the total positions at depth.
  */
  total_positions_at_depth = brd_db->db_entry[0].u.perft_status.positions_at_depth;

  if (0 == silent)
  {
    /* Need to show per-move position counts.
    */
    oneMove_t one_move[MAX_BRD_MOVES];
    const ocPlyInfo_t *ply_ptr = &brd_db->ply_table[1];
    ocBrdDbEntry_t *root_brd_entry = &brd_db->db_entry[0];
    const unsigned int num_moves = (unsigned int)
                      allMoveCandidatesFind (brd_db->ply_table[0].whose_move,
                          &root_brd_entry->u.key.bit_brd,
                          one_move,
                          root_brd_entry->u.key.en_passant_eligible_pawn,
                          root_brd_entry->u.key.castle_eligibility);

    for (unsigned int i = 0; i < num_moves; i++)
    {
      ocBrdDbEntry_t *brd_entry = &brd_db->db_entry[ply_ptr->first_board_in_ply_index + i];
      moveResultsShow (&one_move[i], 
                        brd_entry->u.perft_status.positions_at_depth);
    }

  }

  return total_positions_at_depth;
}

/******************************************************************************
** Debug function to display Board Databas Information.
******************************************************************************/
__attribute__((unused))
static void ocBrdDbShow (ocBrdDb_t *brd_db,
                         const unsigned int onecore_brd_plies)
{
  printf ("Board Database Structure Size (sizeof(ocBrdDb_t)):%'zu\n", sizeof(ocBrdDb_t));
  printf ("Maximum number of positions (ocBrdDb_t->onecore_db_size):%'llu\n", brd_db->onecore_db_size);
  printf ("Maximum number of moves (ocBrdDb_t->onecore_move_db_size):%'llu\n", brd_db->onecore_move_db_size);
  printf ("Maximum number of hash entries (brd_db->onecore_hash_entries):%'llu\n", brd_db->onecore_hash_entries);

  printf ("Number of Positions (ocBrdDb_t->next_free_index):%'u\n", brd_db->next_free_index);
  printf ("Number of Moves (ocBrdDb_t->next_free_move_index):%'u\n", brd_db->next_free_move_index);
  printf ("Number of hash collisions (ocBrdDb_t->hash_collisions):%'u\n", brd_db->hash_collisions);

  for (unsigned int i = 0; i < (onecore_brd_plies + 1); i++)
  {
    printf ("%u - whose_move:%s first_board_in_ply_index:%'u num_boards_in_ply:%'u num_duplicates:%'u\n",
                i,
                (brd_db->ply_table[i].whose_move ==MOVE_WHITE)?"White":"Black", 
                brd_db->ply_table[i].first_board_in_ply_index, 
                brd_db->ply_table[i].num_boards_in_ply,
                brd_db->ply_table[i].num_duplicates); 
  }
}

/******************************************************************************
** Detect whether a duplicate position already exists. 
**
** brd_db - Board Database.
**
** Return Values:
** 0 - No duplicate found.
** 1 - Duplicate Found.
******************************************************************************/
unsigned int duplicateDetect (ocBrdDb_t *const brd_db, 
                              ocBrdDbEntry_t *const brd_entry, 
                              unsigned int *const next_board_index)
{
  unsigned int duplicate_detected = 0;

  /* Compute the hash index for the new entry.
  */
  unsigned int hash_index = 0x811c9dc5;

  hash_index = __builtin_ia32_crc32si(hash_index, brd_entry->u.key.ply_number);

  /* Avoid strict aliasing error by copying the castle eligibility structure 
  ** into a temporaty variable.
  */
  unsigned int temp;
  memcpy (&temp, &brd_entry->u.key.castle_eligibility, sizeof(temp));

  hash_index = __builtin_ia32_crc32si(hash_index, temp);
  hash_index = __builtin_ia32_crc32si(hash_index,  brd_entry->u.key.en_passant_eligible_pawn);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_PAWN | S_WHITE]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_KNIGHT | S_WHITE]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_BISHOP | S_WHITE]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_ROOK | S_WHITE]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_QUEEN | S_WHITE]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_KING | S_WHITE]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_PAWN | S_BLACK]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_KNIGHT | S_BLACK]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_BISHOP | S_BLACK]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_ROOK | S_BLACK]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_QUEEN | S_BLACK]);
  hash_index = (unsigned int) __builtin_ia32_crc32di(hash_index, brd_entry->u.key.bit_brd.piece[S_KING | S_BLACK]);

  hash_index %= (unsigned int) brd_db->onecore_hash_entries;

  unsigned int next_in_hash = brd_db->db_hash[hash_index];
  while (0 != next_in_hash)
  {
    /* We found an existing entry with the same hash value. 
    ** It is possible that there is a hash collision, so we need to verify that 
    ** the existing entry and the new entry actually match.
    ** Entries that cause a hash collision are simply treated as new entries. The assumption
    ** is that the number of hash collisions is very small.
    */
    ocBrdDbEntry_t *old_brd_entry = &brd_db->db_entry[next_in_hash]; 
    
    if ((brd_entry->u.key.ply_number == old_brd_entry->u.key.ply_number) &&
        (0 == memcmp(&brd_entry->u.key.castle_eligibility, &old_brd_entry->u.key.castle_eligibility, sizeof(castleEligibility_t))) &&
        (brd_entry->u.key.en_passant_eligible_pawn == old_brd_entry->u.key.en_passant_eligible_pawn) &&
        (brd_entry->u.key.bit_brd.piece[S_PAWN | S_WHITE] == old_brd_entry->u.key.bit_brd.piece[S_PAWN | S_WHITE]) && 
        (brd_entry->u.key.bit_brd.piece[S_KNIGHT | S_WHITE] == old_brd_entry->u.key.bit_brd.piece[S_KNIGHT | S_WHITE]) && 
        (brd_entry->u.key.bit_brd.piece[S_BISHOP | S_WHITE] == old_brd_entry->u.key.bit_brd.piece[S_BISHOP | S_WHITE]) && 
        (brd_entry->u.key.bit_brd.piece[S_ROOK | S_WHITE] == old_brd_entry->u.key.bit_brd.piece[S_ROOK | S_WHITE]) && 
        (brd_entry->u.key.bit_brd.piece[S_QUEEN | S_WHITE] == old_brd_entry->u.key.bit_brd.piece[S_QUEEN | S_WHITE]) && 
        (brd_entry->u.key.bit_brd.piece[S_KING | S_WHITE] == old_brd_entry->u.key.bit_brd.piece[S_KING | S_WHITE]) && 
        (brd_entry->u.key.bit_brd.piece[S_PAWN | S_BLACK] == old_brd_entry->u.key.bit_brd.piece[S_PAWN | S_BLACK]) && 
        (brd_entry->u.key.bit_brd.piece[S_KNIGHT | S_BLACK] == old_brd_entry->u.key.bit_brd.piece[S_KNIGHT | S_BLACK]) && 
        (brd_entry->u.key.bit_brd.piece[S_BISHOP | S_BLACK] == old_brd_entry->u.key.bit_brd.piece[S_BISHOP | S_BLACK]) && 
        (brd_entry->u.key.bit_brd.piece[S_ROOK | S_BLACK] == old_brd_entry->u.key.bit_brd.piece[S_ROOK | S_BLACK]) && 
        (brd_entry->u.key.bit_brd.piece[S_QUEEN | S_BLACK] == old_brd_entry->u.key.bit_brd.piece[S_QUEEN | S_BLACK]) && 
        (brd_entry->u.key.bit_brd.piece[S_KING | S_BLACK] == old_brd_entry->u.key.bit_brd.piece[S_KING | S_BLACK])) 
    {
      duplicate_detected = 1;

      /* We modify the next_board_index to point to the duplicate position.
      */
      *next_board_index = next_in_hash;

      brd_db->ply_table[brd_entry->u.key.ply_number].num_duplicates++;

      break;
    }  else
    {
      next_in_hash = old_brd_entry->next_in_hash;
      brd_db->hash_collisions++;
    }
  }

  if (0 == duplicate_detected)
  {
    brd_entry->next_in_hash = brd_db->db_hash[hash_index];
    brd_db->db_hash[hash_index] = *next_board_index; 
  } 


  return duplicate_detected;
}

/******************************************************************************
** Search the position and populate the database.
**
** brd_db - Board Database.
**
** Return Values:
** 0 - Database Initialized.
** -1 - Out of memory.
******************************************************************************/
static int ocBrdDbLoad (ocBrdDb_t *const brd_db,
                        const unsigned int onecore_brd_plies,
                        const unsigned long long onecore_db_size,
                        const unsigned long long onecore_move_db_size)
                        
{
  oneMove_t one_move[MAX_BRD_MOVES];


  for (unsigned int ply = 0; ply < onecore_brd_plies; ply++)
  {
    ocPlyInfo_t *ply_info = &brd_db->ply_table[ply];
    ocPlyInfo_t *next_ply_info = &brd_db->ply_table[ply + 1];

    next_ply_info->whose_move = ply_info->whose_move ^ 1;

    for (unsigned int i = 0; i < ply_info->num_boards_in_ply; i++)
    {
      const unsigned int board_index = ply_info->first_board_in_ply_index + i;
      ocBrdDbEntry_t *brd_entry = &brd_db->db_entry[board_index];

      const unsigned int num_moves = (unsigned int) 
                      allMoveCandidatesFind (ply_info->whose_move, 
                          &brd_entry->u.key.bit_brd, 
                          one_move,
                          brd_entry->u.key.en_passant_eligible_pawn, 
                          brd_entry->u.key.castle_eligibility);

      brd_entry->num_legal_moves = num_moves;
      brd_entry->legal_move_index = brd_db->next_free_move_index;
      brd_db->next_free_move_index += num_moves;

      /* If we run out of space in the move database then return an error.
      */
      if (brd_db->next_free_move_index >= onecore_move_db_size)
      {
        return -1;
      }
 
      if (0 == num_moves)
      {
        continue;
      }

      for (unsigned int j = 0; j < num_moves; j++)
      {
        unsigned int next_board_index = brd_db->next_free_index;
        ocBrdDbEntry_t *next_brd_entry = &brd_db->db_entry[next_board_index];
        unsigned int duplicate_found = 0;

        /* For now assume that we have a new unique position. We need to make a copy
        ** of the position anyway in order to apply the new move. 
        ** If the position turns out to be duplicate then we simply don't increment 
        ** the brd_db->next_free_index.
        */
        next_brd_entry->num_legal_moves = 0;
        next_brd_entry->legal_move_index = 0;
        next_brd_entry->next_in_hash = 0;
        next_brd_entry->u.key.castle_eligibility = one_move[j].castle_eligibility;
        next_brd_entry->u.key.en_passant_eligible_pawn = one_move[j].en_passant_eligible_pawn;
        memcpy (&next_brd_entry->u.key.bit_brd, &brd_entry->u.key.bit_brd, sizeof (bitBrd_t));
        next_brd_entry->u.key.ply_number = ply + 1;

        /* Make the move.
        */
        moveDo (ply_info->whose_move, &next_brd_entry->u.key.bit_brd, one_move[j]);

        /* Collisions start at ply 3. For lower plies we don't need to 
        ** check for duplicate database entries.
        */
        if (ply >= 2)
        {
          /* Check if there is a duplicate position. If there is a duplicate then 
          ** modify the next_board_index to point to the duplicate position.
          */
          duplicate_found = duplicateDetect (brd_db, next_brd_entry, &next_board_index);
        }

        if (0 == duplicate_found)
        {
          /* If this is the first position in the ply then set up the ply structure 
          ** to point to this entry.
          */
          if (0 == next_ply_info->first_board_in_ply_index)
          {
            next_ply_info->first_board_in_ply_index = next_board_index;
          }

          /* Increment the number of positions in the ply.
          */
          next_ply_info->num_boards_in_ply++;

          /* We need to insert the new position into the database.
          */
          brd_db->next_free_index++;

          /* If we exceeded the size of the database then return an error.
          */
          if (brd_db->next_free_index == onecore_db_size)
          {
            return -1;
          }
        }

        /* Set up the move pointer from the current entry to the 
        ** next entry.
        */
        brd_db->db_move[brd_entry->legal_move_index + j] = next_board_index;
      }

    }
  }

  return 0;
}
                

/******************************************************************************
** Populate the position database.
**
** brd_db - Board Database.
** brd - Initial Position
** info - Initial Position Info.
**
** Return Values:
** 0 - Database Initialized.
** -1 - Out of memory.
******************************************************************************/
static int ocBrdDbInit (
                ocBrdDb_t *const brd_db,
                const brd_t *const brd,
                const brdCtrlInfo_t *const info,
                const unsigned int onecore_brd_plies,
                const unsigned long long onecore_db_size,
                const unsigned long long onecore_move_db_size)
{
  const color_e whose_move = info->next_move;
  bitBrd_t input_bit_brd;
  castleEligibility_t castle_eligibility;
  unsigned int en_passant_eligible_pawn;

  searchSetup (brd, info, &input_bit_brd,
               &castle_eligibility, &en_passant_eligible_pawn);

  /* As long as we use mmap to allocate memory, these tables don't need
  ** to be cleared to 0 because mmap takes care of that. 
  ** If allocation is changed to malloc then the tables need to be cleared.
  */
  memset (brd_db->db_hash, 0, brd_db->onecore_hash_entries * sizeof (unsigned int));
  memset (brd_db->ply_table, 0, sizeof(brd_db->ply_table));
  brd_db->next_free_index = 0;
  brd_db->next_free_move_index = 0;
  brd_db->hash_collisions = 0;

  /* Add the root node to the board database.
  */
  brd_db->ply_table[0].whose_move = whose_move;
  brd_db->ply_table[0].num_boards_in_ply = 1;
  brd_db->ply_table[0].first_board_in_ply_index = brd_db->next_free_index;

  ocBrdDbEntry_t *root_node = &brd_db->db_entry[brd_db->next_free_index++];

  memcpy (&root_node->u.key.bit_brd, &input_bit_brd, sizeof(bitBrd_t));
  root_node->u.key.castle_eligibility = castle_eligibility;
  root_node->u.key.en_passant_eligible_pawn = en_passant_eligible_pawn;
  root_node->u.key.ply_number = 0;

  root_node->num_legal_moves = 0;
  root_node->legal_move_index = 0;


  return ocBrdDbLoad (brd_db, onecore_brd_plies,
                        onecore_db_size, onecore_move_db_size);
}

/******************************************************
*******************************************************
**
** API Functions
**
*******************************************************
******************************************************/

/********************************************************************
** Override scaling and operating mode for the scperft application.
**
** This function is not thread safe, and should only be called
** during initialization. 
**
** This function is not required to be called, and is used only when 
** default scaling parameters are not suitable or position caching 
** needs to be disabled.
**
** The caller is responsible for passing correct values. There is 
** no error checking of any king in this function.
**
** The caller may leave one or more databases at their default values
** by passing 0 as the new scaling value.
**
********************************************************************/
void onecoreScalingOverride (const unsigned int  brd_plies,
                             const unsigned long long db_entries,
                             const unsigned long long move_db_entries,
                             const unsigned long long hash_entries)
{
  max_onecore_brd_plies = brd_plies;
  override_onecore_db_entries = db_entries;
  override_onecore_move_db_entries = move_db_entries;
  override_hash_entries = hash_entries;
}

/********************************************************************
** Chess Move Selection Performance Test
** The function counts all legal moves from the starting position
** to the specified depth. The function prints out the total moves,
** the total moves from each move of the starting position, and
** the duration of the test. The console output can be suppressed
** by enabling the silent flag.
**
** depth - (input) Number of plies to search.
** brd - (input) 8x8 chess board.
** info - (input) Additional game description.
** silent - (input) - When non-zero, the function doesn't print 
**                    anything on the console, and simply returns 
**                    the move count.
**
** Return Value
** Number of legal positions discovered at depth.
**
********************************************************************/
unsigned long long onecorePerft (const unsigned int depth,
		   const brd_t *const brd, 
		   const brdCtrlInfo_t *const info,
           const unsigned int silent)
{
  /* The depth should be at least 5 in order to build the database.
  ** Otherwise the database makes the search slower. 
  */
  if ((depth < 5) || (0 == max_onecore_brd_plies))
                return bytebrdPerft (depth, brd, info, silent);

  const unsigned int onecore_brd_plies = (depth < 7)?3:
                                (max_onecore_brd_plies == 4)?4:3;

  ocBrdDb_t *brd_db;
  unsigned long long num_positions;
  unsigned long long start_of_test = 0;
  unsigned long long end_of_test, test_duration;
  unsigned int out_of_memory = 0;

  const unsigned long long brd_ratio = (MAX_ONECORE_BRD_MOVES * 2) / 3;
  unsigned long long onecore_db_size;
  unsigned long long onecore_move_db_size;
  

  if (onecore_brd_plies == 3)
  {
    onecore_db_size = MAX_ONECORE_BRD_MOVES * MAX_ONECORE_BRD_MOVES * brd_ratio;
    onecore_move_db_size = MAX_ONECORE_BRD_MOVES * MAX_ONECORE_BRD_MOVES * MAX_ONECORE_BRD_MOVES;
  } else
  {
    onecore_db_size = MAX_ONECORE_BRD_MOVES * MAX_ONECORE_BRD_MOVES * brd_ratio * brd_ratio;
    onecore_move_db_size = MAX_ONECORE_BRD_MOVES * MAX_ONECORE_BRD_MOVES * MAX_ONECORE_BRD_MOVES * MAX_ONECORE_BRD_MOVES;
  }

  /* If application want's to override the default values then do that.
  */
  if (override_onecore_db_entries)
  {
    onecore_db_size = override_onecore_db_entries;
  }
  if (override_onecore_move_db_entries)
  {
    onecore_move_db_size = override_onecore_move_db_entries;
  }

  const unsigned long long onecore_hash_entries = (0 == override_hash_entries)?
                                                        onecore_db_size / 2:
                                                        override_hash_entries;

  if (sizeof(ocBrdDb_t) % 8)
  {
    printf ("ERROR: Please pad the ocBrdDb_t to be multiple of 8.\n");
    exit (-1);
  }
  if (sizeof(ocBrdDbEntry_t) % 8)
  {
    printf ("ERROR: Please pad the ocBrdDbEntry_t to be multiple of 8.\n");
    exit (-1);
  }

  const unsigned long long memory_size = sizeof (ocBrdDb_t) + 
                      (onecore_db_size * sizeof (ocBrdDbEntry_t)) + 
                      (onecore_move_db_size * sizeof(unsigned int)) + 
                      (onecore_hash_entries * sizeof(unsigned int));
  if (0 == silent)
  {
    start_of_test = sysUpTimeMillisecondsGet();
  }

  brd_db = malloc(memory_size);

  brd_db->db_entry = (ocBrdDbEntry_t *) (((unsigned char *) brd_db) + sizeof(ocBrdDb_t));
  brd_db->db_move = (unsigned int *) (((unsigned char *) brd_db->db_entry) + (onecore_db_size * sizeof(ocBrdDbEntry_t)));
  brd_db->db_hash = (unsigned int *) (((unsigned char *) brd_db->db_move) + (onecore_move_db_size * sizeof(unsigned int)));

  brd_db->onecore_hash_entries = onecore_hash_entries; 
  brd_db->onecore_db_size = onecore_db_size;
  brd_db->onecore_move_db_size = onecore_move_db_size;


  if (0 == ocBrdDbInit (brd_db, brd, info, onecore_brd_plies,
                        onecore_db_size, onecore_move_db_size))
  {
    /* We successfully created the position table.
    ** Count moves using this table.
    */
    num_positions = positionSearch (depth, brd_db, silent, onecore_brd_plies);
  } else
  {
#if 0 // HACK
    printf ("Out of memory!\n");
    ocBrdDbShow (brd_db, onecore_brd_plies);
    brdutilBoardPrint (brd);
    exit (0);
#endif
    out_of_memory = 1;
    num_positions = bytebrdPerft (depth, brd, info, silent);
  }

#if 0 // HACK
  ocBrdDbShow (brd_db, onecore_brd_plies);
#endif

  free (brd_db);

  if (0 == silent)
  {
    if (0 == out_of_memory)
    {
      end_of_test = sysUpTimeMillisecondsGet();
      test_duration = end_of_test - start_of_test;

      printf ("Position Search Completed in %'llums\n", test_duration);
      printf ("Total positions at depth %u is %'llu\n", depth, num_positions);
      if (test_duration)
      {
        printf ("Position Table Algorithm processing rate %'llu positions per second.\n",
                 (num_positions / test_duration) * 1000);
      }
    } else
    {
      printf ("WARNING: Out of memory, didn't create the position database.\n");
      printf ("         Consider increasing MAX_ONECORE_BRD_MOVES.\n");
    }
  }

  return num_positions;
}

