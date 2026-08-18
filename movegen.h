/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#ifndef MOVEGEN_H_INCLUDED
#define MOVEGEN_H_INCLUDED

#define _GNU_SOURCE

#if defined(__x86_64__)
#include <immintrin.h>
#else
#include <arm_acle.h> // Assume we are on ARM
#endif

#include "brdutil_api.h"

/* Help the optimizer create better code.
*/
#if 1
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) !!(x)
#define unlikely(x) !!(x)
#endif

#if defined(__BMI__)
#define USE_BMI
#endif

#if defined(__BMI2__)
#define USE_BMI2
#endif


/***********************************************
** Macros to manipulate the bit board.
***********************************************/

#define bitbrdIndexFromPositionGet(m_row, m_column) \
                ((unsigned int) ((m_row) << 3) | (m_column))


#define bitbrdMaskFromPositionGet(m_row,m_column)\
    ((unsigned long long) (1LLU << bitbrdIndexFromPositionGet(m_row, m_column)))

/* Return the index of the least significant bit set in the mask. 
** The returned value ranges from 0 to 63 when at least one bit in the mask is set.
** If no bits are set in the mask then the function returns 0xFFFFFFFF.
*/
#ifdef USE_BMI
#define bitbrdLowestIndexFromMaskGet(m_mask)\
        ((unsigned int) (_tzcnt_u64 ((unsigned long long) (m_mask))))
#else
#define bitbrdLowestIndexFromMaskGet(m_mask)\
        ((unsigned int) ((__builtin_ffsll ((long long) (m_mask))) - 1))
#endif

/* This function takes a piece with color flag and determines the color index.
** For example if the input is (S_PAWN | S_WHITE) then the output is MOVE_WHITE.
** Note that the input must be Piece | Color combination. The function won't work
** for unsigned characters with unexpected bits turned on.
*/
#define bitbrdColorIndexFromColorMaskGet(m_color_piece)\
        ((color_e) ((m_color_piece) >> 3))

/* Clear the bit board.
*/
#define bitbrdClear(m_bit_brd)\
  memset (m_bit_brd, 0, sizeof(bitBrd_t))

/* Add specified piece to the bit board. The piece must include the color mask.
*/
#define bitbrdPieceSet(m_bit_brd,m_piece,m_row,m_column)\
{ \
  const color_e m_color_index = bitbrdColorIndexFromColorMaskGet(m_piece);    \
  const unsigned long long m_mask = bitbrdMaskFromPositionGet (m_row, m_column);\
                                                                          \
  (m_bit_brd)->piece[m_piece] |= m_mask;                                          \
  (m_bit_brd)->color[m_color_index] |= m_mask;                                    \
}

/******************************************************************************
** Make move or undo the last move.
** Note that the code for making and undoing moves is exactly the same.
**
** whose_move - The color of last move.
** bit_brd - Board Bot Mask.
** one_move - The move to make or undo.
**
** Return Values:
**
******************************************************************************/
#define moveDoUndoMacro(m_whose_move,m_bit_brd,m_one_move)\
  (m_bit_brd)->piece[(m_one_move).p1] ^= (m_one_move).mask1;\
  (m_bit_brd)->color[m_whose_move] ^= (m_one_move).mask1;\
  if (unlikely((m_one_move).num_masks > 1))\
  {\
    (m_bit_brd)->piece[(m_one_move).p2] ^= (m_one_move).mask2;\
    (m_bit_brd)->color[bitbrdColorIndexFromColorMaskGet((m_one_move).p2)] ^= (m_one_move).mask2;\
    if (unlikely((m_one_move).num_masks > 2))\
    {\
      (m_bit_brd)->piece[(m_one_move).p3] ^= (m_one_move).mask3;\
      (m_bit_brd)->color[bitbrdColorIndexFromColorMaskGet((m_one_move).p3)] ^= (m_one_move).mask3;\
    }\
  }

#define moveDo moveDoUndoMacro
#define moveUndo moveDoUndoMacro



