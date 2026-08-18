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

#include "bytebrd_api.h"
#include "movegen.h"



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
                            const bitBrd_t *restrict bit_brd,
                            const unsigned long long per_move_total_at_depth)
{
   unsigned char promoted_pawn;
   constexpr char piece_name[] = {0,0,'n','b','r','q',0,0};

   (void) bit_brd;
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
   //bitbrdPrint(bit_brd);
}

/******************************************************************************
** Find all moves for the specified context and depth.
**
** board_db - Board Database.
**
** Return Values:
******************************************************************************/
__attribute__((noinline))
static unsigned long long movesRapidFind(
                const unsigned int depth,
                const color_e whose_move,
                const castleEligibility_t castle_eligibility,
                const unsigned int en_passant_eligible_pawn,
                bitBrd_t *restrict bit_brd)
{
  oneMove_t one_move[MAX_BRD_MOVES];
  const unsigned int num_moves = (unsigned int) allMoveCandidatesFind (whose_move, bit_brd, one_move,
                        en_passant_eligible_pawn, castle_eligibility);
  unsigned long long total_moves_at_depth = 0;
  unsigned long long total_moves_per_move;

  /* Special case processing when depth is 1.
  ** Print all available moves at depth 0 and exit.
  */
  for (unsigned int i = 0; i < num_moves; i++)
  {
    if (depth == 1)
    {
      moveResultsShow (&one_move[i], bit_brd, 1);
      total_moves_at_depth++;
      continue;
    }

    moveDo (whose_move, bit_brd, one_move[i]);
    total_moves_per_move = (depth == 2)?allMoveCandidatesLastPlyFindApi (whose_move ^ 1, bit_brd, 
                                            one_move[i].en_passant_eligible_pawn, 
                                            one_move[i].castle_eligibility):
                                        allMovePerft (whose_move ^ 1, 
                                                               bit_brd->piece, 
                                                               one_move[i].en_passant_eligible_pawn, 
                                                               one_move[i].castle_eligibility,
                                                               depth - 2,
                                                               0,
                                                               bit_brd->color[whose_move],
                                                               bit_brd->color[whose_move ^ 1]);
    moveResultsShow (&one_move[i], bit_brd, total_moves_per_move);
    total_moves_at_depth += total_moves_per_move;

    /* In preparation for the next move restore the position to what
    ** it was prior to the move.
    */
    moveUndo (whose_move, bit_brd, one_move[i]);
  }
  return  total_moves_at_depth;
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
                const brd_t *const brd, 
                const brdCtrlInfo_t *const info,
                const unsigned int silent)
{
  const unsigned long long start_of_test = sysUpTimeMillisecondsGet();
  unsigned long long end_of_test, test_duration;
  const color_e whose_move = info->next_move;
  unsigned long long total_moves_at_depth = 0;
  bitBrd_t input_bit_brd;
  castleEligibility_t castle_eligibility;
  unsigned int en_passant_eligible_pawn;

  searchSetup (brd, info, &input_bit_brd, 
               &castle_eligibility, &en_passant_eligible_pawn);

  if (silent)
  {
    return (depth == 1)?allMoveCandidatesLastPlyFindApi (whose_move, &input_bit_brd, 
                                            en_passant_eligible_pawn, castle_eligibility):
            allMovePerft (whose_move, input_bit_brd.piece, 
                        en_passant_eligible_pawn, castle_eligibility, depth - 1, 0,
                        input_bit_brd.color[whose_move ^ 1],   
                        input_bit_brd.color[whose_move]);      

  }

  total_moves_at_depth = movesRapidFind (depth, 
                                whose_move, 
                                castle_eligibility,
                                en_passant_eligible_pawn,
                                &input_bit_brd);

  end_of_test = sysUpTimeMillisecondsGet();

  test_duration = end_of_test - start_of_test;
  printf ("Position Search Completed in %'llums\n", test_duration);
  printf ("Total positions at depth %u is %'llu\n", depth, total_moves_at_depth);
  if (test_duration)
  {
    printf ("Algorithm processing rate %'llu positions per second.\n",
               (total_moves_at_depth / test_duration) * 1000);
  }

  return total_moves_at_depth;
}

