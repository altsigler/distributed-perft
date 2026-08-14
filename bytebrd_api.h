/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#ifndef BYTEBRD_API_H_INCLUDED
#define BYTEBRD_API_H_INCLUDED

#include "brdutil_api.h"

/* Structure for storing move information.
*/
typedef struct
{
  unsigned int from_r; /* From Row (0-7) */
  unsigned int from_c; /* From Column (0-7) */

  unsigned int to_r; /* To Row */
  unsigned int to_c; /* To Column */

  /* These flags identify special moves.
  ** Note that capture and pawn_promotion flags can be set at the same time.
  */
  unsigned int capture:1; /* 0-No Capture, 1-Capture */
  unsigned int en_passant_capture:1; /* En Passant Capture */
  unsigned int short_castle:1; /* This move is a king-side castle */
  unsigned int long_castle:1; /* This move is a queen-side castle */
  unsigned int pawn_promotion:1; /* This move is a pawn promotion */

  /* The piece information combines piece value and color. 
  ** For example S_PAWN | S_WHITE.
  */
  unsigned char moved_piece; /* Piece that moved */
  unsigned char captured_piece; /* If this move is a capture then this is the captured piece. */
  unsigned char promoted_piece; /* If this move is a pawn promotion then this is the promoted piece */

  /* If this move is an En Passant capture then this is the location of the 
  ** captured pawn.
  */
  unsigned int ep_r; 
  unsigned int ep_c;

} bytebrdMove_t;


/******************************************************************************
** Initialize the move generator.
** This function only needs to be called one time at the beginning.
** This function does NOT need to be called before every move.
**
** Return Values:
******************************************************************************/
void bytebrdInit(void);


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
** Return Values
**  Number of legal moves from this positio to specified depth.
**
********************************************************************/
unsigned long long bytebrdPerft (const unsigned int depth,
                   const brd_t *const brd,
                   const brdCtrlInfo_t *const info,
                   const unsigned int silent);

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
                   unsigned int *const mover_lost);


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
                   const brdCtrlInfo_t *const info);

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
                   brdCtrlInfo_t *const next_info);


#endif /* BYTEBRD_API_H_INCLUDED */