constexpr unsigned long long col7_mask = 0x8080808080808080;
constexpr unsigned long long col0_mask = 0x0101010101010101;

                            
typedef struct
{
  /* List of bit masks for each piece type on the board. The list is indexed by the 
  ** piece and color. For example piece[S_PAWN | S_WHITE] returns a bit mask of all white pawns
  ** on the board.
  */
  unsigned long long piece[16]; 

  /* The mask of all pieces of specified color on the board.
  */
  unsigned long long color[MOVE_COLORS];

} bitBrd_t __attribute__((aligned(32)));

/* These status flags are used to gather information about attack on the king
** and the squares to which the king can move.
*/
typedef struct
{
  unsigned long long pin;
  
  /* This flag is set when the mover king is in check from 
  ** two opponent pieces.
  ** This flag is also set when there are multiple pinned pieces.
  **
  ** The flag is used by the move generator to determine whether potential moves 
  ** need to be verified to make sure they are legal. The verification is needed
  ** only when the mover king is in double check or there are multiple pinned pieces.
  **
  ** Although a double check can be treated as a special case, because only the 
  ** king can move to avoid the double check, this is not done because there are 
  ** very few double checks relative to the number of positions. The extra testing
  ** for the double check negatively impacts performance.
  **
  ** The reason that move verification is not needed with only one check or one pin 
  ** is because the pin mask and the in_check mask can be used to eliminate the 
  ** illegal moves by and-ing these masks with the move candidate mask. 
  ** Note that having a check and a pin at the same time is not an issue and 
  ** doesn't trigger setting the move_test_needed flag.
  */
  unsigned int move_test_needed;

  unsigned long long in_check;
  unsigned long long move_candidate_mask;
} kingAttackHelper_t __attribute__((aligned(32)));

typedef struct
{
  unsigned long long knight_danger_mask;
  unsigned long long diagonal_danger_mask;
  unsigned long long udlr_danger_mask;
  unsigned long long pad;
} dangerMap_t __attribute__((aligned(32)));



/* Castle eligibility flags.
*/
typedef struct
{
  unsigned int white_short_ineligible:1;
  unsigned int black_short_ineligible:1;
  unsigned int white_long_ineligible:1;
  unsigned int black_long_ineligible:1;

} castleEligibility_t;

typedef struct
{
  unsigned long long mask1; /* First mask is always present */
  unsigned long long mask2; /* Second mask is for captures, castles, and pawn promotions. */
  unsigned long long mask3; /* Third mask is for concurrent captures and pawn promotions. */
  unsigned int num_masks;  /* Number of masks to apply on the move. 1 or 2. */
  unsigned char p1;         /* Piece which is moving */
  unsigned char p2; /* 0 or captured piece or rook when castling or promoted pawn value */
  unsigned char p3; /* 0 or promoted pawn when capture and promotion are done concurrently */

  /* The row/column information for the move.
  ** These fields are set by the move selection function.
  */
  unsigned int from_r, from_c, to_r, to_c;

  /* Castle Eligibility Flags for the next move.
  */
  castleEligibility_t castle_eligibility;

  /* En Passant Pawn for the next move.
  */
  unsigned int en_passant_eligible_pawn;

  unsigned long long pad;
} oneMove_t __attribute__((aligned(64)));

/******************************************************************************
** Create bit maps for attacking squares for all 64 board positions.
** These maps can be quickly compared to the bit mask of opponent
** pieces to see if the square is under attack.
** In case of knight, pawn, and king, this comparison is enough to decide
** whether the square is under attack. In case of bishop queen and rook,
** additional checking is needed if one of the opponent pieces is present
** in the attacking lane.
**
** Return Values:
******************************************************************************/
void squareUnderAttackGenerate(void);

/******************************************************************************
** Generate a list of all move candidates in the position.
**
**    whose_move (Input) The moving side.
**    brd - (Input) The current position.
**    bit_brd - (Input) The bit board of the current position.
**    dest - (Output) The list of moves.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**    castle_eligibility - (Input) - Flags indicating who is eligible to castle.
**
** Return Values:
** Total number of move candidates in this position. Note that this could be 0.
******************************************************************************/
unsigned long long allMoveCandidatesFind (
                           const color_e whose_move,
                           bitBrd_t *restrict bit_brd,
                           oneMove_t *const dest,
                           const unsigned int en_passant_eligible_pawn,
                           const castleEligibility_t castle_eligibility);