/******************************************************
*******************************************************
**
** API Functions
**
*******************************************************
******************************************************/

/******************************************************************************
** Initialize the move generator.
** This function only needs to be called one time at the beginning.
** This function does NOT need to be called before every move.
**
** Return Values:
******************************************************************************/
void bytebrdInit(void)
{
  squareUnderAttackGenerate();
}



/********************************************************************
** Determine if the opponent is currently in check.
** This function is typically used to determine if the position is 
** a valid candidate for the next move search. If the opponent
** is in check then the position is NOT valid.
**
** brd - (input) 8x8 chess board.
** info - (input) Additional game description.
**
** Return Values
**  0 - Opponent is not in check.
**  1 - Opponent is in check.
**
********************************************************************/
unsigned int bytebrdUtilOpponentInCheck (
                   const brd_t *const brd,
                   const brdCtrlInfo_t *const info)
{
  const color_e whose_move = info->next_move;
  bitBrd_t input_bit_brd;
  castleEligibility_t castle_eligibility;
  unsigned int en_passant_eligible_pawn;
  unsigned int king_index;

  searchSetup (brd, info, &input_bit_brd, 
               &castle_eligibility, &en_passant_eligible_pawn);

  if (whose_move == MOVE_WHITE)
  {
    king_index = bitbrdLowestIndexFromMaskGet(input_bit_brd.piece[S_KING | S_BLACK]);
    return (kingInCheckApi(MOVE_BLACK, &input_bit_brd, king_index,
                            input_bit_brd.color[MOVE_BLACK],
                            input_bit_brd.color[MOVE_WHITE]))?1:0;
  } 

  king_index = bitbrdLowestIndexFromMaskGet(input_bit_brd.piece[S_KING | S_WHITE]);
  return (kingInCheckApi(MOVE_WHITE, &input_bit_brd, king_index,
                        input_bit_brd.color[MOVE_WHITE],
                        input_bit_brd.color[MOVE_BLACK]))?1:0;

}