/******************************************************************************
** Generate a list of all white piece move candidates in the position.
**                      
**    bit_brd - (Input) The bit board of the current position.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**    castle_eligibility - (Input) - Flags indicating who is eligible to castle.
**           
** Return Values:
** Total number of move candidates in this position. Note that this could be 0.
******************************************************************************/
unsigned long long allWhiteMovePerft (
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const castleEligibility_t castle_eligibility,
                           const unsigned int depth,
                           const unsigned int ply,
                           const unsigned long long mover_pieces_mask,
                           const unsigned long long opponent_pieces_mask);

/******************************************************************************
** Generate a list of all black piece move candidates in the position.
**                      
**    bit_brd - (Input) The bit board of the current position.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**    castle_eligibility - (Input) - Flags indicating who is eligible to castle.
**           
** Return Values:
** Total number of move candidates in this position. Note that this could be 0.
******************************************************************************/
unsigned long long allBlackMovePerft (
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const castleEligibility_t castle_eligibility,
                           const unsigned int depth,
                           const unsigned int ply,
                           const unsigned long long mover_pieces_mask,
                           const unsigned long long opponent_pieces_mask);

/******************************************************************************
** Generate a list of all move candidates in the position.
**                      
**    whose_move (Input) The moving side.
**    bit_brd - (Input) The bit board of the current position.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**    castle_eligibility - (Input) - Flags indicating who is eligible to castle.
**           
** Return Values:
** Total number of move candidates in this position. Note that this could be 0.
******************************************************************************/
__attribute__((always_inline)) inline
unsigned long long allMovePerft (
                           const color_e whose_move,
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const castleEligibility_t castle_eligibility,
                           const unsigned int depth,
                           const unsigned int ply,
                           const unsigned long long opponent_pieces_mask,
                           const unsigned long long mover_pieces_mask)

{
  return (whose_move == MOVE_WHITE)?
                allWhiteMovePerft (piece, en_passant_eligible_pawn,
                                castle_eligibility, depth, ply,
                                mover_pieces_mask, opponent_pieces_mask):
                allBlackMovePerft (piece, en_passant_eligible_pawn,
                                castle_eligibility, depth, ply,
                                mover_pieces_mask, opponent_pieces_mask);
}


/******************************************************************************
** Determine if mover king is in check.
** This function is very similar to squareUnderAttack, except that it
** doesn't check for attack from opponent king. Ths is because a king
** cannot put opponent king in check.
**
**    whose_move - (Input) The attackers color is the opposite of whose_move.
**    bit_brd - (Input) The current position.
**    index - (Input) Location of this square.
**
** Return Values:
**    0 - Square is not under attack.
**    -1 - Square is under attack.
******************************************************************************/
int kingInCheckApi(const color_e whose_move,
                             const bitBrd_t *restrict bit_brd,
                             const unsigned int index,
                             const unsigned long long mover_pieces_mask,
                             const unsigned long long opponent_pieces_mask);


/******************************************************************************
** Generate a list of all move candidates in the position.
** This API function is used by external callers.
**
**    whose_move (Input) The moving side.
**    brd - (Input) The current position.
**    bit_brd - (Input) The bit board of the current position.
**    dest - (Output) The list of moves.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**    castle_eligibility - (Input) - Flags indicating who is eligible to castle.
**
** Return Values:
** Total number of move candidates in this position. Note that this could be 0.
******************************************************************************/
unsigned long long allMoveCandidatesLastPlyFindApi (
                           const color_e whose_move,
                           bitBrd_t *restrict bit_brd,
                           const unsigned int en_passant_eligible_pawn,
                           const castleEligibility_t castle_eligibility);


/******************************************************************************
** Display the bit board on the console.
** The function converts the bit board to board, and then calls the
** movegenUtilBoardPrint(brd) function.
**
** This function is ony used for debugging.
**
******************************************************************************/
void bitbrdPrint (const unsigned long long *const piece);


#endif /* MOVEGEN_H_INCLUDED */