/********************************************************************
** Utility function to apply a move the the current position.
** The position stored in the brd parameter is updated to reflect
** the move. Also the next_info flags are updated for the new 
** position.
**
** brd - (input/output) 8x8 chess board.
** move - (input) Move to make on the board.
** info - (input) Game description for the current position.
** next_info - (output) Game description after the move.
**
** Return Values
**  None
**
********************************************************************/
void bytebrdUtilMoveMake (
                   brd_t *const brd,
                   const bytebrdMove_t *const move,
                   const brdCtrlInfo_t *const info,
                   brdCtrlInfo_t *const next_info)
{
  unsigned char p1 = move->moved_piece;
  unsigned char p2 = move->captured_piece;

  /* Generate the next_info flags for the new position.
  */
  *next_info = *info;
  if (next_info->next_move == MOVE_WHITE)
  {
    /* White is moving.
    */
    next_info->next_move = MOVE_BLACK; // Next move is Black

    if (next_info->white_long_castle_eligible)
    {
      if ((p1 == (S_KING | S_WHITE)) ||
         ((p1 == (S_ROOK | S_WHITE)) &&
         (move->from_r == 0) && (move->from_c == 0)))
      {
        next_info->white_long_castle_eligible = 0;
      }
    }
    if (next_info->white_short_castle_eligible)
    {
      if ((p1 == (S_KING | S_WHITE)) ||
         ((p1 == (S_ROOK | S_WHITE)) &&
         (move->from_r == 0) && (move->from_c == 7)))
      {
        next_info->white_short_castle_eligible = 0;
      }
    }
    if (next_info->black_long_castle_eligible)
    {
      if ((p2 == (S_ROOK | S_BLACK)) &&
         (move->to_r == 7) && (move->to_c == 0))
      {
        next_info->black_long_castle_eligible = 0;
      }
    }
    if (next_info->black_short_castle_eligible)
    {
      if ((p2 == (S_ROOK | S_BLACK)) &&
         (move->to_r == 7) && (move->to_c == 7))
      {
        next_info->black_short_castle_eligible = 0;
      }
    }
  } else
  {
    /* Black is moving.
    */
    next_info->next_move = MOVE_WHITE; // Next move is White
    if (next_info->black_long_castle_eligible)
    {
      if ((p1 == (S_KING | S_BLACK)) ||
         ((p1 == (S_ROOK | S_BLACK)) &&
         (move->from_r == 7) && (move->from_c == 0)))
      {
        next_info->black_long_castle_eligible = 0;
      }
    }
    if (next_info->black_short_castle_eligible)
    {
      if ((p1 == (S_KING | S_BLACK)) ||
         ((p1 == (S_ROOK | S_BLACK)) &&
         (move->from_r == 7) && (move->from_c == 7)))
      {
        next_info->black_short_castle_eligible = 0;
      }
    }
    if (next_info->white_long_castle_eligible)
    {
      if ((p2 == (S_ROOK | S_WHITE)) &&
         (move->to_r == 0) && (move->to_c == 0))
      {
        next_info->white_long_castle_eligible = 0;
      }
    }
    if (next_info->white_short_castle_eligible)
    {
      if ((p2 == (S_ROOK | S_WHITE)) &&
         (move->to_r == 0) && (move->to_c == 7))
      {
        next_info->white_short_castle_eligible = 0;
      }
    }

  }

  /* Determine en passant settings.
  */
  next_info->en_passant_row = 0;
  next_info->en_passant_column = 0;
  next_info->en_passant_capture_eligible = 0;
  if ((PIECE_GET(move->moved_piece) == S_PAWN) &&
     (((move->to_r - move->from_r) == 2) ||
      ((move->from_r - move->to_r) == 2)))
  {
    if (((move->to_c > 0) &&
       (PIECE_GET(brd->rc[move->to_r][move->to_c - 1]) == S_PAWN) &&
       (move->moved_piece != brd->rc[move->to_r][move->to_c - 1])) ||
       ((move->to_c < (BRDS - 1)) &&
       (PIECE_GET(brd->rc[move->to_r][move->to_c + 1]) == S_PAWN) &&
        (move->moved_piece != brd->rc[move->to_r][move->to_c + 1])))
      {
          next_info->en_passant_row = move->to_r & 0x07;
          next_info->en_passant_column = move->to_c & 0x07;
          next_info->en_passant_capture_eligible = 1;
      }
  }

  /* Update the board with the move.
  */
  brd->rc[move->from_r][move->from_c] = 0;
  brd->rc[move->to_r][move->to_c] = move->moved_piece;

  if (move->en_passant_capture)
  {
    brd->rc[move->ep_r][move->ep_c] = 0;
  }

  if (move->promoted_piece)
  {
    brd->rc[move->to_r][move->to_c] = move->promoted_piece;;
  }

  if (move->short_castle)
  {
    brd->rc[move->from_r][move->from_c + 1] = brd->rc[move->from_r][7];
    brd->rc[move->from_r][7] = 0;
  }

  if (move->long_castle)
  {
    brd->rc[move->from_r][move->from_c - 1] = brd->rc[move->from_r][0];
    brd->rc[move->from_r][0] = 0;
  }

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
unsigned long long bytebrdPerft (const unsigned int depth,
		   const brd_t *const brd, 
		   const brdCtrlInfo_t *const info,
           const unsigned int silent)
{
  if (depth == 0)
                return 0;

  /* The input position is assumed to be valid.
  */

  return positionSearch (depth, brd, info, silent);

}


/********************************************************************
** Find all legal next moves from this position.
** The caller is responsible for allocating enough space in the
** move_list array for all moves. The maximum number of legal
** moves is MAX_BRD_MOVES.
**
** This function is intended to be used by applications that
** represent the chess board with the 8x8 byte array. Therefore
** the API is slow as compared to directly using the bit board.
**
** brd - (input) 8x8 chess board.
** info - (input) Additional game description.
** move_list - (output) - List of legal moves from this position.
** mover_lost - (output) - Set to one when there are 0 legal moves
**                         from this position and the mover is in check.
**
** Return Values
**  Number of legal moves from this position.
**  Note that if the number of moves from this position is 0
**  then the mover_lost flag indicates whether the mover is in
**  check mate or stale mate.
**
********************************************************************/
unsigned int bytebrdNextMoveGet (
                   const brd_t *const brd,
                   const brdCtrlInfo_t *const info,
                   bytebrdMove_t *const move_list,
                   unsigned int *const mover_lost)
{
  const color_e whose_move = info->next_move;
  unsigned int num_moves;
  bitBrd_t input_bit_brd;
  castleEligibility_t castle_eligibility;
  unsigned int en_passant_eligible_pawn;
  oneMove_t mv[MAX_BRD_MOVES];
  constexpr bytebrdMove_t zero_move = {};

  searchSetup (brd, info, &input_bit_brd, 
               &castle_eligibility, &en_passant_eligible_pawn);

  num_moves = (unsigned int) allMoveCandidatesFind (whose_move, &input_bit_brd, mv,
                        en_passant_eligible_pawn, 
                        castle_eligibility);

  if (0 == num_moves)
  {
    if (whose_move == MOVE_WHITE)
    {
      const unsigned int king_position = bitbrdLowestIndexFromMaskGet(input_bit_brd.piece[S_KING | S_WHITE]);
      *mover_lost = (unsigned int) kingInCheckApi(MOVE_WHITE, 
                       &input_bit_brd, king_position,
                       input_bit_brd.color[MOVE_WHITE], input_bit_brd.color[MOVE_BLACK]);
    } else
    {
      const unsigned int king_position = bitbrdLowestIndexFromMaskGet(input_bit_brd.piece[S_KING | S_BLACK]);
      *mover_lost = (unsigned int) kingInCheckApi(MOVE_BLACK,
                       &input_bit_brd, king_position,
                       input_bit_brd.color[MOVE_BLACK], input_bit_brd.color[MOVE_WHITE]);
    }
    return 0;
  }
  *mover_lost = 0;

  for (unsigned int i = 0; i < num_moves; i++)
  {
    move_list[i] = zero_move; /* Initialize all fields to 0 */

    move_list[i].from_r = mv[i].from_r;
    move_list[i].from_c = mv[i].from_c;
    move_list[i].to_r = mv[i].to_r;
    move_list[i].to_c = mv[i].to_c;
    move_list[i].moved_piece = mv[i].p1;

    /* Determine if this move is a capture. 
    ** Captured piece is always in p2 and is the 
    ** opposite color of p1.
    */
    if ((mv[i].num_masks > 1) && 
        (COLOR_GET(mv[i].p1) != COLOR_GET(mv[i].p2)))
    {
      move_list[i].captured_piece = mv[i].p2;
      /* Determine if this move is an en passant capture.
      */
      if ((PIECE_GET(mv[i].p1) == S_PAWN) && 
          (PIECE_GET(mv[i].p2) == S_PAWN) &&
          (mv[i].from_r == info->en_passant_row) &&
          (mv[i].to_c == info->en_passant_column)) 
      {
        move_list[i].en_passant_capture = 1;
        move_list[i].ep_r = info->en_passant_row;
        move_list[i].ep_c = info->en_passant_column;
      } else
      {
        /* This is a regular capture.
        */
        move_list[i].capture = 1;
      }
    }

    /* Determine if this move is a pawn promotion.
    ** The promoted piece may be stored in p2 or p3 if the move is also a capture.
    */
    if ((PIECE_GET(mv[i].p1) == S_PAWN) &&
        (((COLOR_GET(mv[i].p1) == S_WHITE) && (mv[i].to_r == (BRDS-1))) ||
         ((COLOR_GET(mv[i].p1) == S_BLACK) && (mv[i].to_r == 0))))
    {
      move_list[i].pawn_promotion = 1;
      if (mv[i].num_masks == 3)
                        move_list[i].promoted_piece = mv[i].p3;
            else
                        move_list[i].promoted_piece = mv[i].p2;
    }

    /* Determine if this move is a castle.
    */
    if ((mv[i].num_masks == 2) && (PIECE_GET(mv[i].p1) == S_KING))
    {
      if ((mv[i].to_c - mv[i].from_c) == 2)
      {
        move_list[i].short_castle = 1;
      } else if ((mv[i].from_c - mv[i].to_c) == 2)
      {
        move_list[i].long_castle = 1;
      }
    }

  }

  return num_moves;
}
