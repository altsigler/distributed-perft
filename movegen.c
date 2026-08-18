/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#define _GNU_SOURCE

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <stdlib.h>
#include "brdutil_api.h"
#include "movegen.h"

static unsigned long long knightAttack[BRDS*BRDS];
static unsigned long long blackPawnAttack[BRDS*BRDS];
static unsigned long long blackPawnEnPassantAttack[BRDS*BRDS];
static unsigned long long whitePawnAttack[BRDS*BRDS];
static unsigned long long whitePawnEnPassantAttack[BRDS*BRDS];

static unsigned long long blackPawnCapture[BRDS*BRDS];

static unsigned long long whitePawnCapture[BRDS*BRDS];

static unsigned long long kingAttack[BRDS*BRDS];
static unsigned long long diagonalAttack[BRDS*BRDS]; /* Used for Bishop and Queen */
static unsigned long long udlrAttack[BRDS*BRDS]; /* Used for Rook and Queen */
static unsigned long long attackLanes[BRDS*BRDS][BRDS*BRDS];

/* In some code paths we need to look at all the attack vectors 
** for the same board position in the same function. In this scenario it is more 
** efficient to retrieve these masks from the same array of structures than 
** from different arrays of 64-bit integers because the index into the 
** array structure only needs to be computed one time.
*/
typedef struct 
{
  unsigned long long diagonalAttack;
  unsigned long long udlrAttack;
  unsigned long long whitePawnAttack;
  unsigned long long blackPawnAttack;
  unsigned long long knightAttack;
  unsigned long long kingAttack;
  unsigned long long pad1;
  unsigned long long pad2;
} aggregateAttack_t __attribute__((aligned(64)));;

aggregateAttack_t aggregateAttack[BRDS*BRDS];


/* These maps are used for shortcut move counting for bishop, rook, and queen.
*/

/* The maximum number of squares visible to bishop or queen on diagonals
** is 13. Therefore 2^13 = 8192 is how many possible combinations of visible
** pieces there are for each board position on diagonal files.
** For up/down/left/right the max squares is 14.
**
** When support for PEXT is not available then the hash table is set to 2^19.
** The 524,288 hash table is big, especially considering that we need a separate
** table for each square. Luckily the table is very sparse, so the use of physical
** memory is not as big as the virtual allocation.
** The reason we need this large table is that our hash function leverages the
** hardware CRC computation, which needs the large table to avoid hash collisions.
*/
#ifdef USE_BMI2
#define MAX_HASH_INDEX (1 << 14)
#else
#define MAX_HASH_INDEX (1 << 19)
#endif
static unsigned long long *diagonalVisibilityMap[BRDS*BRDS];
static unsigned long long *udlrVisibilityMap[BRDS*BRDS];

/* This table is used for testing whether the king is in check and to which
** squares the king can move.
** The "danger" table represents squares from which opponent pieces can
** attack one of the squares to which the king can move.
** There are three different "danger" vectors. One used for knights, one used for
** bishops, one used for rooks, and one used for queens.
** The opponent pawns and king don't use the "danger" table because it is quicker to simply
** test for attack from those pieces.
**
** The "danger" table is organized as an array indexed by the mover king position.
** Note that this table finds dangerous squares for all squares to which the king
** is capable of moving, including squares on which another mover color piece is
** already located.
*/
static dangerMap_t dangerMap[BRDS*BRDS];


/******************************************************************************
** Free memory allocated for tables when the program exits.
******************************************************************************/
static void cleanup(void)
{
  for (unsigned int i = 0; i < BRDS*BRDS; i++)
  {
    if (diagonalVisibilityMap[i])
                        free (diagonalVisibilityMap[i]);
    if (udlrVisibilityMap[i])
                        free (udlrVisibilityMap[i]);
  }
}



#define POSITION_TO_BITMASK_LOOKUP(m_row,m_col)\
                    bitbrdMaskFromPositionGet((unsigned int)(m_row),(unsigned int)(m_col)) 

#define POSIDX(m_row, m_column) (((m_row) << 3) | (m_column))

/* This is a shortcut macro for testing special case when 
** position is in check or a piece is pinned.
** The code using this macro must define and set the 
** pin, in_check, and from_mask local variables.
*/
#define TEST_NEEDED ((pin && (from_mask & pin)) || in_check)

__attribute__((always_inline)) inline
static unsigned long long allMoveCandidatesLastPlyFind (
                           const color_e whose_move,
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const castleEligibility_t castle_eligibility,
                           const unsigned long long opponent_pieces_mask,
                           const unsigned long long mover_pieces_mask);

      
/* Function for making the move. The same function reverses the move.
*/  
inline static void __attribute__((always_inline))
MOVE_APPLY (
                        unsigned long long *restrict piece, 
                        const unsigned int num_masks,
                        const unsigned char p1,
                        const unsigned char p2,
                        const unsigned char p3,
                        const unsigned long long mask1,
                        const unsigned long long mask2,
                        const unsigned long long mask3)
{       
  piece[p1] ^= mask1;
  if (num_masks > 1)
  {
    piece[p2] ^= mask2;
    if (unlikely(num_masks > 2))
    {
      piece[p3] ^= mask3;
    }
  }
}


/******************************************************************************
** Compute lookup key for 64 bit integer.
**
** Return Values:
** 16-bit hash index
******************************************************************************/
inline static unsigned long long __attribute__((always_inline))
lookupKeyCompute (const unsigned long long val, const unsigned long long mask)
{
#if defined(USE_BMI2)
    return  _pext_u64 (val, mask);
#elif defined(__SSE4_1__)
    const unsigned int crc = (unsigned int) __builtin_ia32_crc32di(0, val & mask);
    return (((crc ^ (crc >> 16)) << 3) | (crc & 0x7)) & (MAX_HASH_INDEX - 1);
#elif defined(__ARM_FEATURE_CRC32)
    const unsigned int crc = (unsigned int) __crc32cd(0, val & mask);
    return (((crc ^ (crc >> 16)) << 3) | (crc & 0x7)) & (MAX_HASH_INDEX - 1);
#else
#error Missing hardware support for PEXT and CRC32.
#endif

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
inline static int __attribute__((always_inline))
kingInCheck(const color_e whose_move,
                             const unsigned long long *restrict piece,
                             const unsigned int index,
                             const unsigned long long any_color_pieces_mask)
{
  unsigned long long attack_bishop_or_queen;
  unsigned long long attack_rook_or_queen;

  if (whose_move == MOVE_WHITE)
  {
    if (unlikely((piece[S_BLACK | S_KNIGHT] & knightAttack[index]) ||
        (piece[S_BLACK | S_PAWN] & blackPawnAttack[index])))
                return -1;

    attack_bishop_or_queen = piece[S_BLACK | S_BISHOP] | piece[S_BLACK | S_QUEEN];
    attack_rook_or_queen = piece[S_BLACK | S_ROOK] | piece[S_BLACK | S_QUEEN];
  } else 
  {
    if (unlikely((piece[S_WHITE | S_KNIGHT] & knightAttack[index]) ||
       (piece[S_WHITE | S_PAWN] & whitePawnAttack[index])))
                return -1;

    attack_bishop_or_queen = piece[S_WHITE | S_BISHOP] | piece[S_WHITE | S_QUEEN];
    attack_rook_or_queen = piece[S_WHITE | S_ROOK] | piece[S_WHITE | S_QUEEN];
  } 

  const unsigned long long udlr_attack = udlrAttack[index];
  if (attack_rook_or_queen & udlr_attack)
  {
    const unsigned long long udlr_lookup_key =
                     lookupKeyCompute (any_color_pieces_mask, udlr_attack);
    const unsigned long long udlr_visibility_mask = udlrVisibilityMap[index][udlr_lookup_key];
    if (udlr_visibility_mask & attack_rook_or_queen)
                            return -1;
  }

  const unsigned long long diagonal_attack = diagonalAttack[index];
  if (attack_bishop_or_queen & diagonal_attack)
  {
    const unsigned long long diagonal_lookup_key =
                     lookupKeyCompute (any_color_pieces_mask, diagonal_attack);
    const unsigned long long visibility_mask = diagonalVisibilityMap[index][diagonal_lookup_key];
    if (visibility_mask & attack_bishop_or_queen)
                            return -1;
  }

  return 0;
}
int kingInCheckApi(const color_e whose_move,
                             const bitBrd_t *restrict bit_brd,
                             const unsigned int index,
                             const unsigned long long mover_pieces_mask,
                             const unsigned long long opponent_pieces_mask)

{
  return kingInCheck(whose_move, bit_brd->piece, index, mover_pieces_mask | opponent_pieces_mask);
}

/******************************************************************************
** Determine which piece is located at the specified bit mask.
** This function is called in case of captures, so it is known` for a fact 
** that there is a piece at the specified position.
**
** Return Values:
**
******************************************************************************/
__attribute__((always_inline)) inline
static unsigned char pieceTypeGet (const color_e whose_move,
                                    const unsigned long long mask2,
                                    const unsigned long long *restrict piece)
{
  unsigned char p2;

  if (whose_move == MOVE_WHITE)
  {
    if (mask2 & piece[S_PAWN | S_BLACK])
    {
      return S_PAWN | S_BLACK;
    }
    for (p2 = S_KNIGHT | S_BLACK; p2 < (S_KING | S_BLACK); p2++)
    {
      if (mask2 & piece[p2])
                              break;
    }
  } else 
  {
    if (mask2 & piece[S_PAWN | S_WHITE])
    {
      return S_PAWN | S_WHITE;
    }
    for (p2 = S_KNIGHT | S_WHITE; p2 < (S_KING | S_WHITE); p2++)
    {
      if (mask2 & piece[p2])
                              break;
    }
  } 

  return p2;
}

/******************************************************************************
** This function updates castle eligibility template for a rook.
** The function is called only when a rook is moved.
**
** Return Values:
**
******************************************************************************/
__attribute__((always_inline)) inline
static void castleEligibilityRookTemplateSet (const color_e whose_move,
                            const unsigned int from_index,
                            castleEligibility_t *const next_castle_eligibility)
{
  constexpr unsigned int white_rook_queen_side_index = 0;
  constexpr unsigned int white_rook_king_side_index = 7;
  constexpr unsigned int black_rook_queen_side_index = 7 << 3;
  constexpr unsigned int black_rook_king_side_index = (7 << 3) | 7;

  /* Determine if this move made future castling ineligible.
  */
  if (whose_move == MOVE_WHITE)
  {
      if (from_index == white_rook_queen_side_index)
      {
        next_castle_eligibility->white_long_ineligible = 1;
      } else
      if (from_index == white_rook_king_side_index)
      {
        next_castle_eligibility->white_short_ineligible = 1;
      }
  } else 
  {
      if (from_index == black_rook_queen_side_index)
      {
        next_castle_eligibility->black_long_ineligible = 1;
      } else
      if (from_index == black_rook_king_side_index)
      {
        next_castle_eligibility->black_short_ineligible = 1;
      }
  }
}

/******************************************************************************
** Handle Castle Eligibility Check for Bishop, Queen, and Knight.
** The caller must check that the move is capturing a ROOK before calling
** this function.
**
** Return Values:
**
******************************************************************************/
__attribute__((always_inline)) inline
//__attribute__((noinline)) 
static void castleEligibilityRookCaptureCheck (const color_e whose_move,
                            const unsigned int to_index,
                            castleEligibility_t *next_castle_eligibility)
{

  if (whose_move == MOVE_WHITE)
  {
    constexpr unsigned int black_rook_queen_side_index = 7 << 3;
    constexpr unsigned int black_rook_king_side_index = (7 << 3) | 7;

    if (!next_castle_eligibility->black_long_ineligible &&
         (to_index == black_rook_queen_side_index))
                next_castle_eligibility->black_long_ineligible = 1;
    if (!next_castle_eligibility->black_short_ineligible &&
         (to_index == black_rook_king_side_index))
                next_castle_eligibility->black_short_ineligible = 1;
  } else
  {
    constexpr unsigned int white_rook_queen_side_index = 0;
    constexpr unsigned int white_rook_king_side_index = 7;

    if (!next_castle_eligibility->white_long_ineligible &&
         (to_index == white_rook_queen_side_index))
                next_castle_eligibility->white_long_ineligible = 1;
    if (!next_castle_eligibility->white_short_ineligible &&
         (to_index == white_rook_king_side_index))
                next_castle_eligibility->white_short_ineligible = 1;
  } 
}


/******************************************************************************
** Check if there is a pin between the mover king and attacker.
******************************************************************************/
__attribute__((always_inline)) inline
static void pinUpdate (unsigned long long *restrict pin,
                       unsigned int *restrict move_test_needed,
                        const color_e whose_move,
                        const unsigned long long *restrict piece,
                        const unsigned int position_index,
                        const unsigned int attacker_index,
                        const unsigned int en_passant_eligible_pawn,
                        const unsigned long long mover_pieces_mask,
                        const unsigned long long any_color_pieces_mask)
{
  const unsigned long long attack_lane = attackLanes[position_index][attacker_index];
  const unsigned long long detected_pieces = attack_lane & any_color_pieces_mask;

  /* The piece is pinned if it is the only piece between the king
  ** and the attacker and the piece is the same color as the king.
  */
  const unsigned long long mover_detected_pieces = detected_pieces & mover_pieces_mask;

  if (mover_detected_pieces)
  {
    const unsigned int num_pinned_pieces = (unsigned int) __builtin_popcountll (detected_pieces);
    if (1 == num_pinned_pieces)
    {
      if (*pin)
      {
        *move_test_needed = 1;
      }
      *pin |= (attack_lane | (1LLU << attacker_index));
    } else if ((2 == num_pinned_pieces) &&
            en_passant_eligible_pawn &&
            ((1LLU << en_passant_eligible_pawn) & detected_pieces) &&
            (mover_detected_pieces & piece[(whose_move << 3) | S_PAWN]) &&
            ((en_passant_eligible_pawn >> 3) ==
                    (bitbrdLowestIndexFromMaskGet(mover_detected_pieces) >> 3)))
    {
      /* There is a special case when en-passant eligibile pawn and mover
      ** pawn are pinned together. The en passant capture removes two pawns
      ** from the same row, potentially exposing the king.
      */
      *move_test_needed = 1;
      *pin |= (attack_lane | (1LLU << attacker_index));
    }
  }
}


/******************************************************************************
** Determine if specified square is under attack.
**
**    my_color - (Input) The attackers color is the opposite of my_color.
**    bit_brd - (Input) The current position.
**    index - (Input) Location of this square.
**
** Return Values:
**    0 - Square is not under attack.
**    -1 - Square is under attack.
******************************************************************************/
inline __attribute__((always_inline))
//__attribute__((noinline))
static int squareUnderAttack(
                             const unsigned long long attack_bishop_or_queen,
                             const unsigned long long attack_rook_or_queen,
                             const unsigned long long any_color_pieces_mask,
                             const unsigned long long attack_knight,
                             const unsigned int index)
{

  const aggregateAttack_t *const aggregateAttackVal = &aggregateAttack[index];
  if (attack_bishop_or_queen & aggregateAttackVal->diagonalAttack)
  {
    const unsigned long long diagonal_lookup_key =
                     lookupKeyCompute (any_color_pieces_mask, aggregateAttackVal->diagonalAttack);
    const unsigned long long visibility_mask = diagonalVisibilityMap[index][diagonal_lookup_key];
    if (visibility_mask & attack_bishop_or_queen)
                            return -1;
  }

  if (unlikely((attack_knight & aggregateAttackVal->knightAttack)))
                return -1;

  if (attack_rook_or_queen & aggregateAttackVal->udlrAttack)
  {
    const unsigned long long udlr_lookup_key =
                     lookupKeyCompute (any_color_pieces_mask, aggregateAttackVal->udlrAttack);
    const unsigned long long udlr_visibility_mask = udlrVisibilityMap[index][udlr_lookup_key];
    if (udlr_visibility_mask & attack_rook_or_queen)
                            return -1;
  }

  return 0;
}
/******************************************************************************
** Determine if the mover king is in check.
** Determine legal moves that the king can take.
** Determine which pieces are pinned.
******************************************************************************/
__attribute__((always_inline)) inline
//__attribute__((noinline)) 
static kingAttackHelper_t pinCompute (
                                      const color_e whose_move,
                                      const unsigned long long *restrict piece,
                                      const unsigned int position_index,
                                      const unsigned long long mover_king_mask,
                                      const unsigned int en_passant_eligible_pawn,
                                      const unsigned long long mover_pieces_mask,
                                      unsigned long long any_color_pieces_mask,
                                      unsigned long long king_attack_mask,
                                      const castleEligibility_t castle_eligibility)
{
  unsigned long long attack_pawn;
  unsigned long long attack_king;
  unsigned long long attack_mask;
  const unsigned long long attack_knight = piece[((whose_move ^ 1) << 3) | S_KNIGHT];
  const unsigned long long attack_queen = piece[((whose_move ^ 1) << 3) | S_QUEEN];
  const unsigned long long attack_bishop_or_queen = piece[((whose_move ^ 1) << 3) | S_BISHOP] | attack_queen;
  const unsigned long long attack_rook_or_queen = piece[((whose_move ^ 1) << 3) | S_ROOK] | attack_queen;
  kingAttackHelper_t attack_helper = {};
  const aggregateAttack_t *const aggregateAttackVal = &aggregateAttack[position_index];

  attack_helper.move_candidate_mask = king_attack_mask & 
                                ~mover_pieces_mask; 


  /* Special case when king can't make any moves, not in check,
  ** and doesn't have any pins.
  ** When running perft 7 on the standard staring position, the code
  ** below handles about one quarter of the positions.
  */
  if ((0 == attack_helper.move_candidate_mask) &&
      (0 == (attack_knight & aggregateAttackVal->knightAttack)))
  { 

    const unsigned long long attacker_mask =
              (aggregateAttackVal->diagonalAttack & attack_bishop_or_queen) |
              (aggregateAttackVal->udlrAttack & attack_rook_or_queen);

    if (likely(!attacker_mask))
    {
      return attack_helper;
    }
  }

  // Pawn Attack
  if (whose_move == MOVE_WHITE)
  {
    attack_pawn = piece[S_BLACK | S_PAWN];
    attack_mask = ((attack_pawn & ~col0_mask) >> 9) |
                         ((attack_pawn & ~col7_mask) >> 7);
    if (unlikely(attack_mask & mover_king_mask))
    {
      attack_helper.in_check = 
                    (attack_pawn & aggregateAttackVal->blackPawnAttack);
    }
  } else
  {
    attack_pawn = piece[S_WHITE | S_PAWN];
    attack_mask = ((attack_pawn & ~col0_mask) << 7) |
                        ((attack_pawn & ~col7_mask) << 9);
    if (unlikely(attack_mask & mover_king_mask))
    {
      attack_helper.in_check = 
                    (attack_pawn & aggregateAttackVal->whitePawnAttack);
    }
  }

  // King Attack
  attack_king = piece[(whose_move ^ 1) << 3 | S_KING];
  const unsigned int king_index = bitbrdLowestIndexFromMaskGet(attack_king);
  attack_mask |= kingAttack[king_index];

  const dangerMap_t danger_map = dangerMap[position_index];
                   


  // Knight Attack
  unsigned long long opponent_knight_mask = attack_knight & 
                                                danger_map.knight_danger_mask;
  while (unlikely(opponent_knight_mask))
  {
    const unsigned int index = bitbrdLowestIndexFromMaskGet(opponent_knight_mask);
#if defined(USE_BMI)
    opponent_knight_mask = _blsr_u64(opponent_knight_mask);
#else
    opponent_knight_mask ^= 1LLU << index;
#endif
  
    const unsigned long long knight_attack = knightAttack[index];
    attack_mask |= knight_attack;

    if (knight_attack & mover_king_mask)
    {               
      attack_helper.in_check |= (1LLU << index);
    }               
  }

    // Bishop/Queen Attack
  any_color_pieces_mask ^= mover_king_mask;

  unsigned long long opponent_bishop_mask = attack_bishop_or_queen & 
                                            danger_map.diagonal_danger_mask;
  while (opponent_bishop_mask)
  {
    const unsigned int index = bitbrdLowestIndexFromMaskGet(opponent_bishop_mask);
#if defined(USE_BMI)
    opponent_bishop_mask = _blsr_u64(opponent_bishop_mask);
#else
    opponent_bishop_mask ^= 1LLU << index;
#endif

    const unsigned long long diagonal_attack = diagonalAttack[index];
    const unsigned long long diagonal_lookup_key =
                   lookupKeyCompute (any_color_pieces_mask, diagonal_attack);
    const unsigned long long visibility_mask = diagonalVisibilityMap[index][diagonal_lookup_key];

    attack_mask |= visibility_mask;

    /* If the mover king is in the attack lanes of this piece then check if there 
    ** is a pin.
    */
    if (mover_king_mask & diagonal_attack)
    {
      if (mover_king_mask & visibility_mask)
      {
        if (attack_helper.in_check)
        {
          attack_helper.move_test_needed = 1;
        }
        attack_helper.in_check |= (attackLanes[position_index][index] | (1LLU << index));
      } else
      {
        pinUpdate (&attack_helper.pin, &attack_helper.move_test_needed, 
                    whose_move, piece,  
                    position_index, index, en_passant_eligible_pawn,
                    mover_pieces_mask, any_color_pieces_mask);
      }
    }
  }

  // Rook/Queen Attack
  unsigned long long opponent_rook_mask = attack_rook_or_queen &
                                            danger_map.udlr_danger_mask;
  while (unlikely(opponent_rook_mask))
  {
    const unsigned int index = bitbrdLowestIndexFromMaskGet(opponent_rook_mask);
#if defined(USE_BMI)
    opponent_rook_mask = _blsr_u64(opponent_rook_mask);
#else
    opponent_rook_mask ^= 1LLU << index;
#endif
  
    const unsigned long long udlr_attack = udlrAttack[index];
    const unsigned long long udlr_lookup_key =
                     lookupKeyCompute (any_color_pieces_mask, udlr_attack);
    const unsigned long long visibility_mask = udlrVisibilityMap[index][udlr_lookup_key];

    attack_mask |= visibility_mask;

    /* If the mover king is in the attack lanes of this piece then check if there 
    ** is a pin.
    */
    if (mover_king_mask & udlr_attack)
    {
      if (mover_king_mask & visibility_mask)
      {
        if (attack_helper.in_check)
        {
          attack_helper.move_test_needed = 1;
        }
        attack_helper.in_check |= (attackLanes[position_index][index] | (1LLU << index));
      } else
      {
        pinUpdate (&attack_helper.pin, &attack_helper.move_test_needed,
                    whose_move, piece,
                    position_index, index, en_passant_eligible_pawn,
                    mover_pieces_mask, any_color_pieces_mask);
      }
    }
  }
  
  attack_helper.move_candidate_mask &= ~attack_mask;


  /* Check whether castling moves need be added to the move_candidate_ask.
  */
  if (whose_move == MOVE_WHITE)
  {
    /* Check King Side Castle.
    */
    constexpr unsigned long long white_short_castle_mask =
                                          POSITION_TO_BITMASK_LOOKUP(0, 5) |
                                          POSITION_TO_BITMASK_LOOKUP(0, 6);
    constexpr unsigned long long white_short_move_mask =
                                          POSITION_TO_BITMASK_LOOKUP(0, 4) |
                                          POSITION_TO_BITMASK_LOOKUP(0, 5) |
                                          POSITION_TO_BITMASK_LOOKUP(0, 6);
  
    constexpr unsigned long long white_long_castle_mask =
                                          POSITION_TO_BITMASK_LOOKUP(0, 3) |
                                          POSITION_TO_BITMASK_LOOKUP(0, 2) |
                                          POSITION_TO_BITMASK_LOOKUP(0, 1);
    constexpr unsigned long long white_long_move_mask =
                                          POSITION_TO_BITMASK_LOOKUP(0, 4) |
                                          POSITION_TO_BITMASK_LOOKUP(0, 3) |
                                          POSITION_TO_BITMASK_LOOKUP(0, 2);
  
    if (!castle_eligibility.white_short_ineligible)
    {
      if ((0 == (any_color_pieces_mask & white_short_castle_mask)) &&
          (0 == (attack_mask & white_short_move_mask)) &&
          (0 == squareUnderAttack(attack_bishop_or_queen, attack_rook_or_queen,
                                        any_color_pieces_mask, 
                                        attack_knight, POSIDX(0, 6))))
      {
        attack_helper.move_candidate_mask |= POSITION_TO_BITMASK_LOOKUP(0, 6);
      }
    }
  
  
    /* Check Queen Side Castle.
    */
    if (!castle_eligibility.white_long_ineligible)
    {
      if ((0 == (any_color_pieces_mask & white_long_castle_mask)) &&
          (0 == (attack_mask & white_long_move_mask)) &&
          (0 == squareUnderAttack(attack_bishop_or_queen, attack_rook_or_queen,
                                    any_color_pieces_mask, 
                                    attack_knight, POSIDX(0, 2))))
      {
        attack_helper.move_candidate_mask |= POSITION_TO_BITMASK_LOOKUP(0, 2);
      }
    }
  } else
  {
    /* Check King Side Castle.
    */
    constexpr unsigned long long black_short_castle_mask =
                                          POSITION_TO_BITMASK_LOOKUP(7, 5) |
                                          POSITION_TO_BITMASK_LOOKUP(7, 6);
    constexpr unsigned long long black_short_move_mask =
                                          POSITION_TO_BITMASK_LOOKUP(7, 4) |
                                          POSITION_TO_BITMASK_LOOKUP(7, 5) |
                                          POSITION_TO_BITMASK_LOOKUP(7, 6);
  
    constexpr unsigned long long black_long_castle_mask =
                                          POSITION_TO_BITMASK_LOOKUP(7, 3) |
                                          POSITION_TO_BITMASK_LOOKUP(7, 2) |
                                          POSITION_TO_BITMASK_LOOKUP(7, 1);
    constexpr unsigned long long black_long_move_mask =
                                          POSITION_TO_BITMASK_LOOKUP(7, 4) |
                                          POSITION_TO_BITMASK_LOOKUP(7, 3) |
                                          POSITION_TO_BITMASK_LOOKUP(7, 2);
  
    if (!castle_eligibility.black_short_ineligible)
    {
      if ((0 == (any_color_pieces_mask & black_short_castle_mask)) &&
          (0 == (attack_mask & black_short_move_mask)) &&
          (0 == squareUnderAttack(attack_bishop_or_queen, attack_rook_or_queen,
                                    any_color_pieces_mask, 
                                    attack_knight, POSIDX(7, 6))))
      {
        attack_helper.move_candidate_mask |= POSITION_TO_BITMASK_LOOKUP(7, 6);
      }
    }
  
  
    /* Check Queen Side Castle.
    */
    if (!castle_eligibility.black_long_ineligible)
    {
      if ((0 == (any_color_pieces_mask & black_long_castle_mask)) &&
          (0 == (attack_mask & black_long_move_mask)) &&
          (0 == squareUnderAttack(attack_bishop_or_queen, attack_rook_or_queen,
                                    any_color_pieces_mask, 
                                    attack_knight, POSIDX(7, 2))))
      {
        attack_helper.move_candidate_mask |= POSITION_TO_BITMASK_LOOKUP(7, 2);
      }
    }
  
  }

  return attack_helper;
}


/******************************************************************************
** Find all eligible squares to which the White king can move.
**
**    brd - (Input) The current position.
**    next_mv - (Input/Output) The list of all possible moves.
**    castle_eligibility - (Input) - Flags indicating who is eligible to castle.
**
** Return Values:
**      None.
******************************************************************************/
__attribute__((always_inline)) inline 
//__attribute__((noinline)) inline 
static unsigned long long kingSquaresLastPlyFind (
                            unsigned long long move_candidate_mask)
{
  unsigned long long mn = (unsigned long long) __builtin_popcountll (move_candidate_mask);

  return mn;
}

/******************************************************************************
** Find all eligible square to which the Bishop, Rook, and Queen can move.
** This variant of the move counting function handles Bishop/Rook/Queen/Knight
** when a single pin is detected. 
** The position cannot have any checks or multiple pins.
**
** Return Values:
******************************************************************************/
__attribute__((always_inline)) inline
//__attribute__((noinline)) 
static unsigned long long allBRQNSquaresLastPlyFindPinned (
                                   const color_e whose_move,
                                   const unsigned long long pin,
                                   const unsigned long long *restrict piece,
                                   const unsigned long long mover_pieces_mask,
                                   const unsigned long long any_color_pieces_mask)
{
  unsigned long long mn = 0;
  unsigned long long knight_piece_mask = piece[S_KNIGHT | (whose_move << 3)];
  unsigned long long bishop_piece_mask = piece[S_BISHOP | (whose_move << 3)];
  unsigned long long rook_piece_mask = piece[S_ROOK | (whose_move << 3)];
  unsigned long long queen_piece_mask = piece[S_QUEEN | (whose_move << 3)];
    

  while (bishop_piece_mask)
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(bishop_piece_mask);
#if defined(USE_BMI)
    const unsigned long long piece_mask = _blsi_u64(bishop_piece_mask);
    bishop_piece_mask = _blsr_u64(bishop_piece_mask);
#else
    const unsigned long long piece_mask = 1LLU << piece_index;
    bishop_piece_mask ^= piece_mask;
#endif


    const unsigned long long lookup_index = 
                   lookupKeyCompute (any_color_pieces_mask, diagonalAttack[piece_index]);

    unsigned long long open_and_visible_mask = diagonalVisibilityMap[piece_index][lookup_index] &
                                                      ~mover_pieces_mask;

    if (pin & piece_mask)
    {
      open_and_visible_mask &= pin;  
    }

    /* Add all open and visible opponent squares to the move count.
    */
    mn += (unsigned long long) __builtin_popcountll (open_and_visible_mask);

  }

  while (rook_piece_mask)
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(rook_piece_mask);
#if defined(USE_BMI)
    const unsigned long long piece_mask = _blsi_u64(rook_piece_mask);
    rook_piece_mask = _blsr_u64(rook_piece_mask);
#else
    const unsigned long long piece_mask = 1LLU << piece_index;
    rook_piece_mask ^= piece_mask; 
#endif

    const unsigned long long lookup_index = 
                   lookupKeyCompute (any_color_pieces_mask, udlrAttack[piece_index]);

    unsigned long long open_and_visible_mask = udlrVisibilityMap[piece_index][lookup_index] &
                                                      ~mover_pieces_mask;

    if (pin & piece_mask)
    {
      open_and_visible_mask &= pin;  
    }

    /* Add all open and visible opponent squares to the move count.
    */
    mn += (unsigned long long) __builtin_popcountll (open_and_visible_mask);

  }

  while (knight_piece_mask)
  {
    /* Any squares that already have same color pieces must be excluded from the
    ** move candidates.
    */
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(knight_piece_mask);
    const unsigned long long move_candidate_mask =
                knightAttack[piece_index] &
                      ~mover_pieces_mask;

#if defined(USE_BMI)
    const unsigned long long piece_mask = _blsi_u64(knight_piece_mask);
    knight_piece_mask = _blsr_u64(knight_piece_mask);
#else
    const unsigned long long piece_mask = 1LLU << piece_index;
    knight_piece_mask ^= piece_mask;
#endif

    /* Pinned knight cannot move.
    */
    if (0 == (pin & piece_mask))
    {
      /* If Knight is not pinned and king is not in check
      ** then we can take a short cut for counting destination squares.
      */
      mn += (unsigned long long) __builtin_popcountll (move_candidate_mask);

    }
  }

  while (queen_piece_mask)
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(queen_piece_mask);
#if defined(USE_BMI)
    const unsigned long long piece_mask = _blsi_u64(queen_piece_mask);
    queen_piece_mask = _blsr_u64(queen_piece_mask);
#else
    const unsigned long long piece_mask = 1LLU << piece_index;
    queen_piece_mask ^= piece_mask;
#endif

    const aggregateAttack_t *const aggregateAttackVal = &aggregateAttack[piece_index];
    const unsigned long long lookup_index = 
                   lookupKeyCompute (any_color_pieces_mask, aggregateAttackVal->diagonalAttack);
    const unsigned long long udlr_lookup_index = 
                   lookupKeyCompute (any_color_pieces_mask, aggregateAttackVal->udlrAttack);

    unsigned long long open_and_visible_mask = (udlrVisibilityMap[piece_index][udlr_lookup_index] |
                                                diagonalVisibilityMap[piece_index][lookup_index]) &
                                                        ~mover_pieces_mask;

    if (pin & piece_mask)
    {
      open_and_visible_mask &= pin;  
    }

    /* Add all open and visible opponent squares to the move count.
    */
    mn += (unsigned long long) __builtin_popcountll (open_and_visible_mask);

  }

  return mn;
}
/******************************************************************************
** Find all eligible square to which the Bishop, Rook, and Queen can move.
**
**
** Return Values:
******************************************************************************/
__attribute__((always_inline)) inline
//__attribute__((noinline)) 
static unsigned long long allBRQNPSquaresLastPlyFindNoTest (
                                   const color_e whose_move,
                                   const unsigned long long *restrict piece,
                                   const unsigned int en_passant_eligible_pawn,
                                   const unsigned long long mover_pieces_mask,
                                   const unsigned long long opponent_pieces_mask,
                                   const unsigned long long any_color_pieces_mask)
{
  unsigned long long mn = 0;
  unsigned long long piece_mask = piece[S_PAWN | (whose_move << 3)];
  unsigned long long knight_piece_mask = piece[S_KNIGHT | (whose_move << 3)];
  unsigned long long bishop_piece_mask = piece[S_BISHOP | (whose_move << 3)];
  unsigned long long rook_piece_mask = piece[S_ROOK | (whose_move << 3)];
  unsigned long long queen_piece_mask = piece[S_QUEEN | (whose_move << 3)];
    

  while (bishop_piece_mask)
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(bishop_piece_mask);
#if defined(USE_BMI)
    bishop_piece_mask = _blsr_u64(bishop_piece_mask);
#else
    bishop_piece_mask ^= 1LLU << piece_index;
#endif


    const unsigned long long lookup_index = 
                   lookupKeyCompute (any_color_pieces_mask, diagonalAttack[piece_index]);

    const unsigned long long open_and_visible_mask = diagonalVisibilityMap[piece_index][lookup_index] &
                                                      ~mover_pieces_mask;

    /* Add all open and visible opponent squares to the move count.
    */
    mn += (unsigned long long) __builtin_popcountll (open_and_visible_mask);

  }

  while (rook_piece_mask)
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(rook_piece_mask);
#if defined(USE_BMI)
    rook_piece_mask = _blsr_u64(rook_piece_mask);
#else
    rook_piece_mask ^= 1LLU << piece_index;
#endif

    const unsigned long long lookup_index = 
                   lookupKeyCompute (any_color_pieces_mask, udlrAttack[piece_index]);

    const unsigned long long open_and_visible_mask = udlrVisibilityMap[piece_index][lookup_index] &
                                                      ~mover_pieces_mask;

    /* Add all open and visible opponent squares to the move count.
    */
    mn += (unsigned long long) __builtin_popcountll (open_and_visible_mask);

  }

  while (knight_piece_mask)
  {
    /* Any squares that already have same color pieces must be excluded from the
    ** move candidates.
    */
    const unsigned long long move_candidate_mask =
                knightAttack[bitbrdLowestIndexFromMaskGet(knight_piece_mask)] &
                      ~mover_pieces_mask;

#if defined(USE_BMI)
    knight_piece_mask = _blsr_u64(knight_piece_mask);
#else
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(knight_piece_mask);
    knight_piece_mask ^= 1LLU << piece_index;
#endif

    /* If Knight is not pinned and king is not in check
    ** then we can take a short cut for counting destination squares.
    */
    mn += (unsigned long long) __builtin_popcountll (move_candidate_mask);

  }

  while (queen_piece_mask)
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(queen_piece_mask);
#if defined(USE_BMI)
    queen_piece_mask = _blsr_u64(queen_piece_mask);
#else
    queen_piece_mask ^= 1LLU << piece_index;
#endif

    const aggregateAttack_t *const aggregateAttackVal = &aggregateAttack[piece_index];
    const unsigned long long lookup_index = 
                   lookupKeyCompute (any_color_pieces_mask, aggregateAttackVal->diagonalAttack);
    const unsigned long long udlr_lookup_index = 
                   lookupKeyCompute (any_color_pieces_mask, aggregateAttackVal->udlrAttack);

    const unsigned long long open_and_visible_mask = (udlrVisibilityMap[piece_index][udlr_lookup_index] |
                                                diagonalVisibilityMap[piece_index][lookup_index]) &
                                                        ~mover_pieces_mask;

    /* Add all open and visible opponent squares to the move count.
    */
    mn += (unsigned long long) __builtin_popcountll (open_and_visible_mask);

  }

  if (whose_move == MOVE_WHITE)
  {
    constexpr unsigned long long row1_mask = 0x000000000000ff00;
    constexpr unsigned long long row6_mask = 0x00ff000000000000;

    // Remove pawns eligible for promotion.
    unsigned long long shortcut_pawns = (piece_mask & ~row6_mask);

    if (unlikely(en_passant_eligible_pawn))
    {
      // Determine whch pawns can perform an en passant capture.
      const unsigned long long ep_mask = 1LLU << en_passant_eligible_pawn;
      const unsigned long long ep_capable_1 = shortcut_pawns & ((ep_mask & ~col0_mask) >> 1);
      const unsigned long long ep_capable_2 = shortcut_pawns & ((ep_mask & ~col7_mask) << 1);

      if (ep_capable_1)
                       mn++;

      if (ep_capable_2)
                       mn++;

    }
    // Determine which pawns can perform a regular capture.
    unsigned long long capture_1 = shortcut_pawns & ((opponent_pieces_mask & ~col0_mask) >> 9);
    unsigned long long capture_2 = shortcut_pawns & ((opponent_pieces_mask & ~col7_mask) >> 7);

    mn += (unsigned long long) __builtin_popcountll (capture_1);
    mn += (unsigned long long) __builtin_popcountll (capture_2);

    /* Adjust piece mask to contain only those pawns that can't
    ** be handled with a shortcut.
    */
    piece_mask ^= shortcut_pawns;

    /* Determine which pawns can move one square forward.
    ** The destination square must be empty in order for a pawn to be
    ** eligible for that move.
    */
    shortcut_pawns &= ~(any_color_pieces_mask >> 8);

    // The remaining pawns can move forward one square.
    mn += (unsigned long long) __builtin_popcountll (shortcut_pawns);

    // Only pawns in row 1 can move forward two squares.
    // The second square must be empty in order for a pawn to move two rows.
    shortcut_pawns &= row1_mask & ~(any_color_pieces_mask >> 16);

    // The remaining pawns can move forward two squares.
    mn += (unsigned long long) __builtin_popcountll (shortcut_pawns);

    if (likely(!piece_mask))
    {
      return mn;
    }


    /* At this point the only pawns left in the piece_mask are pawns that are
    ** eligible for promotion.
    ** Compute the move counts for these pawns.
    */
    capture_1 = piece_mask & ((opponent_pieces_mask & ~col0_mask) >> 9);
    capture_2 = piece_mask & ((opponent_pieces_mask & ~col7_mask) >> 7);

    /* Add 4 moves for each capture.
    */
    mn += (unsigned long long) (__builtin_popcountll (capture_1) << 2);
    mn += (unsigned long long) (__builtin_popcountll (capture_2) << 2);

    /* Determine which pawns can move one square forward
    ** and add four moves per pawn.
    */
    piece_mask &= ~(any_color_pieces_mask >> 8);
    mn += (unsigned long long) (__builtin_popcountll (piece_mask) << 2);

  } 

  if (whose_move == MOVE_BLACK)
  {
    constexpr unsigned long long row6_mask = 0x00ff000000000000;
    constexpr unsigned long long row1_mask = 0x000000000000ff00;

    // Remove pawns eligible for promotion.
    unsigned long long shortcut_pawns = (piece_mask & ~row1_mask);


    // Determine whch pawns can perform an en passant capture.
    if (unlikely(en_passant_eligible_pawn))
    {
      const unsigned long long ep_mask = 1LLU << en_passant_eligible_pawn;
      const unsigned long long ep_capable_1 = shortcut_pawns & ((ep_mask & ~col0_mask) >> 1);
      const unsigned long long ep_capable_2 = shortcut_pawns & ((ep_mask & ~col7_mask) << 1);

      if (ep_capable_1)
                       mn++;

      if (ep_capable_2)
                       mn++;

    }

    // Determine which pawns can perform a regular capture.
    unsigned long long capture_1 = shortcut_pawns & ((opponent_pieces_mask & ~col0_mask) << 7);
    unsigned long long capture_2 = shortcut_pawns & ((opponent_pieces_mask & ~col7_mask) << 9);


    mn += (unsigned long long) __builtin_popcountll (capture_1);
    mn += (unsigned long long) __builtin_popcountll (capture_2);

    /* Adjust piece mask to contain only those pawns that can't
    ** be handled with a shortcut.
    */
    piece_mask ^= shortcut_pawns;

    /* Determine which pawns can move one square forward.
    ** The destination square must be empty in order for a pawn to be
    ** eligible for that move.
    */
    shortcut_pawns &= ~(any_color_pieces_mask << 8);

    // The remaining pawns can move forward one square.
    mn += (unsigned long long) __builtin_popcountll (shortcut_pawns);

    // Only pawns in row 6 can move forward two squares.
    // The second square must be empty in order for a pawn to move two rows.
    shortcut_pawns &= row6_mask & ~(any_color_pieces_mask << 16);

    // The remaining pawns can move forward two squares.
    mn += (unsigned long long) __builtin_popcountll (shortcut_pawns);

    if (likely(!piece_mask))
    {
      return mn;
    }

    capture_1 = piece_mask & ((opponent_pieces_mask & ~col0_mask) << 7);
    capture_2 = piece_mask & ((opponent_pieces_mask & ~col7_mask) << 9);


    mn += (unsigned long long) (__builtin_popcountll (capture_1) << 2);
    mn += (unsigned long long) (__builtin_popcountll (capture_2) << 2);

    piece_mask &= ~(any_color_pieces_mask << 8);

    // The remaining pawns can move forward one square.
    mn += (unsigned long long) (__builtin_popcountll (piece_mask) << 2);

  }

  return mn;
}
/******************************************************************************
** Verify whether specified moves are legal.
** This function is used for the last ply.
**
** Return Value:
** The function returns the number of moves that are legal.
******************************************************************************/
//__attribute__((always_inline)) inline 
__attribute__((noinline)) 
static unsigned long long lastPlyMovesValidate (
                                    const unsigned char p1,
                                    unsigned long long from_mask,
                                    unsigned long long move_candidate_mask,
                                    const color_e whose_move,
                                    unsigned long long *restrict piece,
                                    const unsigned int king_position,
                                    const unsigned long long mover_pieces_mask,
                                    const unsigned long long opponent_pieces_mask,
                                    const unsigned long long any_color_pieces_mask)
{
  unsigned long long mn = 0;

  {
    unsigned long long visible_pieces_mask = move_candidate_mask & opponent_pieces_mask;
    unsigned long long open_squares_mask = move_candidate_mask & ~any_color_pieces_mask;

    while (open_squares_mask)
    {
#if defined(USE_BMI)
      const unsigned long long to_mask = _blsi_u64(open_squares_mask);
#else
      const unsigned int to_index = bitbrdLowestIndexFromMaskGet(open_squares_mask);
      const unsigned long long to_mask = 1LLU << to_index;
#endif
      const unsigned long long mask1 = to_mask | from_mask;

#if defined(USE_BMI)
      open_squares_mask = _blsr_u64(open_squares_mask);
#else
      open_squares_mask ^= to_mask;
#endif

      MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
      if (0 == kingInCheck(whose_move, piece, king_position, 
                                    (mover_pieces_mask ^ mask1) | opponent_pieces_mask))
      {
        mn++;
      }
      MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
    }

    while (unlikely(visible_pieces_mask))
    {
#if defined(USE_BMI)
      const unsigned long long to_mask = _blsi_u64(visible_pieces_mask);
#else
      const unsigned int to_index = bitbrdLowestIndexFromMaskGet(visible_pieces_mask);
      const unsigned long long to_mask = 1LLU << to_index;
#endif
      const unsigned long long mask1 = to_mask | from_mask;
      const unsigned char p2 = pieceTypeGet (whose_move, to_mask, piece);

#if defined(USE_BMI)
      visible_pieces_mask = _blsr_u64(visible_pieces_mask);
#else
      visible_pieces_mask ^= to_mask;
#endif

      MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,to_mask,0);
      if (0 == kingInCheck(whose_move, piece, king_position, 
              (mover_pieces_mask ^ mask1) | (opponent_pieces_mask ^ to_mask)))
      {
        mn++;
      }

      MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,to_mask,0);
    }
  }


  return mn;
}

/******************************************************************************
** Find all eligible squares to which the White king can move.
**
**    brd - (Input) The current position.
**    next_mv - (Input/Output) The list of all possible moves.
**    castle_eligibility - (Input) - Flags indicating who is eligible to castle.
**
** Return Values:
**      None.
******************************************************************************/
__attribute__((always_inline)) inline 
static unsigned long long whiteKingSquaresFind (
                            const unsigned int piece_index, 
                            unsigned long long move_candidate_mask,
                            unsigned long long *restrict piece,
                            oneMove_t *const next_mv,
                            const castleEligibility_t castle_eligibility,
                            const unsigned int record_undo_info,
                            const unsigned int depth,
                            const unsigned int ply,
                            const unsigned long long mover_pieces_mask,
                            const unsigned long long opponent_pieces_mask,
                            const unsigned int last_ply,
                            const unsigned long long king_attack_mask)
{
  if (!move_candidate_mask)
                  return 0;

  unsigned long long temp_move_candidate_mask = move_candidate_mask & king_attack_mask;
  const unsigned long long from_mask = 1LLU << piece_index;
  unsigned long long mn = 0;

  /* Disable white castling on all future moves.
  */
  castleEligibility_t template_castle_eligibility = castle_eligibility;
  template_castle_eligibility.white_short_ineligible = 1;
  template_castle_eligibility.white_long_ineligible = 1;

  while (temp_move_candidate_mask)
  {
    unsigned int num_masks;
    unsigned char p2;
    const unsigned int index = bitbrdLowestIndexFromMaskGet(temp_move_candidate_mask);
#if defined(USE_BMI)
    const unsigned long long mask2 = _blsi_u64(temp_move_candidate_mask);
    temp_move_candidate_mask = _blsr_u64(temp_move_candidate_mask);
#else
    const unsigned long long mask2 = 1LLU << index;
    temp_move_candidate_mask ^= mask2;
#endif
    const unsigned long long mask1 = mask2 | from_mask; 
    castleEligibility_t next_castle_eligibility = template_castle_eligibility;

    /* Check if there is an opponent piece at the destination location. 
    ** If so, then this move is a capture. We need to provide the mask of the captured piece.
    */
    if (0 == (mask2 & opponent_pieces_mask))
    {
      MOVE_APPLY (piece, 1,
                          S_KING | S_WHITE,0,0,
                          mask1,0,0);

      /* Make the move.
      */
      if (!record_undo_info)
      {
        mn += (last_ply)?
          allMoveCandidatesLastPlyFind(MOVE_BLACK,piece,0,next_castle_eligibility,
                                        mover_pieces_mask ^ mask1, opponent_pieces_mask):
          allMovePerft(MOVE_BLACK,piece,0,next_castle_eligibility,depth,ply,
                                        mover_pieces_mask ^ mask1, opponent_pieces_mask);
      }
      MOVE_APPLY (piece, 1,
                          S_KING | S_WHITE,0,0,
                          mask1,0,0);

      if (record_undo_info)
      {
        p2 = 0;
        num_masks = 1;
      }
    } else
    {
      p2 = pieceTypeGet (MOVE_WHITE, mask2, piece); 
      num_masks = 2;
      MOVE_APPLY (piece, 2,
                        S_KING | S_WHITE,p2,0,
                        mask1,mask2,0);

      /* Prepair the next castling eligibility flag and make the move.
      */
      {
        if (unlikely(p2 == (S_ROOK | S_BLACK)))
        { 
          if (!castle_eligibility.black_long_ineligible &&
             ((index >> 3) == 7) && 
             ((index & 7) == 0))
          {
            next_castle_eligibility.black_long_ineligible = 1;
          }
          if (!castle_eligibility.black_short_ineligible &&
             ((index >> 3) == 7) && 
             ((index & 7) == 7))
          {
            next_castle_eligibility.black_short_ineligible = 1;
          }
        }
        if (!record_undo_info)
        {
          mn += (last_ply)?
            allMoveCandidatesLastPlyFind(MOVE_BLACK,piece,0,next_castle_eligibility,
                                        mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2):
            allMovePerft(MOVE_BLACK,piece,0,next_castle_eligibility,depth,ply,
                                        mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2);
        }
      }
      MOVE_APPLY (piece, 2,
                        S_KING | S_WHITE,p2,0,
                        mask1,mask2,0);
    }
    if (record_undo_info)
    {
      {
        next_mv[mn].from_r = piece_index >> 3;
        next_mv[mn].from_c = piece_index & 7;
        next_mv[mn].to_r = index >> 3;
        next_mv[mn].to_c = index & 7;
        next_mv[mn].p1 = S_KING | S_WHITE;
        next_mv[mn].p2 = p2;
        next_mv[mn].num_masks = num_masks;
        next_mv[mn].mask1 = mask1; 
        next_mv[mn].mask2 = mask2;
        next_mv[mn].castle_eligibility = next_castle_eligibility;
        next_mv[mn].en_passant_eligible_pawn = 0;
        mn++;
      } 
    } 
  }

  temp_move_candidate_mask = move_candidate_mask & ~king_attack_mask;

  if (!temp_move_candidate_mask)
  {
    return mn;
  }

  /* Check King Side Castle.
  */
  if (temp_move_candidate_mask & POSITION_TO_BITMASK_LOOKUP(0, 6))
  {
    castleEligibility_t next_castle_eligibility = template_castle_eligibility;
    if (record_undo_info)
    {
      next_mv[mn].from_r = 0;
      next_mv[mn].from_c = 4;
      next_mv[mn].to_r = 0;
      next_mv[mn].to_c = 6;
      next_mv[mn].p1 = S_KING | S_WHITE;
      next_mv[mn].p2 = S_ROOK | S_WHITE;
      next_mv[mn].num_masks = 2;
      next_mv[mn].mask1 = from_mask | bitbrdMaskFromPositionGet(0, 6); // King Mask
      next_mv[mn].mask2 = bitbrdMaskFromPositionGet(0, 7) | 
                           bitbrdMaskFromPositionGet(0, 5); // Rook Mask
      next_mv[mn].castle_eligibility = next_castle_eligibility;
      next_mv[mn].en_passant_eligible_pawn = 0;
      mn++;
    } else
    {
      const unsigned char p2 = S_ROOK | S_WHITE;
      const unsigned long long mask1 = from_mask | bitbrdMaskFromPositionGet(0, 6); // King Mask
      const unsigned long long mask2 = bitbrdMaskFromPositionGet(0, 7) |
                                       bitbrdMaskFromPositionGet(0, 5); // Rook Mask

      MOVE_APPLY (piece, 2,
                  S_KING | S_WHITE,p2,0,
                  mask1,mask2,0);
      mn += (last_ply)?
             allMoveCandidatesLastPlyFind(
                          MOVE_BLACK,
                          piece,
                          0,
                          next_castle_eligibility,
                          mover_pieces_mask ^ (mask1 | mask2),
                          opponent_pieces_mask):
             allMovePerft(
                          MOVE_BLACK,
                          piece,
                          0,
                          next_castle_eligibility,
                          depth,
                          ply,
                          mover_pieces_mask ^ (mask1 | mask2),
                          opponent_pieces_mask);
      MOVE_APPLY (piece, 2,
                  S_KING | S_WHITE,p2,0,
                  mask1,mask2,0);
    } 
  }

  /* Check Queen Side Castle.
  */
  if (temp_move_candidate_mask & POSITION_TO_BITMASK_LOOKUP(0, 2))
  {
    castleEligibility_t next_castle_eligibility = template_castle_eligibility;
    if (record_undo_info)
    {
      next_mv[mn].from_r = 0;
      next_mv[mn].from_c = 4;
      next_mv[mn].to_r = 0;
      next_mv[mn].to_c = 2;
      next_mv[mn].p1 = S_KING | S_WHITE;
      next_mv[mn].p2 = S_ROOK | S_WHITE;
      next_mv[mn].num_masks = 2;
      next_mv[mn].mask1 = from_mask | bitbrdMaskFromPositionGet(0, 2); // King Mask
      next_mv[mn].mask2 = bitbrdMaskFromPositionGet(0, 0) | 
                           bitbrdMaskFromPositionGet(0, 3); // Rook Mask
      next_mv[mn].castle_eligibility = next_castle_eligibility;
      next_mv[mn].en_passant_eligible_pawn = 0;
      mn++;
    } else
    {
      const unsigned char p2 = S_ROOK | S_WHITE;
      const unsigned long long mask1 = from_mask | bitbrdMaskFromPositionGet(0, 2); // King Mask
      const unsigned long long mask2 = bitbrdMaskFromPositionGet(0, 0) |
                                       bitbrdMaskFromPositionGet(0, 3); // Rook Mask

      MOVE_APPLY (piece, 2,
                  S_KING | S_WHITE,p2,0,
                  mask1,mask2,0);
      mn += (last_ply)?
             allMoveCandidatesLastPlyFind(
                          MOVE_BLACK,
                          piece,
                          0,
                          next_castle_eligibility,
                          mover_pieces_mask ^ (mask1 | mask2),
                          opponent_pieces_mask):
             allMovePerft(
                          MOVE_BLACK,
                          piece,
                          0,
                          next_castle_eligibility,
                          depth,
                          ply,
                          mover_pieces_mask ^ (mask1 | mask2),
                          opponent_pieces_mask);
      MOVE_APPLY (piece, 2,
                  S_KING | S_WHITE,p2,0,
                  mask1,mask2,0);
    } 
  }
  return mn;
}

/******************************************************************************
** Find all eligible squares to which the Black king can move.
**
**    brd - (Input) The current position.
**    next_mv - (Input/Output) The list of all possible moves.
**    castle_eligibility - (Input) - Flags indicating who is eligible to castle.
**
** Return Values:
**      None.
******************************************************************************/
__attribute__((always_inline)) inline 
static unsigned long long blackKingSquaresFind (
                            const unsigned int piece_index, 
                            unsigned long long move_candidate_mask,
                            unsigned long long *restrict piece,
                            oneMove_t *const next_mv,
                            const castleEligibility_t castle_eligibility,
                            const unsigned int record_undo_info,
                            const unsigned int depth,
                            const unsigned int ply,
                            const unsigned long long mover_pieces_mask,
                            const unsigned long long opponent_pieces_mask,
                            const unsigned int last_ply,
                            const unsigned long long king_attack_mask)
{
  if (!move_candidate_mask)
                  return 0;

  unsigned long long temp_move_candidate_mask = move_candidate_mask & king_attack_mask;
  const unsigned long long from_mask = 1LLU << piece_index;
  unsigned long long mn = 0;

  /* Disable black castling on all future moves.
  */
  castleEligibility_t template_castle_eligibility = castle_eligibility;
  template_castle_eligibility.black_short_ineligible = 1;
  template_castle_eligibility.black_long_ineligible = 1;

  while (temp_move_candidate_mask)
  {
    unsigned int num_masks;
    unsigned char p2;
    const unsigned int index = bitbrdLowestIndexFromMaskGet(temp_move_candidate_mask);
#if defined(USE_BMI)
    const unsigned long long mask2 = _blsi_u64(temp_move_candidate_mask);
    temp_move_candidate_mask = _blsr_u64(temp_move_candidate_mask);
#else
    const unsigned long long mask2 = 1LLU << index;
    temp_move_candidate_mask ^= mask2;
#endif
    const unsigned long long mask1 = mask2 | from_mask; 
    castleEligibility_t next_castle_eligibility = template_castle_eligibility;

    /* Check if there is an opponent piece at the destination location. 
    ** If so, then this move is a capture. We need to provide the mask of the captured piece.
    */
    if (0 == (mask2 & opponent_pieces_mask))
    {
      MOVE_APPLY (piece, 1,
                          S_KING | S_BLACK,0,0,
                          mask1,0,0);
      /* Make the move.
      */
      if (!record_undo_info)
      {
        mn += (last_ply)?
          allMoveCandidatesLastPlyFind(MOVE_WHITE,piece,0,next_castle_eligibility,
                            mover_pieces_mask ^ mask1, opponent_pieces_mask):
          allMovePerft(MOVE_WHITE,piece,0,next_castle_eligibility,depth,ply,
                            mover_pieces_mask ^ mask1, opponent_pieces_mask);
      }
      MOVE_APPLY (piece, 1,
                          S_KING | S_BLACK,0,0,
                          mask1,0,0);

      if (record_undo_info)
      {
        p2 = 0;
        num_masks = 1;
      }
    } else
    {
      num_masks = 2;
      p2 = pieceTypeGet (MOVE_BLACK, mask2, piece); 
      MOVE_APPLY (piece, 2,
                        S_KING | S_BLACK,p2,0,
                        mask1,mask2,0);

      /* Prepair the next castling eligibility flag and make the move.
      */
      {
        if (unlikely(p2 == (S_ROOK | S_WHITE)))
        { 
          if (!castle_eligibility.white_long_ineligible &&
             ((index >> 3) == 0) && 
             ((index & 7) == 0))
          {
            next_castle_eligibility.white_long_ineligible = 1;
          }
          if (!castle_eligibility.white_short_ineligible &&
             ((index >> 3) == 0) && 
             ((index & 7) == 7))
          {
            next_castle_eligibility.white_short_ineligible = 1;
          }
        }
        if (!record_undo_info)
        {
          mn += (last_ply)?
            allMoveCandidatesLastPlyFind(MOVE_WHITE,piece,0,next_castle_eligibility,
                            mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2):
            allMovePerft(MOVE_WHITE,piece,0,next_castle_eligibility,depth,ply,
                            mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2);
        }
      }
      MOVE_APPLY (piece, 2,
                        S_KING | S_BLACK,p2,0,
                        mask1,mask2,0);
    }
    if (record_undo_info)
    {
      {
        next_mv[mn].from_r = piece_index >> 3;
        next_mv[mn].from_c = piece_index & 7;
        next_mv[mn].to_r = index >> 3;
        next_mv[mn].to_c = index & 7;
        next_mv[mn].p1 = S_KING | S_BLACK;
        next_mv[mn].p2 = p2;
        next_mv[mn].num_masks = num_masks;
        next_mv[mn].mask1 = mask1; 
        next_mv[mn].mask2 = mask2;
        next_mv[mn].castle_eligibility = next_castle_eligibility;
        next_mv[mn].en_passant_eligible_pawn = 0;
        mn++;
      } 
    } 
  }

  temp_move_candidate_mask = move_candidate_mask & ~king_attack_mask;

  if (!temp_move_candidate_mask)
  {
    return mn;
  }

  /* Check King Side Castle.
  */
  if (temp_move_candidate_mask & POSITION_TO_BITMASK_LOOKUP(7, 6))
  {
    castleEligibility_t next_castle_eligibility = template_castle_eligibility;
    if (record_undo_info)
    {
      next_mv[mn].from_r = 7;
      next_mv[mn].from_c = 4;
      next_mv[mn].to_r = 7;
      next_mv[mn].to_c = 6;
      next_mv[mn].p1 = S_KING | S_BLACK;
      next_mv[mn].p2 = S_ROOK | S_BLACK;
      next_mv[mn].num_masks = 2;
      next_mv[mn].mask1 = from_mask | bitbrdMaskFromPositionGet(7, 6); // King Mask
      next_mv[mn].mask2 = bitbrdMaskFromPositionGet(7, 7) | 
                           bitbrdMaskFromPositionGet(7, 5); // Rook Mask
      next_mv[mn].castle_eligibility = next_castle_eligibility;
      next_mv[mn].en_passant_eligible_pawn = 0;
      mn++;
    } else
    {
      const unsigned char p2 = S_ROOK | S_BLACK;
      const unsigned long long mask1 = from_mask | bitbrdMaskFromPositionGet(7, 6); // King Mask
      const unsigned long long mask2 = bitbrdMaskFromPositionGet(7, 7) |
                                       bitbrdMaskFromPositionGet(7, 5); // Rook Mask

      MOVE_APPLY (piece, 2,
                  S_KING | S_BLACK,p2,0,
                  mask1,mask2,0);
      mn += (last_ply)?
             allMoveCandidatesLastPlyFind(
                          MOVE_WHITE,
                          piece,
                          0,
                          next_castle_eligibility,
                          mover_pieces_mask ^ (mask1 | mask2),
                          opponent_pieces_mask):
             allMovePerft(
                          MOVE_WHITE,
                          piece,
                          0,
                          next_castle_eligibility,
                          depth,
                          ply,
                          mover_pieces_mask ^ (mask1 | mask2),
                          opponent_pieces_mask);
      MOVE_APPLY (piece, 2,
                  S_KING | S_BLACK,p2,0,
                  mask1,mask2,0);
    } 
  }


  /* Check Queen Side Castle.
  */
  if (temp_move_candidate_mask & POSITION_TO_BITMASK_LOOKUP(7, 2))
  {
    castleEligibility_t next_castle_eligibility = template_castle_eligibility;
    if (record_undo_info)
    {
      next_mv[mn].from_r = 7;
      next_mv[mn].from_c = 4;
      next_mv[mn].to_r = 7;
      next_mv[mn].to_c = 2;
      next_mv[mn].p1 = S_KING | S_BLACK;
      next_mv[mn].p2 = S_ROOK | S_BLACK;
      next_mv[mn].num_masks = 2;
      next_mv[mn].mask1 = from_mask | bitbrdMaskFromPositionGet(7, 2); // King Mask
      next_mv[mn].mask2 = bitbrdMaskFromPositionGet(7, 0) | 
                           bitbrdMaskFromPositionGet(7, 3); // Rook Mask
      next_mv[mn].castle_eligibility = next_castle_eligibility;
      next_mv[mn].en_passant_eligible_pawn = 0;
      mn++;
    } else 
    {
      const unsigned char p2 = S_ROOK | S_BLACK;
      const unsigned long long mask1 = from_mask | bitbrdMaskFromPositionGet(7, 2); // King Mask
      const unsigned long long mask2 = bitbrdMaskFromPositionGet(7, 0) |
                                       bitbrdMaskFromPositionGet(7, 3); // Rook Mask

      MOVE_APPLY (piece, 2,
                  S_KING | S_BLACK,p2,0,
                  mask1,mask2,0);
      mn += (last_ply)?
             allMoveCandidatesLastPlyFind(
                          MOVE_WHITE,
                          piece,
                          0,
                          next_castle_eligibility,
                          mover_pieces_mask ^ (mask1 | mask2),
                          opponent_pieces_mask):
             allMovePerft(
                          MOVE_WHITE,
                          piece,
                          0,
                          next_castle_eligibility,
                          depth,
                          ply,
                          mover_pieces_mask ^ (mask1 | mask2),
                          opponent_pieces_mask);
      MOVE_APPLY (piece, 2,
                  S_KING | S_BLACK,p2,0,
                  mask1,mask2,0);
    } 
  }
  return mn;
}

/******************************************************************************
** Find all eligible square to which the Bishop, Rook, and Queen can move.
**
**
** Return Values:
******************************************************************************/
__attribute__((always_inline)) inline 
static unsigned long long allBishopRookQueenSquaresFind (
                                   const color_e whose_move,
                                   unsigned long long *restrict piece,
                                   oneMove_t *const next_mv,
                                   const unsigned int king_position,
                                   const unsigned int record_undo_info,
                                   const unsigned long long in_check,
                                   const unsigned long long pin,
                                   const unsigned long long move_test_needed,
                                   const castleEligibility_t castle_eligibility,
                                   const unsigned int depth,
                                   const unsigned int ply,
                                   const unsigned long long mover_pieces_mask,
                                   const unsigned long long opponent_pieces_mask,
                                   const unsigned int last_ply)
{
  const unsigned long long any_color_pieces_mask = mover_pieces_mask | opponent_pieces_mask;
  unsigned long long mn = 0;
  unsigned long long valid_moves = (0 == in_check)?~mover_pieces_mask:
                                                    in_check & ~mover_pieces_mask;

  {
    const unsigned char p1 = (unsigned char) ((S_BISHOP) | (whose_move << 3));
    unsigned long long piece_mask = piece[p1];
    while (piece_mask)
    {
      const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
#if defined(USE_BMI)
      const unsigned long long from_mask = _blsi_u64(piece_mask);
      piece_mask = _blsr_u64(piece_mask);
#else
      const unsigned long long from_mask = 1LLU << piece_index;
      piece_mask ^= from_mask;
#endif

      const unsigned long long lookup_index =  
                     lookupKeyCompute (any_color_pieces_mask, diagonalAttack[piece_index]);
                                                                         

      unsigned long long piece_move_mask = 
                    diagonalVisibilityMap[piece_index][lookup_index] & valid_moves;
                                                       
      const unsigned long long visible_pieces_mask = piece_move_mask & opponent_pieces_mask;
      if (unlikely (pin && (pin & from_mask)))
      {
        /* We can only move to other pinned squares.
        */
        piece_move_mask &= pin;
      }

      while (piece_move_mask)
      {
#if defined(USE_BMI)
        const unsigned long long mask2 = _blsi_u64(piece_move_mask);

        piece_move_mask = _blsr_u64(piece_move_mask);
#else
        const unsigned int move_index = bitbrdLowestIndexFromMaskGet(piece_move_mask);
        const unsigned long long mask2 = 1LLU << move_index;
        piece_move_mask ^= mask2;
#endif

        int under_attack = 0;
        unsigned char p2;
        unsigned int num_masks;
        castleEligibility_t next_castle_eligibility = castle_eligibility;

        if (unlikely(mask2 & visible_pieces_mask))
        {
          p2 = pieceTypeGet (whose_move, mask2, piece);
          num_masks = 2;
          if (move_test_needed && TEST_NEEDED)
          {
            const unsigned long long mask1 = mask2 | from_mask;
            MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
            under_attack = kingInCheck(whose_move,
                       piece, king_position, 
                       (mover_pieces_mask ^ mask1) | (opponent_pieces_mask ^ mask2));
            MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
          }
          if (!under_attack)
          {
            if (unlikely(PIECE_GET(p2) == S_ROOK))
            {
              const unsigned int to_index = bitbrdLowestIndexFromMaskGet(mask2);

              castleEligibilityRookCaptureCheck (whose_move,
                                        to_index,
                                        &next_castle_eligibility);

            }
            if (!record_undo_info)
            {
              const unsigned long long mask1 = mask2 | from_mask;
              MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
              mn += (last_ply)?
               allMoveCandidatesLastPlyFind(whose_move ^ 1, piece, 0, next_castle_eligibility,
                                    mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2):
               allMovePerft(whose_move ^ 1, piece, 0, next_castle_eligibility, depth, ply,
                                    mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2);
              MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
            }
          }
        } else
        {
          if (unlikely(move_test_needed && TEST_NEEDED))
          {
            const unsigned long long mask1 = mask2 | from_mask;
            MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
            under_attack = kingInCheck(whose_move,
                       piece, king_position,
                       (mover_pieces_mask ^ mask1) | opponent_pieces_mask);
            MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
          }
          if (!record_undo_info)
          {
            if (!under_attack)
            {
              const unsigned long long mask1 = mask2 | from_mask;
              MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
              mn += (last_ply)?
               allMoveCandidatesLastPlyFind(whose_move ^ 1, piece, 0, next_castle_eligibility,
                                    mover_pieces_mask ^ mask1, opponent_pieces_mask): 
               allMovePerft(whose_move ^ 1, piece, 0, next_castle_eligibility, depth, ply,
                                    mover_pieces_mask ^ mask1, opponent_pieces_mask); 
              MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
            }
          } else
          {
            p2 = 0;
            num_masks = 1;
          }
        }
        if (record_undo_info)
        {
          if (likely(!under_attack))
          {
            const unsigned int to_index = bitbrdLowestIndexFromMaskGet(mask2);

            next_mv[mn].from_r = piece_index >> 3;
            next_mv[mn].from_c = piece_index & 7;
            next_mv[mn].to_r = to_index >> 3;
            next_mv[mn].to_c = to_index & 7;
            next_mv[mn].p1 = p1;
            next_mv[mn].num_masks = num_masks;
            next_mv[mn].mask2 = mask2;
            next_mv[mn].mask1 = mask2 | from_mask;
            next_mv[mn].p2 = p2;
            next_mv[mn].castle_eligibility = next_castle_eligibility;
            next_mv[mn].en_passant_eligible_pawn = 0;
            mn++;
          }
        }
      }
    }
  }
  {
    const unsigned char p1 = (unsigned char) ((S_ROOK) | (whose_move << 3));
    unsigned long long piece_mask = piece[p1];
    while (piece_mask)
    {
      const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
#if defined(USE_BMI)
      const unsigned long long from_mask = _blsi_u64(piece_mask);
      piece_mask = _blsr_u64(piece_mask);
#else
      const unsigned long long from_mask = 1LLU << piece_index;
      piece_mask ^= from_mask;
#endif

      const unsigned long long lookup_index =  
                     lookupKeyCompute (any_color_pieces_mask, udlrAttack[piece_index]);
                                                                         
      unsigned long long piece_move_mask = 
                    udlrVisibilityMap[piece_index][lookup_index] & valid_moves;

      const unsigned long long visible_pieces_mask = piece_move_mask & opponent_pieces_mask;

      /* Set up the castle eligibility template for this rook.
      ** The template will need to be modified for every destination square.
      */
      castleEligibility_t castle_eligibility_template = castle_eligibility;
      castleEligibilityRookTemplateSet (whose_move,
                                      piece_index,
                                      &castle_eligibility_template);

      if (unlikely (pin && (pin & from_mask)))
      {
        /* We can only move to other pinned squares.
        */
        piece_move_mask &= pin;
      }

      while (piece_move_mask)
      {
#if defined(USE_BMI)
        const unsigned long long mask2 = _blsi_u64(piece_move_mask);

        piece_move_mask = _blsr_u64(piece_move_mask);
#else
        const unsigned int move_index = bitbrdLowestIndexFromMaskGet(piece_move_mask);
        const unsigned long long mask2 = 1LLU << move_index;
        piece_move_mask ^= mask2;
#endif

        int under_attack = 0;
        unsigned char p2;
        unsigned int num_masks;
        castleEligibility_t next_castle_eligibility = castle_eligibility_template;

        if (unlikely(mask2 & visible_pieces_mask))
        {
          p2 = pieceTypeGet (whose_move, mask2, piece);
          num_masks = 2;
          if (move_test_needed && TEST_NEEDED)
          {
            const unsigned long long mask1 = mask2 | from_mask;
            MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
            under_attack = kingInCheck(whose_move,
                       piece, king_position,
                       (mover_pieces_mask ^ mask1) | (opponent_pieces_mask ^ mask2));
            MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
          }
          if (!under_attack)
          {
            if (unlikely(PIECE_GET(p2) == S_ROOK))
            {
              const unsigned int to_index = bitbrdLowestIndexFromMaskGet(mask2);

              castleEligibilityRookCaptureCheck (whose_move,
                                        to_index,
                                        &next_castle_eligibility);

            }
            if (!record_undo_info)
            {
              const unsigned long long mask1 = mask2 | from_mask;
              MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
              mn += (last_ply)?
               allMoveCandidatesLastPlyFind(whose_move ^ 1, piece, 0, next_castle_eligibility,
                                               mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2):
               allMovePerft(whose_move ^ 1, piece, 0, next_castle_eligibility, depth, ply,
                                               mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2);
              MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
            }
          }
        } else
        {
          if (unlikely(move_test_needed && TEST_NEEDED))
          {
            const unsigned long long mask1 = mask2 | from_mask;
            MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
            under_attack = kingInCheck(whose_move,
                       piece, king_position,
                       (mover_pieces_mask ^ mask1) | opponent_pieces_mask);
            MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
          }
          if (!record_undo_info)
          {
            if (!under_attack)
            {
              const unsigned long long mask1 = mask2 | from_mask;
              MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
              mn += (last_ply)?
               allMoveCandidatesLastPlyFind(whose_move ^ 1, piece, 0, next_castle_eligibility,
                                mover_pieces_mask ^ mask1, opponent_pieces_mask): 
               allMovePerft(whose_move ^ 1, piece, 0, next_castle_eligibility, depth, ply,
                                mover_pieces_mask ^ mask1, opponent_pieces_mask); 
              MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
            }
          } else
          {
            p2 = 0;
            num_masks = 1;
          }
        }
        if (record_undo_info)
        {
          if (!under_attack)
          {
            const unsigned int to_index = bitbrdLowestIndexFromMaskGet(mask2);

            next_mv[mn].from_r = piece_index >> 3;
            next_mv[mn].from_c = piece_index & 7;
            next_mv[mn].to_r = to_index >> 3;
            next_mv[mn].to_c = to_index & 7;
            next_mv[mn].p1 = p1;
            next_mv[mn].num_masks = num_masks;
            next_mv[mn].mask2 = mask2;
            next_mv[mn].mask1 = mask2 | from_mask;
            next_mv[mn].p2 = p2;
            next_mv[mn].castle_eligibility = next_castle_eligibility;
            next_mv[mn].en_passant_eligible_pawn = 0;
            mn++;
          }
        }
      }
    }
  }
  {
    const unsigned char p1 = (unsigned char) ((S_QUEEN) | (whose_move << 3));
    unsigned long long piece_mask = piece[p1];
    while (piece_mask)
    {
      const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
#if defined(USE_BMI)
      const unsigned long long from_mask = _blsi_u64(piece_mask);
      piece_mask = _blsr_u64(piece_mask);
#else
      const unsigned long long from_mask = 1LLU << piece_index;
      piece_mask ^= from_mask;
#endif

      const aggregateAttack_t *const aggregateAttackVal = &aggregateAttack[piece_index];
      const unsigned long long lookup_index =  
                     lookupKeyCompute (any_color_pieces_mask, aggregateAttackVal->diagonalAttack);
      const unsigned long long udlr_lookup_index =  
                     lookupKeyCompute (any_color_pieces_mask, aggregateAttackVal->udlrAttack);

      unsigned long long piece_move_mask = 
                    (diagonalVisibilityMap[piece_index][lookup_index] |
                    udlrVisibilityMap[piece_index][udlr_lookup_index]) & valid_moves;

      const unsigned long long visible_pieces_mask = piece_move_mask & opponent_pieces_mask;


      if (unlikely(pin && (pin & from_mask)))
      {
        /* We can only move to other pinned squares.
        */
        piece_move_mask &= pin;
      }

      while (piece_move_mask)
      {
#if defined(USE_BMI)
        const unsigned long long mask2 = _blsi_u64(piece_move_mask);
        piece_move_mask = _blsr_u64(piece_move_mask);
#else
        const unsigned int move_index = bitbrdLowestIndexFromMaskGet(piece_move_mask);
        const unsigned long long mask2 = 1LLU << move_index;
        piece_move_mask ^= mask2;
#endif

        int under_attack = 0;
        unsigned char p2;
        unsigned int num_masks;
        castleEligibility_t next_castle_eligibility = castle_eligibility;

        if (unlikely(mask2 & visible_pieces_mask))
        {
          p2 = pieceTypeGet (whose_move, mask2, piece);
          num_masks = 2;
          if (move_test_needed && TEST_NEEDED )
          {
            const unsigned long long mask1 = mask2 | from_mask;
            MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
            under_attack = kingInCheck(whose_move,
                       piece, king_position,
                       (mover_pieces_mask ^ mask1) | (opponent_pieces_mask ^ mask2));
            MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
          }
          if (!under_attack)
          {
            if (unlikely(PIECE_GET(p2) == S_ROOK))
            {
              const unsigned int to_index = bitbrdLowestIndexFromMaskGet(mask2);
              castleEligibilityRookCaptureCheck (whose_move,
                                        to_index,
                                        &next_castle_eligibility);
            }
            if (!record_undo_info)
            {
              const unsigned long long mask1 = mask2 | from_mask;
              MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
              mn += (last_ply)?
               allMoveCandidatesLastPlyFind(whose_move ^ 1, piece, 0, next_castle_eligibility,
                                    mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2):
               allMovePerft(whose_move ^ 1, piece, 0, next_castle_eligibility, depth, ply,
                                    mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2);
              MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
            }
          }
        } else
        {
          if (unlikely(move_test_needed && TEST_NEEDED))
          {
            const unsigned long long mask1 = mask2 | from_mask;
            MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
            under_attack = kingInCheck(whose_move,
                       piece, king_position,
                       (mover_pieces_mask ^ mask1) | opponent_pieces_mask);
            MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
          }
          if (!record_undo_info)
          {
            if (!under_attack)
            {
              const unsigned long long mask1 = mask2 | from_mask;
              MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
              mn += (last_ply)?
               allMoveCandidatesLastPlyFind(whose_move ^ 1, piece, 0, next_castle_eligibility,
                            mover_pieces_mask ^ mask1, opponent_pieces_mask):
               allMovePerft(whose_move ^ 1, piece, 0, next_castle_eligibility, depth, ply,
                            mover_pieces_mask ^ mask1, opponent_pieces_mask);
              MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
            }
          } else
          {
            p2 = 0;
            num_masks = 1;
          }
        }
        if (record_undo_info)
        {
          if (!under_attack)
          {
            const unsigned int to_index = bitbrdLowestIndexFromMaskGet(mask2);

            next_mv[mn].from_r = piece_index >> 3;
            next_mv[mn].from_c = piece_index & 7;
            next_mv[mn].to_r = to_index >> 3;
            next_mv[mn].to_c = to_index & 7;
            next_mv[mn].p1 = p1;
            next_mv[mn].num_masks = num_masks;
            next_mv[mn].mask2 = mask2;
            next_mv[mn].mask1 = mask2 | from_mask;
            next_mv[mn].p2 = p2;
            next_mv[mn].castle_eligibility = next_castle_eligibility;
            next_mv[mn].en_passant_eligible_pawn = 0;
            mn++;
          } 
        }
      }
    }
  }

  return mn;
}

/******************************************************************************
** Find all eligible square to which the Bishop, Rook, and Queen can move.
**
**
** Return Values:
******************************************************************************/
__attribute__((always_inline)) inline 
static unsigned long long allBishopRookQueenSquaresLastPlyFind (
                                   const color_e whose_move,
                                   unsigned long long *restrict piece,
                                   const unsigned int king_position,
                                   const unsigned long long in_check,
                                   const unsigned long long pin,
                                   const unsigned long long move_test_needed,
                                   const unsigned long long mover_pieces_mask,
                                   const unsigned long long opponent_pieces_mask)
{
  const unsigned long long any_color_pieces_mask = mover_pieces_mask | opponent_pieces_mask;
  unsigned long long mn = 0;
  unsigned long long valid_moves = (0 == in_check)?~mover_pieces_mask:
                                                    in_check & ~mover_pieces_mask;

  {
    const unsigned char p1 = (unsigned char) ((S_BISHOP) | (whose_move << 3));
    unsigned long long piece_mask = piece[p1];
    while (piece_mask)
    {
      const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
#if defined(USE_BMI)
      const unsigned long long from_mask = _blsi_u64(piece_mask);
      piece_mask = _blsr_u64(piece_mask);
#else
      const unsigned long long from_mask = 1LLU << piece_index;
      piece_mask ^= from_mask;
#endif


      const unsigned long long lookup_index =  
                     lookupKeyCompute (any_color_pieces_mask, diagonalAttack[piece_index]);
                                                                         
      unsigned long long open_and_visible_mask = diagonalVisibilityMap[piece_index][lookup_index] &
                                                        valid_moves;


      if (unlikely (pin && (pin & from_mask)))
      {
        /* We can only move to other pinned squares.
        */
        open_and_visible_mask &= pin;
      }

      /* If the king is not in check and the piece is not pinned, then we can take a 
      ** shortcut for counting moves.
      */
      if (!move_test_needed || !TEST_NEEDED)
      {
        /* Add all open and visible opponent squares to the move count.
        */
        mn += (unsigned long long) __builtin_popcountll (open_and_visible_mask);

      } else
      {
        mn += lastPlyMovesValidate (p1, from_mask, open_and_visible_mask,
                                    whose_move, piece,
                                    king_position, 
                                    mover_pieces_mask, opponent_pieces_mask,
                                    any_color_pieces_mask);
      }
    }
  }
  {
    const unsigned char p1 = (unsigned char) ((S_ROOK) | (whose_move << 3));
    unsigned long long piece_mask = piece[p1];
    while (piece_mask)
    {
      const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
#if defined(USE_BMI)
      const unsigned long long from_mask = _blsi_u64(piece_mask);
      piece_mask = _blsr_u64(piece_mask);
#else
      const unsigned long long from_mask = 1LLU << piece_index;
      piece_mask ^= from_mask;
#endif

      const unsigned long long lookup_index = 
                     lookupKeyCompute (any_color_pieces_mask, udlrAttack[piece_index]);

      unsigned long long open_and_visible_mask = udlrVisibilityMap[piece_index][lookup_index] &
                                                        valid_moves;

      if (unlikely(pin && (pin & from_mask)))
      {
        /* We can only move to other pinned squares.
        */
        open_and_visible_mask &= pin;
      }

      /* When there is no double check or double pin we can take a 
      ** shortcut for counting moves.
      */
      if (!move_test_needed || !TEST_NEEDED)
      {
        /* Add all open and visible opponent squares to the move count.
        */
        mn += (unsigned long long) __builtin_popcountll (open_and_visible_mask);

      } else
      {
        mn += lastPlyMovesValidate (p1, from_mask, open_and_visible_mask,
                                    whose_move, piece,
                                    king_position, 
                                    mover_pieces_mask, opponent_pieces_mask,
                                    any_color_pieces_mask);
      }
    }
  }
  {
    const unsigned char p1 = (unsigned char) ((S_QUEEN) | (whose_move << 3));
    unsigned long long piece_mask = piece[p1];
    while (piece_mask)
    {
      const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
#if defined(USE_BMI)
      const unsigned long long from_mask = _blsi_u64(piece_mask);
      piece_mask = _blsr_u64(piece_mask);
#else
      const unsigned long long from_mask = 1LLU << piece_index;
      piece_mask ^= from_mask;
#endif

      const aggregateAttack_t *const aggregateAttackVal = &aggregateAttack[piece_index];
      const unsigned long long lookup_index =  
                     lookupKeyCompute (any_color_pieces_mask, aggregateAttackVal->diagonalAttack);
      const unsigned long long udlr_lookup_index =  
                     lookupKeyCompute (any_color_pieces_mask, aggregateAttackVal->udlrAttack);

      unsigned long long open_and_visible_mask = (udlrVisibilityMap[piece_index][udlr_lookup_index] |
                                                  diagonalVisibilityMap[piece_index][lookup_index]) &
                                                        valid_moves;

      if (unlikely (pin && (pin & from_mask)))
      {
        /* We can only move to other pinned squares.
        */
        open_and_visible_mask &= pin;
      }

      if (!move_test_needed || !TEST_NEEDED)
      {
        /* Add all open and visible opponent squares to the move count.
        */
        mn += (unsigned long long) __builtin_popcountll (open_and_visible_mask);

      } else
      {
        mn += lastPlyMovesValidate (p1, from_mask, open_and_visible_mask,
                                    whose_move, piece,
                                    king_position, 
                                    mover_pieces_mask, opponent_pieces_mask,
                                    any_color_pieces_mask);
      }
    }
  }

  return mn;
}

/******************************************************************************
** Find all eligible square to which the knight can move.
**
**    brd - (Input) The current position.
**    next_mv - (Input/Output) The next move.
**
** Return Values:
******************************************************************************/
__attribute__((always_inline)) inline
static unsigned long long allKnightSquaresFind (
                                    const color_e whose_move,
                                    unsigned long long *restrict piece,
                                    oneMove_t *const next_mv,
                                    const unsigned int king_position,
                                    const unsigned int record_undo_info,
                                    const unsigned long long in_check,
                                    const unsigned long long pin,
                                    const unsigned long long move_test_needed,
                                    const castleEligibility_t castle_eligibility,
                                    const unsigned int depth,
                                    const unsigned int ply,
                                    const unsigned long long mover_pieces_mask,
                                    const unsigned long long opponent_pieces_mask,
                                    const unsigned int last_ply)
{
  const unsigned char p1 = (unsigned char) (S_KNIGHT | (whose_move << 3));
  unsigned long long piece_mask = piece[p1];
  unsigned long long mn = 0;
  unsigned long long valid_moves = (0 == in_check)?~mover_pieces_mask:
                                                    in_check & ~mover_pieces_mask;

  while (piece_mask)
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
    unsigned long long move_candidate_mask =  knightAttack[piece_index];
#if defined(USE_BMI)
    const unsigned long long from_mask = _blsi_u64(piece_mask);
    piece_mask = _blsr_u64(piece_mask);
#else
    const unsigned long long from_mask = 1LLU << piece_index;
    piece_mask ^= from_mask;
#endif

    /* Any squares that already have same color pieces must be excluded from the
    ** move candidates.
    */
    move_candidate_mask &= valid_moves;

    if (unlikely(pin && (pin & from_mask)))
    {
      /* If the knight is pinned then we don't even need to check the move.
      ** Pinned knighs can't move.
      */
      move_candidate_mask = 0;
    }

    while (move_candidate_mask)
    {
      unsigned long long mask2;
      unsigned int num_masks;
      unsigned char p2;
      const unsigned int index = bitbrdLowestIndexFromMaskGet(move_candidate_mask);
 #if defined(USE_BMI)
      const unsigned long long p1_move_mask = _blsi_u64(move_candidate_mask);
      move_candidate_mask = _blsr_u64(move_candidate_mask);
 #else
      const unsigned long long p1_move_mask = 1LLU << index;
      move_candidate_mask ^= p1_move_mask;
 #endif
      int under_attack = 0;
      castleEligibility_t next_castle_eligibility = castle_eligibility;

      /* Check if there is an opponent piece at the destination location. 
      ** If so, then this move is a capture. We need to provide the mask of the captured piece.
      */
      if (p1_move_mask & opponent_pieces_mask)
      {
        mask2 = p1_move_mask;
        p2 = pieceTypeGet (whose_move, p1_move_mask, piece);
        num_masks = 2;
        if (unlikely(move_test_needed && in_check))
        {
          const unsigned long long mask1 = p1_move_mask | from_mask;
          MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
          under_attack = kingInCheck(whose_move, 
                       piece, king_position,
                       (mover_pieces_mask ^ mask1) | (opponent_pieces_mask ^ mask2));
          MOVE_APPLY (piece, 2,
                        p1,p2,0,
                        mask1,mask2,0);
        } 

        if (!under_attack)
        {
          if (unlikely(PIECE_GET(p2) == S_ROOK))
          {
            castleEligibilityRookCaptureCheck (whose_move, 
                                             index, 
                                            &next_castle_eligibility);
          }
          if (!record_undo_info)
          {
            unsigned long long mask1 = p1_move_mask | from_mask;
            MOVE_APPLY (piece, 2,
                          p1,p2,0,
                          mask1,mask2,0);
            mn += (last_ply)?
               allMoveCandidatesLastPlyFind(whose_move ^ 1, piece, 0, next_castle_eligibility,
                                    mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2):
               allMovePerft(whose_move ^ 1, piece, 0, next_castle_eligibility, depth, ply,
                                    mover_pieces_mask ^ mask1, opponent_pieces_mask ^ mask2);
            MOVE_APPLY (piece, 2,
                          p1,p2,0,
                          mask1,mask2,0);
          }
        }
      } else
      {
        if (unlikely(move_test_needed && in_check))
        {
          const unsigned long long mask1 = p1_move_mask | from_mask;
          MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
          under_attack = kingInCheck(whose_move, 
                       piece, king_position,
                       (mover_pieces_mask ^ mask1) | opponent_pieces_mask);
          MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
        }
        if (!record_undo_info && !under_attack)
        {
          unsigned long long mask1 = p1_move_mask | from_mask;
          MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
          mn += (last_ply)?
             allMoveCandidatesLastPlyFind(whose_move ^ 1, piece, 0, next_castle_eligibility,
                                mover_pieces_mask ^ mask1, opponent_pieces_mask):
             allMovePerft(whose_move ^ 1, piece, 0, next_castle_eligibility, depth, ply,
                                mover_pieces_mask ^ mask1, opponent_pieces_mask);
          MOVE_APPLY (piece, 1,
                        p1,0,0,
                        mask1,0,0);
        }

        if (record_undo_info)
        {
          num_masks = 1;
          p2 = 0;
          mask2 = 0;
        }
      }

      if (record_undo_info)
      {
        if (!under_attack)
        {
          next_mv[mn].from_r = piece_index >> 3;
          next_mv[mn].from_c = piece_index & 7;
          next_mv[mn].to_r = index >> 3;
          next_mv[mn].to_c = index & 7;
          next_mv[mn].p1 = p1;
          next_mv[mn].p2 = p2;
          next_mv[mn].num_masks = num_masks;
          next_mv[mn].mask1 = p1_move_mask | from_mask;
          next_mv[mn].mask2 = mask2;
          next_mv[mn].castle_eligibility = next_castle_eligibility;
          next_mv[mn].en_passant_eligible_pawn = 0;
          mn++;
        } 
      }
    }
  }

  return mn;
}

/******************************************************************************
** Find all eligible square to which the knight can move.
**
**    brd - (Input) The current position.
**    next_mv - (Input/Output) The next move.
**
** Return Values:
******************************************************************************/
__attribute__((always_inline)) inline
static unsigned long long allKnightSquaresLastPlyFind (
                                    const color_e whose_move,
                                    unsigned long long *restrict piece,
                                    const unsigned int king_position,
                                    const unsigned long long in_check,
                                    const unsigned long long pin,
                                    const unsigned long long move_test_needed,
                                    const unsigned long long mover_pieces_mask,
                                    const unsigned long long opponent_pieces_mask)
{
  const unsigned long long any_color_pieces_mask = mover_pieces_mask | opponent_pieces_mask;
  const unsigned char p1 = (unsigned char) (S_KNIGHT | (whose_move << 3));
  unsigned long long piece_mask = piece[p1];
  unsigned long long valid_moves = (0 == in_check)?~mover_pieces_mask:
                                                    in_check & ~mover_pieces_mask;

  unsigned long long mn = 0;

  while (piece_mask)
  {
#if defined(USE_BMI)
    const unsigned long long from_mask = _blsi_u64(piece_mask);
#else
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
    const unsigned long long from_mask = 1LLU << piece_index;
#endif

    /* Any squares that already have same color pieces must be excluded from the
    ** move candidates.
    */
    unsigned long long move_candidate_mask =  
                knightAttack[bitbrdLowestIndexFromMaskGet(piece_mask)] &
                    valid_moves;

#if defined(USE_BMI)
    piece_mask = _blsr_u64(piece_mask);
#else
    piece_mask ^= from_mask;
#endif

    if (unlikely(pin && (pin & from_mask)))
    {
      /* If the knight is pinned then we don't even need to check the move.
      ** Pinned knighs can't move.
      */
      continue;
    } 

    /* If there is no double pin or double check
    ** then we can take a short cut for counting destination squares.
    */
    if (!move_test_needed || !in_check)
    {
      mn += (unsigned long long) __builtin_popcountll (move_candidate_mask);
    } else
    {
      mn += lastPlyMovesValidate (p1, from_mask, move_candidate_mask,
                                    whose_move, piece,
                                    king_position, 
                                    mover_pieces_mask, opponent_pieces_mask,
                                    any_color_pieces_mask);
    }
  }
 
  return mn;
}

/******************************************************************************
** Find all eligible square to which the White pawn can move.
**
**    brd - (Input) The current position.
**    next_mv - (Input/Output) The next move.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**
** Return Values:
******************************************************************************/
__attribute__((always_inline)) inline
static unsigned long long allWhitePawnSquaresFind (
                                    unsigned long long *restrict piece,
                                    oneMove_t *const next_mv,
                                    const unsigned int en_passant_eligible_pawn,
                                    const unsigned int king_position,
                                    const unsigned int record_undo_info,
                                    const unsigned long long in_check,
                                    const unsigned long long pin,
                                    const castleEligibility_t castle_eligibility,
                                    const unsigned int depth,
                                    const unsigned int ply,
                                    const unsigned long long mover_pieces_mask,
                                    const unsigned long long opponent_pieces_mask,
                                    const unsigned int last_ply)
{
  unsigned long long piece_mask = piece[S_PAWN | S_WHITE];

  const unsigned long long any_color_pieces_mask = mover_pieces_mask | opponent_pieces_mask;
  unsigned long long mn = 0;

  while (piece_mask)
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
    unsigned int to_index;
    unsigned long long p1_move_mask;
#if defined(USE_BMI)
    const unsigned long long from_mask = _blsi_u64(piece_mask);
    piece_mask = _blsr_u64(piece_mask);
#else
    const unsigned long long from_mask = 1LLU << piece_index;
    piece_mask ^= from_mask; /* Clear the bit associated with this piece */
#endif

    constexpr unsigned long long row1_mask = 0x000000000000ff00;

    /* Check upper file, one square advance. The destination square
    ** must be empty.
    */
    to_index = piece_index + 8;
    p1_move_mask = 1LLU << to_index;
    if (0 == (p1_move_mask & any_color_pieces_mask))
    {
      unsigned char p2 = 0;
      unsigned long long mask1;
      unsigned int num_masks;
      int under_attack = 0;

      constexpr unsigned long long row7_mask = 0xff00000000000000;

      /* Special handling for pawn promotion.
      */
      if (unlikely(p1_move_mask & row7_mask))
      {
        num_masks = 2;
        mask1 = from_mask;
        p2 = S_QUEEN | S_WHITE;
        if (TEST_NEEDED)
        {
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,p1_move_mask,0);
          under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                    (mover_pieces_mask ^ (mask1 | p1_move_mask)) |
                                    opponent_pieces_mask);
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,p1_move_mask,0);
        }

        if (!record_undo_info && (!under_attack))
        {
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,p1_move_mask,0);
          mn += (last_ply)?
             allMoveCandidatesLastPlyFind(MOVE_BLACK, piece, 0, castle_eligibility,
                                    mover_pieces_mask ^ (mask1 | p1_move_mask),
                                    opponent_pieces_mask):
             allMovePerft(MOVE_BLACK, piece, 0, castle_eligibility, depth, ply,
                                    mover_pieces_mask ^ (mask1 | p1_move_mask),
                                    opponent_pieces_mask);
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,p1_move_mask,0);
        }

      } else
      {
        mask1 = p1_move_mask | from_mask;
        num_masks = 1;
        if (TEST_NEEDED)
        {
          if (!en_passant_eligible_pawn && 
              ((in_check && !(in_check & p1_move_mask)) ||
                ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
          {
            /* If the move is outside of pin or in_check masks then we 
            ** know that its bad, and don't need to test it.
            */
            under_attack = 1;
          } else
          {
            MOVE_APPLY (piece, 1,
                        S_PAWN | S_WHITE,0,0,
                        mask1,0,0);
            under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                        (mover_pieces_mask ^ mask1) |
                                        opponent_pieces_mask);
            MOVE_APPLY (piece, 1,
                        S_PAWN | S_WHITE,0,0,
                        mask1,0,0);
          }
        }
        if (!record_undo_info && !under_attack)
        {
          MOVE_APPLY (piece, 1,
                        S_PAWN | S_WHITE,0,0,
                        mask1,0,0);
          mn += (last_ply)?
             allMoveCandidatesLastPlyFind(MOVE_BLACK, piece, 0, castle_eligibility,
                                    mover_pieces_mask ^ mask1, opponent_pieces_mask):
             allMovePerft(MOVE_BLACK, piece, 0, castle_eligibility, depth, ply,
                                    mover_pieces_mask ^ mask1, opponent_pieces_mask);
          MOVE_APPLY (piece, 1,
                        S_PAWN | S_WHITE,0,0,
                        mask1,0,0);
        }
      }

      if (0 == under_attack)
      {
        if (record_undo_info)
        {
          next_mv[mn].from_r = piece_index >> 3;
          next_mv[mn].from_c = piece_index & 7;
          next_mv[mn].to_r = to_index >> 3;
          next_mv[mn].to_c = piece_index & 7;
          next_mv[mn].p1 = S_PAWN | S_WHITE;
          next_mv[mn].num_masks = num_masks;
          next_mv[mn].p2 = p2;
          next_mv[mn].mask1 = mask1;
          next_mv[mn].mask2 = p1_move_mask;
          next_mv[mn].castle_eligibility = castle_eligibility;
          next_mv[mn].en_passant_eligible_pawn = 0;
          mn++;
        }

        /* If we had a pawn promotion then generate moves for promoting
        ** to lower ranked pieces. We don't need to do the squareUnderAttack()
        ** check for these moves.
        */
        if (unlikely(num_masks == 2))
        {
          for (p2 = (S_KNIGHT | S_WHITE); p2 <= (S_ROOK | S_WHITE); p2++)
          {
            if (record_undo_info)
            {
              next_mv[mn] = next_mv[mn-1];
              next_mv[mn].p2 = p2;
              mn++;
            } else
            {
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_WHITE,p2,0,
                            mask1,p1_move_mask,0);
              mn += (last_ply)?
                   allMoveCandidatesLastPlyFind(
                                MOVE_BLACK,
                                piece,
                                0,
                                castle_eligibility,
                                mover_pieces_mask ^ (mask1 | p1_move_mask),
                                opponent_pieces_mask):
                   allMovePerft(
                                MOVE_BLACK,
                                piece,
                                0,
                                castle_eligibility,
                                depth,
                                ply,
                                mover_pieces_mask ^ (mask1 | p1_move_mask),
                                opponent_pieces_mask);
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_WHITE,p2,0,
                            mask1,p1_move_mask,0);
            }
          }
        }
      }
      /* Check upper file, two square advance. The pawn must be at starting rank,
      ** and the two lower squares must be empty.
      */
      to_index = piece_index + 16;
      if ((from_mask & row1_mask) &&
        (0 == (((p1_move_mask = 1LLU << to_index)) & any_color_pieces_mask)))
      {

        under_attack = 0;
        if (TEST_NEEDED)
        {
          if (!en_passant_eligible_pawn &&
              ((in_check && !(in_check & p1_move_mask)) ||
                ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
          {
            under_attack = 1;
          } else
          {
            mask1 = p1_move_mask | from_mask;
            MOVE_APPLY (piece, 1,
                            S_PAWN | S_WHITE,0,0,
                            mask1,0,0);
            under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                    (mover_pieces_mask ^ mask1) | 
                                    opponent_pieces_mask);
            MOVE_APPLY (piece, 1,
                            S_PAWN | S_WHITE,0,0,
                            mask1,0,0);
          }
        }
        if (!under_attack)
        {
          /* The pawn moved two squares, so check if it is eligible for
          ** en passant capture. In order to be eligible, there must be an
          ** opponent pawn in the same row.
          */
          const unsigned int next_en_passant_eligible_pawn =
          (blackPawnEnPassantAttack[to_index] & piece[S_PAWN | S_BLACK])?to_index:0;

          if (!record_undo_info)
          {
            mask1 = p1_move_mask | from_mask;


            MOVE_APPLY (piece, 1,
                          S_PAWN | S_WHITE,0,0,
                          mask1,0,0);
            mn += (last_ply)?
                     allMoveCandidatesLastPlyFind(
                                  MOVE_BLACK,
                                  piece,
                                  next_en_passant_eligible_pawn,
                                  castle_eligibility,
                                  mover_pieces_mask ^ mask1, 
                                  opponent_pieces_mask):
                     allMovePerft(
                                  MOVE_BLACK,
                                  piece,
                                  next_en_passant_eligible_pawn,
                                  castle_eligibility,
                                  depth,
                                  ply,
                                  mover_pieces_mask ^ mask1, 
                                  opponent_pieces_mask);
            MOVE_APPLY (piece, 1,
                          S_PAWN | S_WHITE,0,0,
                          mask1,0,0);
          } else
          {
            next_mv[mn].from_r = piece_index >> 3;
            next_mv[mn].from_c = piece_index & 7;
            next_mv[mn].to_r = to_index >> 3;
            next_mv[mn].to_c = piece_index & 7;
            next_mv[mn].p1 = S_PAWN | S_WHITE;
            next_mv[mn].num_masks = 1;
            next_mv[mn].mask1 = p1_move_mask | from_mask;
            next_mv[mn].castle_eligibility = castle_eligibility;
            next_mv[mn].en_passant_eligible_pawn = next_en_passant_eligible_pawn;
            mn++;
          }
        }
      }
    }


    /* If there are no opponent pieces in any attack position, including en passant
    ** attack position, then skip checking for capture.
    */
    if (!(opponent_pieces_mask & whitePawnCapture[piece_index]))
                                                    continue;
    /* Check Left Upper.
    ** This must be a capture or en-passant capture.
    */
    to_index = (piece_index + 8) - 1;
    p1_move_mask = 1LLU << to_index;
    if (((piece_index & 7) > 0) &&
        ((p1_move_mask & opponent_pieces_mask) ||
          (en_passant_eligible_pawn == (piece_index - 1))))
    {
      unsigned char p2;
      unsigned long long mask2;
      unsigned int num_masks = 2;
      unsigned long long mask1 = p1_move_mask | from_mask;
      unsigned char p3 = 0;
      int under_attack = 0;
      castleEligibility_t next_castle_eligibility = castle_eligibility;
      unsigned long long new_mover_pieces_mask;
      unsigned long long new_opponent_pieces_mask;


      /* For en passant capture we know that the captured piece is a pawn.
      */
      if (en_passant_eligible_pawn == (piece_index - 1))
      {
        p2 = S_PAWN | S_BLACK;
        mask2 = 1LLU << en_passant_eligible_pawn;
        new_mover_pieces_mask = mover_pieces_mask ^ mask1;
        new_opponent_pieces_mask = opponent_pieces_mask ^ mask2;
        if (TEST_NEEDED)
        {
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,mask2,0);
          under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                    new_mover_pieces_mask |
                                    new_opponent_pieces_mask);
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,mask2,0);
        }
      } else
      {
        p2 = pieceTypeGet (MOVE_WHITE, p1_move_mask, piece);
        mask2 = p1_move_mask;
        new_opponent_pieces_mask = opponent_pieces_mask ^ mask2;

        constexpr unsigned long long row7_mask = 0xff00000000000000;

        /* Special handling for pawn promotion.
        */
        if (unlikely(p1_move_mask & row7_mask))
        {
          if (unlikely(PIECE_GET(p2) == S_ROOK))
          {
            castleEligibilityRookCaptureCheck (MOVE_WHITE, 
                                  to_index,
                                  &next_castle_eligibility);
          }
          num_masks = 3;
          mask1 = from_mask;
          p3 = S_QUEEN | S_WHITE;
          new_mover_pieces_mask = mover_pieces_mask ^ (mask1 | p1_move_mask);
          if (TEST_NEEDED)
          {
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_WHITE,p2,p3,
                        mask1,mask2,p1_move_mask);
            under_attack = kingInCheck(MOVE_WHITE, piece, king_position, 
                                    new_mover_pieces_mask |
                                    new_opponent_pieces_mask);
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_WHITE,p2,p3,
                        mask1,mask2,p1_move_mask);
          }
        } else
        {
          new_mover_pieces_mask = mover_pieces_mask ^ mask1;
          if (TEST_NEEDED)
          {
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,mask2,0);
            under_attack = kingInCheck(MOVE_WHITE, piece, king_position, 
                                new_mover_pieces_mask |
                                new_opponent_pieces_mask);
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,mask2,0);
          }
        }
      }

      if (likely(!under_attack))
      {
        if (record_undo_info)
        {
          next_mv[mn].from_r = piece_index >> 3;
          next_mv[mn].from_c = piece_index & 7;
          next_mv[mn].to_r = to_index >> 3;
          next_mv[mn].to_c = to_index & 7;
          next_mv[mn].num_masks = num_masks;
          next_mv[mn].p1 = S_PAWN | S_WHITE;
          next_mv[mn].p2 = p2;
          next_mv[mn].p3 = p3;
          next_mv[mn].mask1 = mask1;
          next_mv[mn].mask2 = mask2;
          next_mv[mn].mask3 = p1_move_mask;
          next_mv[mn].castle_eligibility = next_castle_eligibility;
          next_mv[mn].en_passant_eligible_pawn = 0;
          mn++;
        } else
        {
          MOVE_APPLY (piece, num_masks,
                        S_PAWN | S_WHITE,p2,p3,
                        mask1,mask2,p1_move_mask);
          mn += (last_ply)?
                   allMoveCandidatesLastPlyFind(
                                MOVE_BLACK,
                                piece,
                                0,
                                next_castle_eligibility,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask):
                   allMovePerft(
                                MOVE_BLACK,
                                piece,
                                0,
                                next_castle_eligibility,
                                depth,
                                ply,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask);
          MOVE_APPLY (piece, num_masks,
                        S_PAWN | S_WHITE,p2,p3,
                        mask1,mask2,p1_move_mask);
        } 

        /* If we had a pawn promotion then generate moves for promoting
        ** to lower ranked pieces. We don't need to do the squareUnderAttack()
        ** check for these moves.
        */
        if (unlikely(num_masks == 3))
        {
          for (p3 = (S_KNIGHT | S_WHITE); p3 <= (S_ROOK | S_WHITE); p3++)
          {
            if (record_undo_info)
            {
              next_mv[mn] = next_mv[mn-1];
              next_mv[mn].p3 = p3;
              mn++;
            } else
            {
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_WHITE,p2,p3,
                            mask1,mask2,p1_move_mask);
              mn += (last_ply)?
                   allMoveCandidatesLastPlyFind(
                                MOVE_BLACK,
                                piece,
                                0,
                                next_castle_eligibility,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask):
                   allMovePerft(
                                MOVE_BLACK,
                                piece,
                                0,
                                next_castle_eligibility,
                                depth,
                                ply,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask);
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_WHITE,p2,p3,
                            mask1,mask2,p1_move_mask);
            } 
          }
        }
      }
    }

    /* Check Right Upper.
    ** This must be a capture or en-passant capture.
    */
    to_index = (piece_index + 8) + 1;
    if (((piece_index & 7) < (BRDS-1)) &&
        (((p1_move_mask = (1LLU << to_index)) & opponent_pieces_mask) ||
          (en_passant_eligible_pawn == (piece_index + 1))))
    {
      unsigned char p2;
      unsigned long long mask2;
      unsigned int num_masks = 2;
      unsigned long long mask1 = p1_move_mask | from_mask;
      unsigned char p3 = 0;
      int under_attack = 0;
      castleEligibility_t next_castle_eligibility = castle_eligibility;
      unsigned long long new_mover_pieces_mask;
      unsigned long long new_opponent_pieces_mask;

      /* For en passant capture we know that the captured piece is a pawn.
      */
      if (en_passant_eligible_pawn == (piece_index + 1))
      {
        p2 = S_PAWN | S_BLACK;
        mask2 = 1LLU << en_passant_eligible_pawn;
        new_mover_pieces_mask = mover_pieces_mask ^ mask1;
        new_opponent_pieces_mask = opponent_pieces_mask ^ mask2;
        if (TEST_NEEDED)
        {
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,mask2,0);
          under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                    new_mover_pieces_mask |
                                    new_opponent_pieces_mask);
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,mask2,0);
        }
      } else
      {
        p2 = pieceTypeGet (MOVE_WHITE, p1_move_mask, piece);
        mask2 = p1_move_mask;
        new_opponent_pieces_mask = opponent_pieces_mask ^ mask2;

        constexpr unsigned long long row7_mask = 0xff00000000000000;

        /* Special handling for pawn promotion.
        */
        if (unlikely(p1_move_mask & row7_mask))
        {
          if (unlikely(PIECE_GET(p2) == S_ROOK))
          {
            castleEligibilityRookCaptureCheck (MOVE_WHITE, 
                                    to_index,
                                    &next_castle_eligibility);
          }
          num_masks = 3;
          mask1 = from_mask;
          p3 = S_QUEEN | S_WHITE;
          new_mover_pieces_mask = mover_pieces_mask ^ (mask1 | p1_move_mask);
          if (TEST_NEEDED)
          {
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_WHITE,p2,p3,
                        mask1,mask2,p1_move_mask);
            under_attack = kingInCheck(MOVE_WHITE, piece, king_position, 
                                    new_mover_pieces_mask |
                                    new_opponent_pieces_mask);
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_WHITE,p2,p3,
                        mask1,mask2,p1_move_mask);
          }
        } else
        {
          new_mover_pieces_mask = mover_pieces_mask ^ mask1;
          if (TEST_NEEDED)
          {
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,mask2,0);
            under_attack = kingInCheck(MOVE_WHITE, piece, king_position, 
                                    new_mover_pieces_mask |
                                    new_opponent_pieces_mask);
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,mask2,0);
          }
        }
      }

      if (likely(!under_attack))
      {
        if (record_undo_info)
        {
          next_mv[mn].from_r = piece_index >> 3;
          next_mv[mn].from_c = piece_index & 7;
          next_mv[mn].to_r = to_index >> 3;
          next_mv[mn].to_c = to_index & 7;
          next_mv[mn].p1 = S_PAWN | S_WHITE;
          next_mv[mn].num_masks = num_masks;
          next_mv[mn].p2 = p2;
          next_mv[mn].p3 = p3;
          next_mv[mn].mask1 = mask1;
          next_mv[mn].mask2 = mask2;
          next_mv[mn].mask3 = p1_move_mask;
          next_mv[mn].castle_eligibility = next_castle_eligibility;
          next_mv[mn].en_passant_eligible_pawn = 0;
          mn++;
        } else 
        {
          MOVE_APPLY (piece, num_masks,
                        S_PAWN | S_WHITE,p2,p3,
                        mask1,mask2,p1_move_mask);
          mn += (last_ply)?
                   allMoveCandidatesLastPlyFind(
                                MOVE_BLACK,
                                piece,
                                0,
                                next_castle_eligibility,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask):
                   allMovePerft(
                                MOVE_BLACK,
                                piece,
                                0,
                                next_castle_eligibility,
                                depth,
                                ply,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask);
          MOVE_APPLY (piece, num_masks,
                        S_PAWN | S_WHITE,p2,p3,
                        mask1,mask2,p1_move_mask);
        } 


        /* If we had a pawn promotion then generate moves for promoting
        ** to lower ranked pieces. We don't need to do the squareUnderAttack()
        ** check for these moves.
        */
        if (unlikely(num_masks == 3))
        {
          for (p3 = (S_KNIGHT | S_WHITE); p3 <= (S_ROOK | S_WHITE); p3++)
          {
            if (record_undo_info)
            {
              next_mv[mn] = next_mv[mn-1];
              next_mv[mn].p3 = p3;
              mn++;
            } else 
            {
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_WHITE,p2,p3,
                            mask1,mask2,p1_move_mask);
              mn += (last_ply)?
                   allMoveCandidatesLastPlyFind(
                                MOVE_BLACK,
                                piece,
                                0,
                                next_castle_eligibility,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask):
                   allMovePerft(
                                MOVE_BLACK,
                                piece,
                                0,
                                next_castle_eligibility,
                                depth,
                                ply,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask);
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_WHITE,p2,p3,
                            mask1,mask2,p1_move_mask);
            } 
          }
        }
      }
    }
  }
  return mn;
}

/******************************************************************************
** Find all eligible square to which the White pawn can move.
**
**    brd - (Input) The current position.
**    next_mv - (Input/Output) The next move.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**
** Return Values:
******************************************************************************/
__attribute__((always_inline)) inline
static unsigned long long allWhitePawnSquaresLastPlyFind (
                                    unsigned long long *restrict piece,
                                    const unsigned int en_passant_eligible_pawn,
                                    const unsigned int king_position,
                                    const unsigned long long in_check,
                                    const unsigned long long pin,
                                    unsigned long long move_test_needed,
                                    const unsigned long long mover_pieces_mask,
                                    const unsigned long long opponent_pieces_mask)
{
  unsigned long long piece_mask = piece[S_PAWN | S_WHITE];

  const unsigned long long any_color_pieces_mask = mover_pieces_mask | opponent_pieces_mask;
  unsigned long long mn = 0;
  constexpr unsigned long long row7_mask = 0xff00000000000000;
  constexpr unsigned long long row1_mask = 0x000000000000ff00;

  if (en_passant_eligible_pawn)
  {
    move_test_needed = 1;
  }

  /* If the king is not in check then we can take a shortcut to count moves
  ** for certain pawns.
  ** In order to be eligible for this shortcut, the pawn must not be pinned
  ** and must not be eligible for promotion.
  **
  ** In the code below we perform the shortcut move count on the pawns that 
  ** are eligibile, and handle the rest using the standard procedure.
  */
  if (!(in_check && move_test_needed))
  {
    constexpr unsigned long long row6_mask = 0x00ff000000000000;

    // Remove pawns eligible for promotion.
    // Remove pinned pawns.
    unsigned long long shortcut_pawns = (piece_mask & ~row6_mask);

    shortcut_pawns &= ~pin;

    if (unlikely(en_passant_eligible_pawn))
    {
      // Determine whch pawns can perform an en passant capture.
      const unsigned long long ep_mask = 1LLU << en_passant_eligible_pawn; 
      const unsigned long long ep_capable_1 = shortcut_pawns & ((ep_mask & ~col0_mask) >> 1);
      const unsigned long long ep_capable_2 = shortcut_pawns & ((ep_mask & ~col7_mask) << 1);

      if (ep_capable_1)
                       mn++;

      if (ep_capable_2)
                       mn++;

    }
    // Determine which pawns can perform a regular capture.
    unsigned long long capture_1 = shortcut_pawns & ((opponent_pieces_mask & ~col0_mask) >> 9);
    unsigned long long capture_2 = shortcut_pawns & ((opponent_pieces_mask & ~col7_mask) >> 7);

    if (in_check)
    {
      capture_1 &= (in_check >> 9);
      capture_2 &= (in_check >> 7);
    }

    mn += (unsigned long long) __builtin_popcountll (capture_1);
    mn += (unsigned long long) __builtin_popcountll (capture_2);

    /* Adjust piece mask to contain only those pawns that can't 
    ** be handled with a shortcut.
    */
    piece_mask ^= shortcut_pawns;

    /* Determine which pawns can move one square forward.
    ** The destination square must be empty in order for a pawn to be 
    ** eligible for that move.
    */
    shortcut_pawns &= ~(any_color_pieces_mask >> 8);

    // The remaining pawns can move forward one square.
    if (in_check)
    {
      unsigned long long eligible_pawns = (shortcut_pawns << 8) & in_check;
      mn += (unsigned long long) __builtin_popcountll (eligible_pawns);
    } else
    {
      mn += (unsigned long long) __builtin_popcountll (shortcut_pawns);
    }

    // Only pawns in row 1 can move forward two squares.
    shortcut_pawns &= row1_mask;

    // The second square must be empty in order for a pawn to move two rows.
    shortcut_pawns &= ~(any_color_pieces_mask >> 16);

    // The remaining pawns can move forward two squares.
    if (in_check)
    {
      unsigned long long eligible_pawns = (shortcut_pawns << 16) & in_check;
      mn += (unsigned long long) __builtin_popcountll (eligible_pawns);
    } else
    {
      mn += (unsigned long long) __builtin_popcountll (shortcut_pawns);
    }

  }

  while (unlikely(piece_mask))
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
    unsigned long long p1_move_mask;
#if defined(USE_BMI)
    const unsigned long long from_mask = _blsi_u64(piece_mask);
    piece_mask = _blsr_u64(piece_mask);
#else
    const unsigned long long from_mask = 1LLU << piece_index;
    piece_mask ^= from_mask; /* Clear the bit associated with this piece */
#endif



    /* Check upper file, one square advance. The destination square
    ** must be empty.
    */
    p1_move_mask = 1LLU << (piece_index + 8); 
    if (0 == (p1_move_mask & any_color_pieces_mask))
    {
      mn++;


      /* Special handling for pawn promotion.
      */
      if (unlikely(p1_move_mask & row7_mask))
      {
        mn += 3;

        if (TEST_NEEDED)
        {
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,S_QUEEN | S_WHITE,0,
                        from_mask,p1_move_mask,0);
          if (kingInCheck(MOVE_WHITE, piece, king_position,
                            (mover_pieces_mask ^ (from_mask | p1_move_mask)) |
                            opponent_pieces_mask))

          {
            mn -= 4;
          }
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,S_QUEEN | S_WHITE,0,
                        from_mask,p1_move_mask,0);
        }
      } else
      {
        if (!en_passant_eligible_pawn &&
            ((in_check && !(in_check & p1_move_mask)) ||
            ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
        {
          mn--;
        } else if (move_test_needed)
        {
          const unsigned long long mask1 = p1_move_mask | from_mask;
          MOVE_APPLY (piece, 1,
                      S_PAWN | S_WHITE,0,0,
                      mask1,0,0);
          if (kingInCheck(MOVE_WHITE, piece, king_position,
                            (mover_pieces_mask ^ mask1) |
                            opponent_pieces_mask))
          {
            mn--;
          }
          MOVE_APPLY (piece, 1,
                      S_PAWN | S_WHITE,0,0,
                      mask1,0,0);
        }
      }

      /* Check upper file, two square advance. The pawn must be at starting rank,
      ** and the two lower squares must be empty.
      */
      if ((from_mask & row1_mask) &&
           (0 == (any_color_pieces_mask & (p1_move_mask = (1LLU << (piece_index + 16))))))
      {
        mn++;
        if (!en_passant_eligible_pawn &&
            ((in_check && !(in_check & p1_move_mask)) ||
            ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
        {
          mn--;
        } else if (move_test_needed)
        {
          const unsigned long long mask1 = p1_move_mask | from_mask;
          MOVE_APPLY (piece, 1,
                          S_PAWN | S_WHITE,0,0,
                          mask1,0,0);
          if (kingInCheck(MOVE_WHITE, piece, king_position,
                            (mover_pieces_mask ^ mask1) |
                            opponent_pieces_mask))
          {
            mn--;
          }
          MOVE_APPLY (piece, 1,
                          S_PAWN | S_WHITE,0,0,
                          mask1,0,0);
        }
      }
    }

    /* If there are no opponent pieces in any attack position, including en passant
    ** attack position, then skip checking for capture.
    */
    if (!(opponent_pieces_mask & whitePawnCapture[piece_index]))
                                                    continue;

    unsigned int ep_index = piece_index - 1;

    /* Check Left Upper.
    ** This must be a capture or en-passant capture.
    */
    if (((piece_index & 7) > 0) &&
        (((p1_move_mask = 1LLU << ((piece_index + 8) - 1)) & opponent_pieces_mask) ||
          (en_passant_eligible_pawn == ep_index)))
    {
      unsigned int pawn_promotion = 0;
      int under_attack = 0;


      /* For en passant capture we know that the captured piece is a pawn.
      */
      if (unlikely(en_passant_eligible_pawn == ep_index))
      {
          const unsigned long long mask1 = p1_move_mask | from_mask;
          const unsigned long long mask2 = 1LLU << en_passant_eligible_pawn;
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,S_PAWN | S_BLACK,0,
                        mask1,mask2,0);
          under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                (mover_pieces_mask ^ mask1) |
                                (opponent_pieces_mask ^ mask2));

          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,S_PAWN | S_BLACK,0,
                        mask1,mask2,0);
      } else
      {
        /* Special handling for pawn promotion.
        */
        if (unlikely(p1_move_mask & row7_mask))
        {
          pawn_promotion = 1;
          if (TEST_NEEDED)
          {
            const unsigned char p2 = pieceTypeGet (MOVE_WHITE, p1_move_mask, piece);
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_WHITE,p2,S_QUEEN | S_WHITE,
                        from_mask,p1_move_mask,p1_move_mask);
            under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                (mover_pieces_mask ^ (from_mask | p1_move_mask)) |
                                (opponent_pieces_mask ^ p1_move_mask));
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_WHITE,p2,S_QUEEN | S_WHITE,
                        from_mask,p1_move_mask,p1_move_mask);
          }
        } else
        {
          if (!en_passant_eligible_pawn &&
              ((in_check && !(in_check & p1_move_mask)) ||
              ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
          {
            under_attack = 1;
          } else if (move_test_needed)
          {
            const unsigned long long mask1 = p1_move_mask | from_mask;
            const unsigned char p2 = pieceTypeGet (MOVE_WHITE, p1_move_mask, piece);
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,p1_move_mask,0);
            under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                    (mover_pieces_mask ^ mask1) |
                                    (opponent_pieces_mask ^ p1_move_mask));
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,p1_move_mask,0);
          }
        }
      }

      if (0 == under_attack)
      {
        mn++;

        /* If we had a pawn promotion then generate moves for promoting
        ** to lower ranked pieces. We don't need to do the squareUnderAttack()
        ** check for these moves.
        */
        if (unlikely(pawn_promotion))
        {
          mn += 3;
        }
      }
    }

    ep_index = piece_index + 1;

    /* Check Right Upper.
    ** This must be a capture or en-passant capture.
    */
    if (((piece_index & 7) < (BRDS-1)) &&
        (((p1_move_mask = 1LLU << ((piece_index + 8) + 1)) & opponent_pieces_mask) ||
          (en_passant_eligible_pawn == ep_index)))
    {
      unsigned int pawn_promotion = 0;
      int under_attack = 0;

      /* For en passant capture we know that the captured piece is a pawn.
      */
      if (unlikely(en_passant_eligible_pawn == ep_index))
      {
          const unsigned long long mask1 = p1_move_mask | from_mask;
          const unsigned long long mask2 = 1LLU << en_passant_eligible_pawn;
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,S_PAWN | S_BLACK,0,
                        mask1,mask2,0);
          under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                    (mover_pieces_mask ^ mask1) |
                                    (opponent_pieces_mask ^ mask2)); 
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,S_PAWN | S_BLACK,0,
                        mask1,mask2,0);
      } else
      {
        /* Special handling for pawn promotion.
        */
        if (unlikely(p1_move_mask & row7_mask))
        {
          pawn_promotion = 1;
          if (TEST_NEEDED)
          {
            const unsigned char p2 = pieceTypeGet (MOVE_WHITE, p1_move_mask, piece);
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_WHITE,p2,S_QUEEN | S_WHITE,
                        from_mask,p1_move_mask,p1_move_mask);
            under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                        (mover_pieces_mask ^ (from_mask | p1_move_mask)) |
                                        (opponent_pieces_mask ^ p1_move_mask));
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_WHITE,p2,S_QUEEN | S_WHITE,
                        from_mask,p1_move_mask,p1_move_mask);
          }
        } else
        {
          if (!en_passant_eligible_pawn &&
              ((in_check && !(in_check & p1_move_mask)) ||
              ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
          {
            under_attack = 1;
          } else if (move_test_needed)
          {
            const unsigned long long mask1 = p1_move_mask | from_mask;
            const unsigned char p2 = pieceTypeGet (MOVE_WHITE, p1_move_mask, piece);
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,p1_move_mask,0);
            under_attack = kingInCheck(MOVE_WHITE, piece, king_position,
                                        (mover_pieces_mask ^ mask1) | 
                                        (opponent_pieces_mask ^ p1_move_mask));
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_WHITE,p2,0,
                        mask1,p1_move_mask,0);
          }
        }
      }

      if (0 == under_attack)
      {
        mn++;

        /* If we had a pawn promotion then generate moves for promoting
        ** to lower ranked pieces. We don't need to do the squareUnderAttack()
        ** check for these moves.
        */
        if (unlikely(pawn_promotion))
        {
          mn += 3;
        }
      }
    }
  }
  return mn;
}

/******************************************************************************
** Find all eligible squares to which the Black pawn can move.
**
**    brd - (Input) The current position.
**    next_mv - (Input/Output) The next move.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**
** Return Values:
******************************************************************************/
__attribute__((always_inline)) inline
static unsigned long long allBlackPawnSquaresFind (
                                    unsigned long long *restrict piece,
                                    oneMove_t *const next_mv,
                                    const unsigned int en_passant_eligible_pawn,
                                    const unsigned int king_position,
                                    const unsigned int record_undo_info,
                                    const unsigned long long in_check,
                                    const unsigned long long pin,
                                    const castleEligibility_t castle_eligibility,
                                    const unsigned int depth,
                                    const unsigned int ply,
                                    const unsigned long long mover_pieces_mask,
                                    const unsigned long long opponent_pieces_mask,
                                    const unsigned int last_ply)
{
  unsigned long long piece_mask = piece[S_PAWN | S_BLACK];

  const unsigned long long any_color_pieces_mask = opponent_pieces_mask | mover_pieces_mask;
  unsigned long long mn = 0;

  while (piece_mask)
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
    unsigned int to_index;
    unsigned long long p1_move_mask;
#if defined(USE_BMI)
    const unsigned long long from_mask = _blsi_u64(piece_mask);
    piece_mask = _blsr_u64(piece_mask);
#else                                   
    const unsigned long long from_mask = 1LLU << piece_index;
    piece_mask ^= from_mask; /* Clear the bit associated with this piece */
#endif        


    constexpr unsigned long long row6_mask = 0x00ff000000000000;


    /* Check lower file, one square advance. The destination square
    ** must be empty.
    */
    to_index = piece_index - 8;
    p1_move_mask = 1LLU << to_index;
    if (0 == (p1_move_mask & any_color_pieces_mask))
    {
      unsigned char p2;
      unsigned long long mask1;
      unsigned int num_masks;
      int under_attack = 0;

      constexpr unsigned long long row0_mask = 0x00000000000000ff;

      /* Special handling for pawn promotion.
      */
      if (unlikely(p1_move_mask & row0_mask))
      {
        num_masks = 2;
        mask1 = from_mask;
        p2 = S_QUEEN | S_BLACK;
        if (TEST_NEEDED)
        {
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,p1_move_mask,0);
          under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                    (mover_pieces_mask ^ (mask1 | p1_move_mask)) |
                                    opponent_pieces_mask);
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,p1_move_mask,0);
        }

        if (!record_undo_info && (!under_attack))
        {
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,p1_move_mask,0);
          mn += (last_ply)?
             allMoveCandidatesLastPlyFind(MOVE_WHITE, piece, 0, castle_eligibility,
                                    mover_pieces_mask ^ (mask1 | p1_move_mask),
                                    opponent_pieces_mask):
             allMovePerft(MOVE_WHITE, piece, 0, castle_eligibility, depth, ply,
                                    mover_pieces_mask ^ (mask1 | p1_move_mask),
                                    opponent_pieces_mask);
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,p1_move_mask,0);
        }
      } else
      {
        num_masks = 1;
        p2 = 0;
        mask1 = p1_move_mask | from_mask;
        if (TEST_NEEDED)
        {
          if (!en_passant_eligible_pawn &&
                ((in_check && !(in_check & p1_move_mask)) ||
                ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
          {
            under_attack = 1;
          } else
          {
            MOVE_APPLY (piece, 1,
                        S_PAWN | S_BLACK,0,0,
                        mask1,0,0);
            under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                        (mover_pieces_mask ^ mask1) |
                                        opponent_pieces_mask);
            MOVE_APPLY (piece, 1,
                        S_PAWN | S_BLACK,0,0,
                        mask1,0,0);
          }
        }

        if (!record_undo_info && (!under_attack))
        {
          MOVE_APPLY (piece, 1,
                        S_PAWN | S_BLACK,0,0,
                        mask1,0,0);
          mn += (last_ply)?
             allMoveCandidatesLastPlyFind(MOVE_WHITE, piece, 0, castle_eligibility,
                            mover_pieces_mask ^ mask1, opponent_pieces_mask):
             allMovePerft(MOVE_WHITE, piece, 0, castle_eligibility, depth, ply,
                            mover_pieces_mask ^ mask1, opponent_pieces_mask);
          MOVE_APPLY (piece, 1,
                        S_PAWN | S_BLACK,0,0,
                        mask1,0,0);
        }
      }

      if (!under_attack)
      {
        if (record_undo_info)
        {
          next_mv[mn].from_r = piece_index >> 3;
          next_mv[mn].from_c = piece_index & 7;
          next_mv[mn].to_r = to_index >> 3;
          next_mv[mn].to_c = piece_index & 7;
          next_mv[mn].p1 = S_PAWN | S_BLACK;
          next_mv[mn].num_masks = num_masks;
          next_mv[mn].p2 = p2;
          next_mv[mn].mask1 = mask1;
          next_mv[mn].mask2 = p1_move_mask;
          next_mv[mn].castle_eligibility = castle_eligibility;
          next_mv[mn].en_passant_eligible_pawn = 0;
          mn++;
        }

        /* If we had a pawn promotion then generate moves for promoting
        ** to lower ranked pieces. We don't need to do the squareUnderAttack()
        ** check for these moves.
        */
        if (unlikely(num_masks == 2))
        {
          for (p2 = (S_KNIGHT | S_BLACK); p2 <= (S_ROOK | S_BLACK); p2++)
          {
            if (record_undo_info)
            {
              next_mv[mn] = next_mv[mn-1];
              next_mv[mn].p2 = p2;
              mn++;
            } else
            {
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_BLACK,p2,0,
                            mask1,p1_move_mask,0);
              mn += (last_ply)?
                   allMoveCandidatesLastPlyFind(
                                MOVE_WHITE,
                                piece,
                                0,
                                castle_eligibility,
                                mover_pieces_mask ^ (mask1 | p1_move_mask),
                                opponent_pieces_mask):
                   allMovePerft(
                                MOVE_WHITE,
                                piece,
                                0,
                                castle_eligibility,
                                depth,
                                ply,
                                mover_pieces_mask ^ (mask1 | p1_move_mask),
                                opponent_pieces_mask);
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_BLACK,p2,0,
                            mask1,p1_move_mask,0);
            }
          }
        }
      }

      /* Check lower file, two square advance. The pawn must be at starting rank,
      ** and the two lower squares must be empty.
      */
      to_index = piece_index - 16;
      if ((from_mask & row6_mask) &&
          (0 == ((p1_move_mask = 1LLU << to_index) & any_color_pieces_mask)))

      {
        under_attack = 0;

        if (TEST_NEEDED)
        {
          if (!en_passant_eligible_pawn &&
               ((in_check && !(in_check & p1_move_mask)) ||
               ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
          {
            under_attack = 1;
          } else
          {
            mask1 = p1_move_mask | from_mask;
            MOVE_APPLY (piece, 1,
                          S_PAWN | S_BLACK,0,0,
                          mask1,0,0);
            under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                    (mover_pieces_mask ^ mask1) |
                                    opponent_pieces_mask);
            MOVE_APPLY (piece, 1,
                          S_PAWN | S_BLACK,0,0,
                          mask1,0,0);
          }
        }
        if (!under_attack)
        {
          /* The pawn moved two squares, so check if it is eligible for
          ** en passant capture. In order to be eligible, there must be an
          ** opponent pawn in the same row.
          */
          const unsigned int next_en_passant_eligible_pawn =
          (whitePawnEnPassantAttack[to_index] & piece[S_PAWN | S_WHITE])?to_index:0;

          if (!record_undo_info)
          {
            mask1 = p1_move_mask | from_mask;

            MOVE_APPLY (piece, 1,
                          S_PAWN | S_BLACK,0,0,
                          mask1,0,0);
            mn += (last_ply)?
                     allMoveCandidatesLastPlyFind(
                                  MOVE_WHITE,
                                  piece,
                                  next_en_passant_eligible_pawn,
                                  castle_eligibility,
                                  mover_pieces_mask ^ mask1, 
                                  opponent_pieces_mask):
                     allMovePerft(
                                  MOVE_WHITE,
                                  piece,
                                  next_en_passant_eligible_pawn,
                                  castle_eligibility,
                                  depth,
                                  ply,
                                  mover_pieces_mask ^ mask1, 
                                  opponent_pieces_mask);
            MOVE_APPLY (piece, 1,
                          S_PAWN | S_BLACK,0,0,
                          mask1,0,0);
          } else
          {
            next_mv[mn].from_r = piece_index >> 3;
            next_mv[mn].from_c = piece_index & 7;
            next_mv[mn].to_r = to_index >> 3;
            next_mv[mn].to_c = piece_index & 7;
            next_mv[mn].p1 = S_PAWN | S_BLACK;
            next_mv[mn].num_masks = 1;
            next_mv[mn].mask1 = p1_move_mask | from_mask;
            next_mv[mn].castle_eligibility = castle_eligibility;
            next_mv[mn].en_passant_eligible_pawn = next_en_passant_eligible_pawn;
            mn++;
          }
        }
      }
    }



    /* If there are no opponent pieces in any attack position, including en passant
    ** attack position, then skip checking for capture.
    */
    if (!(opponent_pieces_mask & blackPawnCapture[piece_index]))
                                                    continue;

    /* Check left Lower.
    ** This must be a capture or en-passant capture.
    */
    to_index = (piece_index - 8) - 1;
    if (((piece_index & 7) > 0) &&
        (((p1_move_mask = (1LLU << to_index)) & opponent_pieces_mask) ||
          (en_passant_eligible_pawn == (piece_index - 1))))
    {
      unsigned char p2;
      unsigned long long mask2;
      unsigned int num_masks = 2;
      unsigned long long mask1 = p1_move_mask | from_mask;
      unsigned char p3 = 0;
      int under_attack = 0;
      castleEligibility_t next_castle_eligibility = castle_eligibility;
      unsigned long long new_mover_pieces_mask;
      unsigned long long new_opponent_pieces_mask;


      /* For en passant capture we know that the captured piece is a pawn.
      */
      if (en_passant_eligible_pawn == (piece_index - 1))
      {
        p2 = S_PAWN | S_WHITE;
        mask2 = 1LLU << en_passant_eligible_pawn;
        new_mover_pieces_mask = mover_pieces_mask ^ mask1;
        new_opponent_pieces_mask = opponent_pieces_mask ^ mask2;
        if (TEST_NEEDED)
        {
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,mask2,0);
          under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                new_mover_pieces_mask |
                                new_opponent_pieces_mask);
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,mask2,0);
        }
      } else
      {
        p2 = pieceTypeGet (MOVE_BLACK, p1_move_mask, piece);
        mask2 = p1_move_mask;
        new_opponent_pieces_mask = opponent_pieces_mask ^ mask2;

        constexpr unsigned long long row0_mask = 0x00000000000000ff;

        /* Special handling for pawn promotion.
        */
        if (unlikely(p1_move_mask & row0_mask))
        {
          if (unlikely(PIECE_GET(p2) == S_ROOK))
          {
            castleEligibilityRookCaptureCheck (MOVE_BLACK,
                                        to_index, 
                                        &next_castle_eligibility);
          }
          num_masks = 3;
          mask1 = from_mask;
          p3 = S_QUEEN | S_BLACK;
          new_mover_pieces_mask = mover_pieces_mask ^ (mask1 | p1_move_mask);
          if (TEST_NEEDED)
          {
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_BLACK,p2,p3,
                        mask1,mask2,p1_move_mask);
            under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                    new_mover_pieces_mask |
                                    new_opponent_pieces_mask);
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_BLACK,p2,p3,
                        mask1,mask2,p1_move_mask);
          }
        } else
        {
          new_mover_pieces_mask = mover_pieces_mask ^ mask1;
          if (TEST_NEEDED)
          {
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,mask2,0);
            under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                        new_mover_pieces_mask |
                                        new_opponent_pieces_mask);
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,mask2,0);
          }
        }
      }

      if (likely(!under_attack))
      {
        if (record_undo_info)
        {
          next_mv[mn].from_r = piece_index >> 3;
          next_mv[mn].from_c = piece_index & 7;
          next_mv[mn].to_r = to_index >> 3;
          next_mv[mn].to_c = to_index & 7;
          next_mv[mn].p1 = S_PAWN | S_BLACK;
          next_mv[mn].num_masks = num_masks;
          next_mv[mn].p2 = p2;
          next_mv[mn].p3 = p3;
          next_mv[mn].mask1 = mask1;
          next_mv[mn].mask2 = mask2;
          next_mv[mn].mask3 = p1_move_mask;
          next_mv[mn].castle_eligibility = next_castle_eligibility;
          next_mv[mn].en_passant_eligible_pawn = 0;
          mn++;
        } else
        {
          MOVE_APPLY (piece, num_masks,
                        S_PAWN | S_BLACK,p2,p3,
                        mask1,mask2, p1_move_mask);
          mn += (last_ply)?
                   allMoveCandidatesLastPlyFind(
                                MOVE_WHITE,
                                piece,
                                0,
                                next_castle_eligibility,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask):
                   allMovePerft(
                                MOVE_WHITE,
                                piece,
                                0,
                                next_castle_eligibility,
                                depth,
                                ply,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask);
          MOVE_APPLY (piece, num_masks,
                        S_PAWN | S_BLACK,p2,p3,
                        mask1,mask2, p1_move_mask);
        } 


        /* If we had a pawn promotion then generate moves for promoting
        ** to lower ranked pieces. We don't need to do the squareUnderAttack()
        ** check for these moves.
        */
        if (unlikely(num_masks == 3))
        {
          for (p3 = (S_KNIGHT | S_BLACK); p3 <= (S_ROOK | S_BLACK); p3++)
          {
            if (record_undo_info)
            {
              next_mv[mn] = next_mv[mn-1];
              next_mv[mn].p3 = p3;
              mn++;
            } else
            {
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_BLACK,p2,p3,
                            mask1,mask2, p1_move_mask);
              mn += (last_ply)?
                   allMoveCandidatesLastPlyFind(
                                MOVE_WHITE,
                                piece,
                                0,
                                next_castle_eligibility,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask):
                   allMovePerft(
                                MOVE_WHITE,
                                piece,
                                0,
                                next_castle_eligibility,
                                depth,
                                ply,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask);
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_BLACK,p2,p3,
                            mask1,mask2, p1_move_mask);
            } 
          }
        }
      }
    }
    /* Check right Lower.
    ** This must be a capture or en-passant capture.
    */

    to_index = (piece_index - 8) + 1;
    p1_move_mask = 1LLU << to_index;
    if (((piece_index & 7) < (BRDS-1)) &&
        ((p1_move_mask & opponent_pieces_mask) ||
          (en_passant_eligible_pawn == (piece_index + 1))))
    {
      unsigned char p2;
      unsigned long long mask2;
      unsigned int num_masks = 2;
      unsigned long long mask1 = p1_move_mask | from_mask;
      unsigned char p3 = 0;
      int under_attack = 0;
      castleEligibility_t next_castle_eligibility = castle_eligibility;
      unsigned long long new_mover_pieces_mask;
      unsigned long long new_opponent_pieces_mask;

      /* For en passant capture we know that the captured piece is a pawn.
      */
      if (en_passant_eligible_pawn == (piece_index + 1))
      {
        p2 = S_PAWN | S_WHITE;
        mask2 = 1LLU << en_passant_eligible_pawn;
        new_mover_pieces_mask = mover_pieces_mask ^ mask1;
        new_opponent_pieces_mask = opponent_pieces_mask ^ mask2;
        if (TEST_NEEDED)
        {
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,mask2,0);
          under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                            new_mover_pieces_mask |
                            new_opponent_pieces_mask);
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,mask2,0);
        }
      } else
      {
        p2 = pieceTypeGet (MOVE_BLACK, p1_move_mask, piece);
        mask2 = p1_move_mask;
        new_opponent_pieces_mask = opponent_pieces_mask ^ mask2;

        constexpr unsigned long long row0_mask = 0x00000000000000ff;

        /* Special handling for pawn promotion.
        */
        if (unlikely(p1_move_mask & row0_mask))
        {
          if (unlikely(PIECE_GET(p2) == S_ROOK))
          {
            castleEligibilityRookCaptureCheck (MOVE_BLACK,
                                        to_index, 
                                        &next_castle_eligibility);
          }
          num_masks = 3;
          mask1 = from_mask;
          p3 = S_QUEEN | S_BLACK;
          new_mover_pieces_mask = mover_pieces_mask ^ (mask1 | p1_move_mask);
          if (TEST_NEEDED)
          {
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_BLACK,p2,p3,
                        mask1,mask2,p1_move_mask);
            under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                    new_mover_pieces_mask |
                                    new_opponent_pieces_mask);
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_BLACK,p2,p3,
                        mask1,mask2,p1_move_mask);
          }
        } else
        {
          new_mover_pieces_mask = mover_pieces_mask ^ mask1;
          if (TEST_NEEDED)
          {
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,mask2,0);
            under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                new_mover_pieces_mask |
                                new_opponent_pieces_mask);
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,mask2,0);
          }
        }
      }

      if (likely(!under_attack))
      {
        if (record_undo_info)
        {
          next_mv[mn].from_r = piece_index >> 3;
          next_mv[mn].from_c = piece_index & 7;
          next_mv[mn].to_r = to_index >> 3;
          next_mv[mn].to_c = to_index & 7;
          next_mv[mn].p1 = S_PAWN | S_BLACK;
          next_mv[mn].num_masks = num_masks;
          next_mv[mn].p2 = p2;
          next_mv[mn].p3 = p3;
          next_mv[mn].mask1 = mask1;
          next_mv[mn].mask2 = mask2;
          next_mv[mn].mask3 = p1_move_mask;
          next_mv[mn].castle_eligibility = next_castle_eligibility;
          next_mv[mn].en_passant_eligible_pawn = 0;
          mn++;
        } else
        {
          MOVE_APPLY (piece, num_masks,
                        S_PAWN | S_BLACK,p2,p3,
                        mask1,mask2, p1_move_mask);
          mn += (last_ply)?
                   allMoveCandidatesLastPlyFind(
                                MOVE_WHITE,
                                piece,
                                0,
                                next_castle_eligibility,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask):
                   allMovePerft(
                                MOVE_WHITE,
                                piece,
                                0,
                                next_castle_eligibility,
                                depth,
                                ply,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask);
          MOVE_APPLY (piece, num_masks,
                        S_PAWN | S_BLACK,p2,p3,
                        mask1,mask2, p1_move_mask);
        } 


        /* If we had a pawn promotion then generate moves for promoting
        ** to lower ranked pieces. We don't need to do the squareUnderAttack()
        ** check for these moves.
        */
        if (unlikely(num_masks == 3))
        {
          for (p3 = (S_KNIGHT | S_BLACK); p3 <= (S_ROOK | S_BLACK); p3++)
          {
            if (record_undo_info)
            {
              next_mv[mn] = next_mv[mn-1];
              next_mv[mn].p3 = p3;
              mn++;
            } else
            {
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_BLACK,p2,p3,
                            mask1,mask2, p1_move_mask);
              mn += (last_ply)?
                   allMoveCandidatesLastPlyFind(
                                MOVE_WHITE,
                                piece,
                                0,
                                next_castle_eligibility,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask):
                   allMovePerft(
                                MOVE_WHITE,
                                piece,
                                0,
                                next_castle_eligibility,
                                depth,
                                ply,
                                new_mover_pieces_mask,
                                new_opponent_pieces_mask);
              MOVE_APPLY (piece, num_masks,
                            S_PAWN | S_BLACK,p2,p3,
                            mask1,mask2, p1_move_mask);
            } 
          }
        }
      }
    }
  }
  return mn;
}

/******************************************************************************
** Find all eligible squares to which the Black pawn can move.
**
**    brd - (Input) The current position.
**    next_mv - (Input/Output) The next move.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**
** Return Values:
******************************************************************************/
__attribute__((always_inline)) inline
static unsigned long long allBlackPawnSquaresLastPlyFind (
                                    unsigned long long *restrict piece,
                                    const unsigned int en_passant_eligible_pawn,
                                    const unsigned int king_position,
                                    const unsigned long long in_check,
                                    const unsigned long long pin,
                                    unsigned long long move_test_needed,
                                    const unsigned long long mover_pieces_mask,
                                    const unsigned long long opponent_pieces_mask)
{
  unsigned long long piece_mask = piece[S_PAWN | S_BLACK];

  const unsigned long long any_color_pieces_mask = opponent_pieces_mask | mover_pieces_mask;
  unsigned long long mn = 0;
  constexpr unsigned long long row0_mask = 0x00000000000000ff;
  constexpr unsigned long long row6_mask = 0x00ff000000000000;

  if (en_passant_eligible_pawn)
  {
    move_test_needed = 1;
  }

  /* If the king is not in check then we can take a shortcut to count moves
  ** for certain pawns.
  ** In order to be eligible for this shortcut, the pawn must not be pinned
  ** and must not be eligible for promotion.
  **
  ** In the code below we perform the shortcut move count on the pawns that
  ** are eligibile, and handle the rest using the standard procedure.
  */
  if (!(in_check && move_test_needed))
  {
    constexpr unsigned long long row1_mask = 0x000000000000ff00;

    // Remove pawns eligible for promotion.
    // Remove pinned pawns.
    unsigned long long shortcut_pawns = (piece_mask & ~row1_mask);

    shortcut_pawns &= ~pin;

    // Determine whch pawns can perform an en passant capture.
    if (unlikely(en_passant_eligible_pawn))
    {
      const unsigned long long ep_mask = 1LLU << en_passant_eligible_pawn; 
      const unsigned long long ep_capable_1 = shortcut_pawns & ((ep_mask & ~col0_mask) >> 1);
      const unsigned long long ep_capable_2 = shortcut_pawns & ((ep_mask & ~col7_mask) << 1);

      if (ep_capable_1)
                       mn++;

      if (ep_capable_2)
                       mn++;

    }

    // Determine which pawns can perform a regular capture.
    unsigned long long capture_1 = shortcut_pawns & ((opponent_pieces_mask & ~col0_mask) << 7);
    unsigned long long capture_2 = shortcut_pawns & ((opponent_pieces_mask & ~col7_mask) << 9);

    if (in_check)
    { 
      capture_1 &= in_check << 7;
      capture_2 &= in_check << 9;
    }

    mn += (unsigned long long) __builtin_popcountll (capture_1);
    mn += (unsigned long long) __builtin_popcountll (capture_2);

    /* Adjust piece mask to contain only those pawns that can't 
    ** be handled with a shortcut.
    */
    piece_mask ^= shortcut_pawns;

    /* Determine which pawns can move one square forward.
    ** The destination square must be empty in order for a pawn to be 
    ** eligible for that move.
    */
    shortcut_pawns &= ~(any_color_pieces_mask << 8);

    // The remaining pawns can move forward one square.
    if (in_check)
    {       
      unsigned long long eligible_pawns = (shortcut_pawns >> 8) & in_check;
      mn += (unsigned long long) __builtin_popcountll (eligible_pawns);
    } else  
    {     
      mn += (unsigned long long) __builtin_popcountll (shortcut_pawns);
    }                   

    // Only pawns in row 6 can move forward two squares.
    shortcut_pawns &= row6_mask;

    // The second square must be empty in order for a pawn to move two rows.
    shortcut_pawns &= ~(any_color_pieces_mask << 16);

    // The remaining pawns can move forward two squares.
    if (in_check)
    {
      unsigned long long eligible_pawns = (shortcut_pawns >> 16) & in_check;
      mn += (unsigned long long) __builtin_popcountll (eligible_pawns);
    } else
    {
      mn += (unsigned long long) __builtin_popcountll (shortcut_pawns);
    }

  }

  while (unlikely(piece_mask))
  {
    const unsigned int piece_index = bitbrdLowestIndexFromMaskGet(piece_mask);
    unsigned long long p1_move_mask;
#if defined(USE_BMI)
    const unsigned long long from_mask = _blsi_u64(piece_mask);
    piece_mask = _blsr_u64(piece_mask);
#else
    const unsigned long long from_mask = 1LLU << piece_index;
    piece_mask ^= from_mask; /* Clear the bit associated with this piece */
#endif




    /* Check lower file, one square advance. The destination square
    ** must be empty.
    */
    p1_move_mask = 1LLU << (piece_index - 8);
    if (0 == (p1_move_mask & any_color_pieces_mask))
    {
      mn++;

      /* Special handling for pawn promotion.
      */
      if (unlikely(p1_move_mask & row0_mask))
      {
        mn += 3;
        if (TEST_NEEDED)
        {
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,S_QUEEN | S_BLACK,0,
                        from_mask,p1_move_mask,0);
          if (kingInCheck(MOVE_BLACK, piece, king_position,
                            (mover_pieces_mask ^ (from_mask | p1_move_mask)) |
                            opponent_pieces_mask))
          {
            mn -= 4;
          }
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,S_QUEEN | S_BLACK,0,
                        from_mask,p1_move_mask,0);
        }
      } else
      {
        if (!en_passant_eligible_pawn &&
            ((in_check && !(in_check & p1_move_mask)) ||
            ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
        {
          mn--;
        } else if (move_test_needed)
        {
          const unsigned long long mask1 = p1_move_mask | from_mask;
          MOVE_APPLY (piece, 1,
                        S_PAWN | S_BLACK,0,0,
                        mask1,0,0);
          if (kingInCheck(MOVE_BLACK, piece, king_position,
                            (mover_pieces_mask ^ mask1) |
                            opponent_pieces_mask))
          {
            mn--;
          }
          MOVE_APPLY (piece, 1,
                        S_PAWN | S_BLACK,0,0,
                        mask1,0,0);
        }
      }

      /* Check lower file, two square advance. The pawn must be at starting rank,
      ** and the two lower squares must be empty.
      */
      if ((from_mask & row6_mask) &&
          (0 == ((p1_move_mask = 1LLU << (piece_index - 16)) & any_color_pieces_mask)))
      {
        mn++;
        if (!en_passant_eligible_pawn &&
            ((in_check && !(in_check & p1_move_mask)) ||
            ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
        {
          mn--;
        } else if (move_test_needed)
        {
          const unsigned long long mask1 = p1_move_mask | from_mask;
          MOVE_APPLY (piece, 1,
                          S_PAWN | S_BLACK,0,0,
                          mask1,0,0);
          if (kingInCheck(MOVE_BLACK, piece, king_position,
                        (mover_pieces_mask ^ mask1) | 
                        opponent_pieces_mask))
          {
            mn--;
          }
          MOVE_APPLY (piece, 1,
                          S_PAWN | S_BLACK,0,0,
                          mask1,0,0);
        }
      }
    }

    /* If there are no opponent pieces in any attack position, including en passant
    ** attack position, then skip checking for capture.
    */
    if (!(opponent_pieces_mask & blackPawnCapture[piece_index]))
                                                    continue;

    unsigned int ep_index = piece_index - 1;

    /* Check left Lower.
    ** This must be a capture or en-passant capture.
    */
    if (((piece_index & 7) > 0) &&
        (((p1_move_mask = 1LLU << ((piece_index - 8) - 1)) & opponent_pieces_mask) ||
          (en_passant_eligible_pawn == ep_index)))
    {
      unsigned int pawn_promotion = 0;
      int under_attack = 0;


      /* For en passant capture we know that the captured piece is a pawn.
      */
      if (en_passant_eligible_pawn == ep_index)
      {
          const unsigned long long mask1 = p1_move_mask | from_mask;
          unsigned long long mask2 = 1LLU << en_passant_eligible_pawn;
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,S_PAWN | S_WHITE,0,
                        mask1,mask2,0);
          under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                (mover_pieces_mask ^ mask1) |
                                (opponent_pieces_mask ^ mask2));
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,S_PAWN | S_WHITE,0,
                        mask1,mask2,0);
      } else
      {

        /* Special handling for pawn promotion.
        */
        if (unlikely(p1_move_mask & row0_mask))
        {
          pawn_promotion = 1;
          if (TEST_NEEDED)
          {
            const unsigned char p2 = pieceTypeGet (MOVE_BLACK, p1_move_mask, piece);
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_BLACK,p2,S_QUEEN | S_BLACK,
                        from_mask,p1_move_mask,p1_move_mask);
            under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                    (mover_pieces_mask ^ (from_mask | p1_move_mask)) |
                                    (opponent_pieces_mask ^ p1_move_mask));
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_BLACK,p2,S_QUEEN | S_BLACK,
                        from_mask,p1_move_mask,p1_move_mask);
          }
        } else
        {
          if (!en_passant_eligible_pawn &&
              ((in_check && !(in_check & p1_move_mask)) ||
              ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
          {
            under_attack = 1;
          } else if (move_test_needed)
          {
            const unsigned long long mask1 = p1_move_mask | from_mask;
            const unsigned char p2 = pieceTypeGet (MOVE_BLACK, p1_move_mask, piece);
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,p1_move_mask,0);
            under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                    (mover_pieces_mask ^ mask1) |
                                    (opponent_pieces_mask ^ p1_move_mask));
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,p1_move_mask,0);
          }
        }
      }

      if (0 == under_attack)
      {
        mn++;

        /* If we had a pawn promotion then generate moves for promoting
        ** to lower ranked pieces. We don't need to do the squareUnderAttack()
        ** check for these moves.
        */
        if (unlikely(pawn_promotion))
        {
          mn += 3;
        }
      }
    }
    /* Check right Lower.
    ** This must be a capture or en-passant capture.
    */
    ep_index = piece_index + 1;

    if (((piece_index & 7) < (BRDS-1)) &&
        (((p1_move_mask = 1LLU << ((piece_index - 8) + 1)) & opponent_pieces_mask) ||
          (en_passant_eligible_pawn == ep_index)))
    {
      unsigned int pawn_promotion = 0;
      int under_attack = 0;

      /* For en passant capture we know that the captured piece is a pawn.
      */
      if (en_passant_eligible_pawn == ep_index)
      {
          const unsigned long long mask1 = p1_move_mask | from_mask;
          const unsigned long long mask2 = 1LLU << en_passant_eligible_pawn;
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,S_PAWN | S_WHITE,0,
                        mask1,mask2,0);
          under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                    (mover_pieces_mask ^ mask1) |
                                    (opponent_pieces_mask ^ mask2));
          MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,S_PAWN | S_WHITE,0,
                        mask1,mask2,0);
      } else
      {
        /* Special handling for pawn promotion.
        */
        if (unlikely(p1_move_mask & row0_mask))
        {
          pawn_promotion = 1;
          if (TEST_NEEDED)
          {
            const unsigned char p2 = pieceTypeGet (MOVE_BLACK, p1_move_mask, piece);

            MOVE_APPLY (piece, 3,
                        S_PAWN | S_BLACK,p2,S_QUEEN | S_BLACK,
                        from_mask,p1_move_mask,p1_move_mask);
            under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                    (mover_pieces_mask ^ (from_mask | p1_move_mask)) |
                                    (opponent_pieces_mask ^ p1_move_mask));
            MOVE_APPLY (piece, 3,
                        S_PAWN | S_BLACK,p2,S_QUEEN | S_BLACK,
                        from_mask,p1_move_mask,p1_move_mask);
          }
        } else
        {
          if (!en_passant_eligible_pawn &&
              ((in_check && !(in_check & p1_move_mask)) ||
              ((pin && (pin & from_mask)) && !(pin & p1_move_mask))))
          {
            under_attack = 1;
          } else if (move_test_needed)
          {
            const unsigned long long mask1 = p1_move_mask | from_mask;
            const unsigned char p2 = pieceTypeGet (MOVE_BLACK, p1_move_mask, piece);

            MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,p1_move_mask,0);
            under_attack = kingInCheck(MOVE_BLACK, piece, king_position,
                                    (mover_pieces_mask ^ mask1) |
                                    (opponent_pieces_mask ^ p1_move_mask));
            MOVE_APPLY (piece, 2,
                        S_PAWN | S_BLACK,p2,0,
                        mask1,p1_move_mask,0);
          }
        }
      }

      if (0 == under_attack)
      {
        mn++;

        /* If we had a pawn promotion then generate moves for promoting
        ** to lower ranked pieces. We don't need to do the squareUnderAttack()
        ** check for these moves.
        */
        if (unlikely(pawn_promotion))
        {
          mn += 3;
        }
      }
    }
  }
  return mn;
}

/******************************************************************************
** Generate a list of all move candidates in the position.
**
**    whose_move (Input) The moving side.
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
                           const castleEligibility_t castle_eligibility)
{
  unsigned long long num_moves = 0;
  unsigned int king_position;
  unsigned long long mover_pieces_mask;
  unsigned long long opponent_pieces_mask;
  unsigned long long any_color_pieces_mask;
  unsigned long long king_mask;
  kingAttackHelper_t attack_helper;
  unsigned long long king_attack_mask;


  if (whose_move == MOVE_WHITE)
  {
    king_mask = bit_brd->piece[S_KING | S_WHITE];
    king_position = bitbrdLowestIndexFromMaskGet(king_mask);
    king_attack_mask = kingAttack[king_position];

    opponent_pieces_mask = bit_brd->color[MOVE_BLACK];
    mover_pieces_mask = bit_brd->color[MOVE_WHITE];
    any_color_pieces_mask = mover_pieces_mask | opponent_pieces_mask;

    /* Determine if the king is currently in check. When the king is not in check
    ** we can take a short cut on testing the legality of potential moves.
    ** When the king is not in check and there is a pin then the pin output
    ** parameter is set to the pin mask.
    */
    attack_helper = pinCompute (
                            MOVE_WHITE, bit_brd->piece, king_position, king_mask,
                            en_passant_eligible_pawn,
                            mover_pieces_mask, any_color_pieces_mask,
                            king_attack_mask, castle_eligibility);

    if (!attack_helper.in_check && !attack_helper.pin)
    {
      num_moves += whiteKingSquaresFind(king_position, attack_helper.move_candidate_mask,
                       bit_brd->piece, &dest[num_moves],
                       castle_eligibility, 1, 0, 0,
                       mover_pieces_mask, opponent_pieces_mask, 0,
                       king_attack_mask);

      num_moves += allWhitePawnSquaresFind(bit_brd->piece, &dest[num_moves],
                                    en_passant_eligible_pawn, king_position,
                                    1, 0, 0,
                                    castle_eligibility, 0, 0,
                                    mover_pieces_mask, opponent_pieces_mask, 0);
      num_moves += allKnightSquaresFind(MOVE_WHITE, bit_brd->piece, &dest[num_moves], king_position,
                                        1, 0, 0, 0,
                                        castle_eligibility, 0, 0,
                                        mover_pieces_mask, opponent_pieces_mask, 0);
      num_moves += allBishopRookQueenSquaresFind(MOVE_WHITE, bit_brd->piece,
                                        &dest[num_moves], king_position, 1, 0, 0, 0,
                                        castle_eligibility, 0, 0,
                                        mover_pieces_mask, opponent_pieces_mask, 0);
    } else
    {
      num_moves += whiteKingSquaresFind(king_position, attack_helper.move_candidate_mask,
                       bit_brd->piece, &dest[num_moves],
                       castle_eligibility, 1, 0, 0,
                       mover_pieces_mask, opponent_pieces_mask, 0,
                       king_attack_mask);

      num_moves += allWhitePawnSquaresFind(bit_brd->piece, &dest[num_moves],
                                    en_passant_eligible_pawn, king_position,
                                    1, attack_helper.in_check, attack_helper.pin,
                                    castle_eligibility, 0, 0,
                                    mover_pieces_mask, opponent_pieces_mask, 0);
      num_moves += allKnightSquaresFind(MOVE_WHITE, bit_brd->piece, &dest[num_moves], king_position,
                                        1, attack_helper.in_check, attack_helper.pin, attack_helper.move_test_needed,
                                        castle_eligibility, 0, 0, 
                                        mover_pieces_mask, opponent_pieces_mask, 0);
      num_moves += allBishopRookQueenSquaresFind(MOVE_WHITE, bit_brd->piece,
                &dest[num_moves], king_position, 1, attack_helper.in_check, attack_helper.pin, attack_helper.move_test_needed,
                castle_eligibility, 0, 0,
                mover_pieces_mask, opponent_pieces_mask, 0);
    }
  } else 
  {
    king_mask = bit_brd->piece[S_KING | S_BLACK];
    king_position = bitbrdLowestIndexFromMaskGet(king_mask);
    king_attack_mask = kingAttack[king_position];

    mover_pieces_mask = bit_brd->color[MOVE_BLACK];
    opponent_pieces_mask = bit_brd->color[MOVE_WHITE];
    any_color_pieces_mask = mover_pieces_mask | opponent_pieces_mask;

    /* Determine if the king is currently in check. When the king is not in check
    ** we can take a short cut on testing the legality of potential moves.
    ** When the king is not in check and there is a pin then the pin output
    ** parameter is set to the pin mask.
    */
    attack_helper = pinCompute (
                            MOVE_BLACK, bit_brd->piece, king_position, king_mask,
                            en_passant_eligible_pawn,
                            mover_pieces_mask, any_color_pieces_mask,
                            king_attack_mask, castle_eligibility);

    if (!attack_helper.in_check && !attack_helper.pin)
    {
      num_moves += blackKingSquaresFind(king_position, attack_helper.move_candidate_mask,
                       bit_brd->piece, &dest[num_moves],
                       castle_eligibility, 1, 0, 0,
                       mover_pieces_mask, opponent_pieces_mask, 0,
                       king_attack_mask);

      num_moves += allBlackPawnSquaresFind(bit_brd->piece, &dest[num_moves],
                                    en_passant_eligible_pawn, king_position,
                                    1, 0, 0,
                                    castle_eligibility, 0, 0,
                                    mover_pieces_mask, opponent_pieces_mask, 0);
      num_moves += allKnightSquaresFind(MOVE_BLACK, bit_brd->piece, &dest[num_moves], king_position,
                                        1, 0, 0, 0,
                                        castle_eligibility, 0, 0,
                                        mover_pieces_mask, opponent_pieces_mask, 0);
      num_moves += allBishopRookQueenSquaresFind(MOVE_BLACK, bit_brd->piece,
                                        &dest[num_moves], king_position, 1, 0, 0, 0,
                                        castle_eligibility, 0, 0,
                                        mover_pieces_mask, opponent_pieces_mask, 0);
    } else
    {
      num_moves += blackKingSquaresFind(king_position, attack_helper.move_candidate_mask,
                       bit_brd->piece, &dest[num_moves],
                       castle_eligibility, 1, 0, 0,
                       mover_pieces_mask, opponent_pieces_mask, 0,
                       king_attack_mask);

      num_moves += allBlackPawnSquaresFind(bit_brd->piece, &dest[num_moves],
                                    en_passant_eligible_pawn, king_position,
                                    1, attack_helper.in_check, attack_helper.pin,
                                    castle_eligibility, 0, 0,
                                    mover_pieces_mask, opponent_pieces_mask, 0);
      num_moves += allKnightSquaresFind(MOVE_BLACK, bit_brd->piece, &dest[num_moves], king_position,
                                        1, attack_helper.in_check, attack_helper.pin, attack_helper.move_test_needed,
                                        castle_eligibility, 0, 0,
                                        mover_pieces_mask, opponent_pieces_mask, 0);
      num_moves += allBishopRookQueenSquaresFind(MOVE_BLACK, bit_brd->piece,
             &dest[num_moves], king_position, 1, attack_helper.in_check, attack_helper.pin, attack_helper.move_test_needed,
             castle_eligibility, 0, 0,
             mover_pieces_mask, opponent_pieces_mask, 0);
    }
  } 



  return num_moves;
}

/******************************************************************************
** Generate a list of all white pieces move candidates in the position.
**
**    bit_brd - (Input) The bit board of the current position.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**    castle_eligibility - (Input) - Flags indicating who is eligible to castle.
**
** Return Values:
** Total number of move candidates in this position. Note that this could be 0.
******************************************************************************/
__attribute__((noinline)) 
unsigned long long allWhiteMovePerft (
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const castleEligibility_t castle_eligibility,
                           const unsigned int depth,
                           unsigned int ply,
                           const unsigned long long mover_pieces_mask,
                           const unsigned long long opponent_pieces_mask)
{
  unsigned long long num_moves = 0;
  kingAttackHelper_t attack_helper;
  const unsigned int next_ply_is_last = (depth == ++ply);
  const unsigned long long  king_mask = piece[S_KING | S_WHITE];
  const unsigned int   king_position = bitbrdLowestIndexFromMaskGet(king_mask);
  const unsigned long long  king_attack_mask = kingAttack[king_position];
  const unsigned long long any_color_pieces_mask = opponent_pieces_mask | mover_pieces_mask;

    /* Determine if the king is currently in check. When the king is not in check 
    ** we can take a short cut on testing the legality of potential moves.
    ** When the king is not in check and there is a pin then the pin output 
    ** parameter is set to the pin mask.
    */
  attack_helper = pinCompute (
                            MOVE_WHITE, piece, king_position, king_mask,
                            en_passant_eligible_pawn,
                            mover_pieces_mask, any_color_pieces_mask,
                            king_attack_mask, castle_eligibility);

    if (!attack_helper.in_check && !attack_helper.pin)
    {
      if (next_ply_is_last)
      {
        num_moves += whiteKingSquaresFind(king_position, attack_helper.move_candidate_mask,
                       piece, 0,
                       castle_eligibility, 0, depth, ply,
                       mover_pieces_mask, opponent_pieces_mask, 1,
                       king_attack_mask);

        num_moves += allWhitePawnSquaresFind(piece, 0,
                                    en_passant_eligible_pawn, king_position,
                                    0, 0, 0,
                                    castle_eligibility, depth, ply,
                                    mover_pieces_mask, opponent_pieces_mask, 1);
        num_moves += allKnightSquaresFind(MOVE_WHITE, piece, 0, king_position,
                                        0, 0, 0, 0,
                                        castle_eligibility, depth, ply,
                                        mover_pieces_mask, opponent_pieces_mask, 1);
        num_moves += allBishopRookQueenSquaresFind(MOVE_WHITE, piece,
                                        0, king_position, 0, 0, 0, 0,
                                        castle_eligibility, depth, ply,
                                        mover_pieces_mask, opponent_pieces_mask, 1);
      } else
      {
        num_moves += whiteKingSquaresFind(king_position, attack_helper.move_candidate_mask,
                       piece, 0,
                       castle_eligibility, 0, depth, ply,
                       mover_pieces_mask, opponent_pieces_mask, 0,
                       king_attack_mask);

        num_moves += allWhitePawnSquaresFind(piece, 0,
                                    en_passant_eligible_pawn, king_position,
                                    0, 0, 0,
                                    castle_eligibility, depth, ply,
                                    mover_pieces_mask, opponent_pieces_mask, 0);
        num_moves += allKnightSquaresFind(MOVE_WHITE, piece, 0, king_position,
                                        0, 0, 0, 0,
                                        castle_eligibility, depth, ply,
                                        mover_pieces_mask, opponent_pieces_mask, 0);
        num_moves += allBishopRookQueenSquaresFind(MOVE_WHITE, piece,
                                        0, king_position, 0, 0, 0, 0,
                                        castle_eligibility, depth, ply,
                                        mover_pieces_mask, opponent_pieces_mask, 0);
      }
    } else
    {
      num_moves += whiteKingSquaresFind(king_position, attack_helper.move_candidate_mask,
                       piece, 0,
                       castle_eligibility, 0, depth, ply,
                       mover_pieces_mask, opponent_pieces_mask, next_ply_is_last,
                       king_attack_mask);

      num_moves += allWhitePawnSquaresFind(piece, 0,
                                    en_passant_eligible_pawn, king_position,
                                    0, attack_helper.in_check, attack_helper.pin,
                                    castle_eligibility, depth, ply,
                                    mover_pieces_mask, opponent_pieces_mask, next_ply_is_last);
      num_moves += allKnightSquaresFind(MOVE_WHITE, piece, 0, king_position,
                                        0, attack_helper.in_check, attack_helper.pin, attack_helper.move_test_needed,
                                        castle_eligibility, depth, ply,
                                        mover_pieces_mask, opponent_pieces_mask, next_ply_is_last);
      num_moves += allBishopRookQueenSquaresFind(MOVE_WHITE, piece,
                             0, king_position, 0, attack_helper.in_check, attack_helper.pin, attack_helper.move_test_needed,
                             castle_eligibility, depth, ply,
                             mover_pieces_mask, opponent_pieces_mask, next_ply_is_last);
    }



  return num_moves;
}
/******************************************************************************
** Generate a list of all black pieces move candidates in the position.
**
**    bit_brd - (Input) The bit board of the current position.
**    en_passant_eligible_pawn - (Input) - Location of en passant eligible pawn.
**    castle_eligibility - (Input) - Flags indicating who is eligible to castle.
**
** Return Values:
** Total number of move candidates in this position. Note that this could be 0.
******************************************************************************/
__attribute__((noinline)) 
unsigned long long allBlackMovePerft (
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const castleEligibility_t castle_eligibility,
                           const unsigned int depth,
                           unsigned int ply,
                           const unsigned long long mover_pieces_mask,
                           const unsigned long long opponent_pieces_mask)
{
  unsigned long long num_moves = 0;
  kingAttackHelper_t attack_helper;
  const unsigned int next_ply_is_last = (depth == ++ply);
  const unsigned long long  king_mask = piece[S_KING | S_BLACK];
  const unsigned int   king_position = bitbrdLowestIndexFromMaskGet(king_mask);
  const unsigned long long  king_attack_mask = kingAttack[king_position];
  const unsigned long long any_color_pieces_mask = opponent_pieces_mask | mover_pieces_mask;

    /* Determine if the king is currently in check. When the king is not in check 
    ** we can take a short cut on testing the legality of potential moves.
    ** When the king is not in check and there is a pin then the pin output 
    ** parameter is set to the pin mask.
    */
    attack_helper = pinCompute (
                            MOVE_BLACK, piece, king_position, king_mask,
                            en_passant_eligible_pawn,
                            mover_pieces_mask, any_color_pieces_mask,
                            king_attack_mask, castle_eligibility);

    if (!attack_helper.in_check && !attack_helper.pin)
    {
      if (next_ply_is_last)
      {
        num_moves += blackKingSquaresFind(king_position, attack_helper.move_candidate_mask,
                       piece, 0,
                       castle_eligibility, 0, depth, ply,
                       mover_pieces_mask, opponent_pieces_mask, 1,
                       king_attack_mask);

        num_moves += allBlackPawnSquaresFind(piece, 0,
                                    en_passant_eligible_pawn, king_position,
                                    0, 0, 0,
                                    castle_eligibility, depth, ply,
                                    mover_pieces_mask, opponent_pieces_mask, 1);
        num_moves += allKnightSquaresFind(MOVE_BLACK, piece, 0, king_position,
                                        0, 0, 0, 0,
                                        castle_eligibility, depth, ply,
                                        mover_pieces_mask, opponent_pieces_mask, 1);
        num_moves += allBishopRookQueenSquaresFind(MOVE_BLACK, piece,
                                        0, king_position, 0, 0, 0, 0,
                                        castle_eligibility, depth, ply,
                                        mover_pieces_mask, opponent_pieces_mask, 1);
      } else
      {
        num_moves += blackKingSquaresFind(king_position, attack_helper.move_candidate_mask,
                       piece, 0,
                       castle_eligibility, 0, depth, ply,
                       mover_pieces_mask, opponent_pieces_mask, 0,
                       king_attack_mask);

        num_moves += allBlackPawnSquaresFind(piece, 0,
                                    en_passant_eligible_pawn, king_position,
                                    0, 0, 0,
                                    castle_eligibility, depth, ply,
                                    mover_pieces_mask, opponent_pieces_mask, 0);
        num_moves += allKnightSquaresFind(MOVE_BLACK, piece, 0, king_position,
                                        0, 0, 0, 0,
                                        castle_eligibility, depth, ply,
                                        mover_pieces_mask, opponent_pieces_mask, 0);
        num_moves += allBishopRookQueenSquaresFind(MOVE_BLACK, piece,
                                        0, king_position, 0, 0, 0, 0,
                                        castle_eligibility, depth, ply,
                                        mover_pieces_mask, opponent_pieces_mask, 0);
      }
    } else
    {
      num_moves += blackKingSquaresFind(king_position, attack_helper.move_candidate_mask,
                       piece, 0,
                       castle_eligibility, 0, depth, ply,
                       mover_pieces_mask, opponent_pieces_mask, next_ply_is_last,
                       king_attack_mask);

      num_moves += allBlackPawnSquaresFind(piece, 0,
                                    en_passant_eligible_pawn, king_position,
                                    0, attack_helper.in_check, attack_helper.pin,
                                    castle_eligibility, depth, ply,
                                    mover_pieces_mask, opponent_pieces_mask, next_ply_is_last);
      num_moves += allKnightSquaresFind(MOVE_BLACK, piece, 0, king_position,
                                        0, attack_helper.in_check, attack_helper.pin, attack_helper.move_test_needed,
                                        castle_eligibility, depth, ply,
                                        mover_pieces_mask, opponent_pieces_mask, next_ply_is_last);
      num_moves += allBishopRookQueenSquaresFind(MOVE_BLACK, piece,
                           0, king_position, 0, attack_helper.in_check, attack_helper.pin, attack_helper.move_test_needed,
                           castle_eligibility, depth, ply,
                           mover_pieces_mask, opponent_pieces_mask, next_ply_is_last);
    }

  return num_moves;
}


/******************************************************************************
** Handle last ply moves when there are checks or pins in the position.
**
******************************************************************************/
__attribute__((always_inline)) inline
static unsigned long long allWhiteLastPlyInLine(
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const unsigned int king_position,
                           const kingAttackHelper_t *const attack_helper,
                           const unsigned long long mover_pieces_mask,
                           const unsigned long long opponent_pieces_mask)
{
  unsigned long long num_moves;


  if (!attack_helper->in_check && !attack_helper->move_test_needed)
  {
    if (0 == en_passant_eligible_pawn)
    {
      num_moves = allWhitePawnSquaresLastPlyFind(piece,
                                    0, king_position,
                                    0, attack_helper->pin, 0,
                                    mover_pieces_mask, opponent_pieces_mask); 
    } else
    {
      num_moves = allWhitePawnSquaresLastPlyFind(piece,
                                    en_passant_eligible_pawn, king_position,
                                    0, attack_helper->pin, 0,
                                    mover_pieces_mask, opponent_pieces_mask); 
    }

    num_moves += allBRQNSquaresLastPlyFindPinned (
                                   MOVE_WHITE,
                                   attack_helper->pin,
                                   piece,
                                   mover_pieces_mask,
                                   mover_pieces_mask | opponent_pieces_mask
                                   );
  } else
  {
    num_moves = allWhitePawnSquaresLastPlyFind(piece,
                                    en_passant_eligible_pawn, king_position,
                                    attack_helper->in_check, attack_helper->pin, attack_helper->move_test_needed,
                                    mover_pieces_mask, opponent_pieces_mask);

    num_moves += allKnightSquaresLastPlyFind(MOVE_WHITE, piece, king_position,
                                        attack_helper->in_check, attack_helper->pin, 
                                        attack_helper->move_test_needed,
                                        mover_pieces_mask, opponent_pieces_mask
                                        );
    num_moves += allBishopRookQueenSquaresLastPlyFind(MOVE_WHITE, piece,
                                        king_position, attack_helper->in_check, attack_helper->pin, 
                                        attack_helper->move_test_needed,
                                        mover_pieces_mask, opponent_pieces_mask
                                        );
  }

  return num_moves;
}

/******************************************************************************
******************************************************************************/
__attribute__((noinline)) 
//__attribute__((always_inline)) inline
static unsigned long long allWhiteLastPly(
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const unsigned int king_position,
                           const kingAttackHelper_t *const attack_helper,
                           const unsigned long long mover_pieces_mask,
                           const unsigned long long opponent_pieces_mask)
{
  return allWhiteLastPlyInLine (piece, en_passant_eligible_pawn, 
                                king_position, attack_helper,
                                mover_pieces_mask, opponent_pieces_mask
                                );
}
                            
/******************************************************************************
** Handle last ply moves when there are checks or pins in the position.
**
******************************************************************************/
__attribute__((always_inline)) inline
static unsigned long long allBlackLastPlyInLine(
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const unsigned int king_position,
                           const kingAttackHelper_t *const attack_helper,
                           const unsigned long long mover_pieces_mask,
                           const unsigned long long opponent_pieces_mask)
{
  unsigned long long num_moves;


  if (!attack_helper->in_check && !attack_helper->move_test_needed)
  {
    if (0 == en_passant_eligible_pawn)
    {
      num_moves = allBlackPawnSquaresLastPlyFind(piece,
                                    0, king_position,
                                    0, attack_helper->pin, 0,
                                    mover_pieces_mask, opponent_pieces_mask); 
    } else
    {
      num_moves = allBlackPawnSquaresLastPlyFind(piece,
                                    en_passant_eligible_pawn, king_position,
                                    0, attack_helper->pin, 0,
                                    mover_pieces_mask, opponent_pieces_mask); 
    }

    num_moves += allBRQNSquaresLastPlyFindPinned (
                                   MOVE_BLACK,
                                   attack_helper->pin,
                                   piece,
                                   mover_pieces_mask,
                                   mover_pieces_mask | opponent_pieces_mask
                                   );
  } else
  {
    num_moves = allBlackPawnSquaresLastPlyFind(piece,
                                    en_passant_eligible_pawn, king_position,
                                    attack_helper->in_check, attack_helper->pin, attack_helper->move_test_needed,
                                    mover_pieces_mask, opponent_pieces_mask); 

    num_moves += allKnightSquaresLastPlyFind(MOVE_BLACK, piece, king_position,
                                        attack_helper->in_check, attack_helper->pin,
                                        attack_helper->move_test_needed,
                                        mover_pieces_mask, opponent_pieces_mask
                                        );
    num_moves += allBishopRookQueenSquaresLastPlyFind(MOVE_BLACK, piece,
                                        king_position, attack_helper->in_check, attack_helper->pin,
                                        attack_helper->move_test_needed,
                                        mover_pieces_mask, opponent_pieces_mask
                                        );
  }

  return num_moves;
}

__attribute__((noinline))
//__attribute__((always_inline)) inline
static unsigned long long allBlackLastPly(
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const unsigned int king_position,
                           const kingAttackHelper_t *const attack_helper,
                           const unsigned long long mover_pieces_mask,
                           const unsigned long long opponent_pieces_mask)
{
  return allBlackLastPlyInLine (piece, en_passant_eligible_pawn,
                                king_position, attack_helper,
                                mover_pieces_mask, opponent_pieces_mask
                                );
}



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
__attribute__((always_inline)) inline
static unsigned long long allMoveCandidatesLastPlyFind (
                           const color_e whose_move,
                           unsigned long long *restrict piece,
                           const unsigned int en_passant_eligible_pawn,
                           const castleEligibility_t castle_eligibility,
                           const unsigned long long opponent_pieces_mask,
                           const unsigned long long mover_pieces_mask)
{     
  unsigned long long num_moves = 0;
  const unsigned long long king_mask = piece [S_KING | (whose_move << 3)];
  const unsigned int king_position = bitbrdLowestIndexFromMaskGet(king_mask);
  const unsigned long long  king_attack_mask = kingAttack[king_position];
  const unsigned long long any_color_pieces_mask = opponent_pieces_mask | mover_pieces_mask;

  /* Determine if the king is currently in check. When the king is not in check 
  ** we can take a short cut on testing the legality of potential moves.
  ** When the king is not in check and there is a pin then the pin output 
  ** parameter is set to the pin mask.
  */                              
  const kingAttackHelper_t attack_helper = pinCompute (
                            whose_move, piece, king_position, king_mask,
                            en_passant_eligible_pawn,
                            mover_pieces_mask, any_color_pieces_mask,
                            king_attack_mask, castle_eligibility);

  num_moves += kingSquaresLastPlyFind (
                            attack_helper.move_candidate_mask);

  if (whose_move == MOVE_WHITE)
  {
    if (!attack_helper.in_check && !attack_helper.pin)
    {
      num_moves += allBRQNPSquaresLastPlyFindNoTest (MOVE_WHITE, piece, en_passant_eligible_pawn,
                                            mover_pieces_mask,opponent_pieces_mask, 
                                            any_color_pieces_mask);
    } else
    {
      num_moves += allWhiteLastPly (piece, en_passant_eligible_pawn,
                                    king_position,
                                    &attack_helper, 
                                    mover_pieces_mask, opponent_pieces_mask);
    }
  } else 
  {
    if (!attack_helper.in_check && !attack_helper.pin)
    {
      num_moves +=  allBRQNPSquaresLastPlyFindNoTest (MOVE_BLACK, piece, en_passant_eligible_pawn,
                                            mover_pieces_mask,opponent_pieces_mask, 
                                            any_color_pieces_mask);
    } else
    {
      num_moves += allBlackLastPly (piece, en_passant_eligible_pawn,
                                    king_position,
                                    &attack_helper, 
                                    mover_pieces_mask, opponent_pieces_mask);
    }                                   
  } 

  return num_moves;
}

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
                           const castleEligibility_t castle_eligibility)
{
  return allMoveCandidatesLastPlyFind (whose_move, bit_brd->piece, 
                            en_passant_eligible_pawn, castle_eligibility,
                            bit_brd->color[whose_move ^ 1],
                            bit_brd->color[whose_move]);

}

/******************************************************************************
**
** Return Values:
******************************************************************************/
__attribute__((noinline))
static void udlrSquaresMapCreate (void)
{
  for (unsigned int i = 0; i < BRDS*BRDS; i++)
  {
    udlrVisibilityMap[i] = malloc (MAX_HASH_INDEX * sizeof(unsigned long long));
    assert (udlrVisibilityMap[i]);
  }
  for (unsigned int index = 0; index < BRDS*BRDS; index++)
  {
    unsigned long long attack_mask = udlrAttack[index];
    const unsigned int num_bits = (unsigned int) __builtin_popcountll (attack_mask);
    unsigned int bit_index[num_bits];

    for (unsigned int i = 0; i < num_bits; i++)
    {
      bit_index[i] = bitbrdLowestIndexFromMaskGet(attack_mask);
      attack_mask ^= (1LLU << bit_index[i]);
    }

    /* Derive every possible permutation of bits in the attack mask.
    */
    for (unsigned int mask = 0; mask < (1U << num_bits); mask++)
    {
      unsigned long long visibility_mask = 0;
      unsigned int temp_mask = mask;
      for (unsigned int i = 0; i < num_bits; i++)
      {
        visibility_mask |= ((unsigned long long) (temp_mask & 1)) << bit_index[i];
        temp_mask >>= 1;
      }
      /* With PEXT instruction there are no hash collisions.
      */
      const unsigned long long hash_index =  lookupKeyCompute (visibility_mask, udlrAttack[index]);

      udlrVisibilityMap[index][hash_index] = 0;

      /* Compute the open squares and visible pieces masks for this visibility mask.
      */
      unsigned int row;
      unsigned int column;
      unsigned long long square_mask;


      /* Up
      */
      row = index >> 3;
      column = index & 7;
      while (row < (BRDS-1))
      {
        row++;
        square_mask = 1LLU << ((row << 3) | column);
        if (0 == (visibility_mask & square_mask))
        {
          udlrVisibilityMap[index][hash_index] |= square_mask;
        } else
        {
          udlrVisibilityMap[index][hash_index] |= square_mask;
          break;
        }
      }

      /* Right
      */
      row = index >> 3;
      column = index & 7;
      while (column < (BRDS-1))
      {
        column++;
        square_mask = 1LLU << ((row << 3) | column);
        if (0 == (visibility_mask & square_mask))
        {
          udlrVisibilityMap[index][hash_index] |= square_mask;
        } else
        {
          udlrVisibilityMap[index][hash_index] |= square_mask;
          break;
        }
      }


      /* Down
      */
      row = index >> 3;
      column = index & 7;
      while (row > 0)
      {
        row--;
        square_mask = 1LLU << ((row << 3) | column);
        if (0 == (visibility_mask & square_mask))
        {
          udlrVisibilityMap[index][hash_index] |= square_mask;
        } else
        {
          udlrVisibilityMap[index][hash_index] |= square_mask;
          break;
        }
      }

      /* Left
      */
      row = index >> 3;
      column = index & 7;
      while (column > 0)
      {
        column--;
        square_mask = 1LLU << ((row << 3) | column);
        if (0 == (visibility_mask & square_mask))
        {
          udlrVisibilityMap[index][hash_index] |= square_mask;
        } else
        {
          udlrVisibilityMap[index][hash_index] |= square_mask;
          break;
        }
      }
    }

  }
}

/******************************************************************************
**
** Return Values:
******************************************************************************/
__attribute__((noinline))
static void diagonalSquaresMapCreate (void)
{
  for (unsigned int i = 0; i < BRDS*BRDS; i++)
  {
    diagonalVisibilityMap[i] = malloc (MAX_HASH_INDEX * sizeof(unsigned long long));
    assert (diagonalVisibilityMap[i]);
  }

  for (unsigned int index = 0; index < BRDS*BRDS; index++)
  {
    unsigned long long attack_mask = diagonalAttack[index];
    const unsigned int num_bits = (unsigned int) __builtin_popcountll (attack_mask);
    unsigned int bit_index[num_bits];

    for (unsigned int i = 0; i < num_bits; i++)
    {
      bit_index[i] = bitbrdLowestIndexFromMaskGet(attack_mask);
      attack_mask ^= (1LLU << bit_index[i]);
    }

    /* Derive every possible permutation of bits in the attack mask.
    */
    for (unsigned int mask = 0; mask < (1U << num_bits); mask++)
    {
      unsigned long long visibility_mask = 0;
      unsigned int temp_mask = mask;
      for (unsigned int i = 0; i < num_bits; i++)
      {
        visibility_mask |= ((unsigned long long) (temp_mask & 1)) << bit_index[i];
        temp_mask >>= 1;
      }
      /* With PEXT instruction there are no hash collisions.
      */
      const unsigned long long hash_index =  lookupKeyCompute (visibility_mask, diagonalAttack[index]);

      diagonalVisibilityMap[index][hash_index] = 0;

      /* Compute the open squares and visible pieces masks for this visibility mask.
      */
      unsigned int row;
      unsigned int column;
      unsigned long long square_mask;


      /* Upper Left
      */
      row = index >> 3;
      column = index & 7;
      while ((row < (BRDS-1)) && (column > 0))
      {
        row++;
        column--;
        square_mask = 1LLU << ((row << 3) | column);
        if (0 == (visibility_mask & square_mask))
        {
          diagonalVisibilityMap[index][hash_index] |= square_mask;
        } else
        {
          diagonalVisibilityMap[index][hash_index] |= square_mask;
          break;
        }
      }

      /* Upper Right
      */
      row = index >> 3;
      column = index & 7;
      while ((row < (BRDS-1)) && (column < (BRDS-1)))
      {
        row++;
        column++;
        square_mask = 1LLU << ((row << 3) | column);
        if (0 == (visibility_mask & square_mask))
        {
          diagonalVisibilityMap[index][hash_index] |= square_mask;
        } else
        {
          diagonalVisibilityMap[index][hash_index] |= square_mask;
          break;
        }
      }


      /* Lower Right
      */
      row = index >> 3;
      column = index & 7;
      while ((row > 0) && (column < (BRDS-1)))
      {
        row--;
        column++;
        square_mask = 1LLU << ((row << 3) | column);
        if (0 == (visibility_mask & square_mask))
        {
          diagonalVisibilityMap[index][hash_index] |= square_mask;
        } else
        {
          diagonalVisibilityMap[index][hash_index] |= square_mask;
          break;
        }
      }

      /* Lower Left
      */
      row = index >> 3;
      column = index & 7;
      while ((row > 0) && (column > 0))
      {
        row--;
        column--;
        square_mask = 1LLU << ((row << 3) | column);
        if (0 == (visibility_mask & square_mask))
        {
          diagonalVisibilityMap[index][hash_index] |= square_mask;
        } else
        {
          diagonalVisibilityMap[index][hash_index] |= square_mask;
          break;
        }
      }
    }

  }
}

/******************************************************************************
**
** Return Values:
******************************************************************************/
__attribute__((noinline))
static void dangerMapCreate (void)
{
  for (unsigned int i = 0; i < BRDS*BRDS; i++)
  {
    unsigned long long king_attack_mask = kingAttack[i];

    /* Add attack vectors on the squares to which the king can move.
    */
    while (king_attack_mask)
    {
      const unsigned int index = bitbrdLowestIndexFromMaskGet(king_attack_mask);
      king_attack_mask ^= 1LLU << index;

      dangerMap[i].knight_danger_mask |= knightAttack[index];

      dangerMap[i].diagonal_danger_mask |= diagonalAttack[index];
      dangerMap[i].udlr_danger_mask |= udlrAttack[index];
    }

    /* Add attack vectors on the king.
    */
    dangerMap[i].knight_danger_mask |= knightAttack[i];
    dangerMap[i].diagonal_danger_mask |= diagonalAttack[i];
    dangerMap[i].udlr_danger_mask |= udlrAttack[i];
  }
}


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
void squareUnderAttackGenerate(void)
{
  unsigned int row, column;
  unsigned long long mask;
  unsigned int index;


  /* Generate bit mask for attack from knight.
  */
  for (row = 0; row < BRDS; row++)
  {
    for (column = 0; column < BRDS; column++)
    {
      index = bitbrdIndexFromPositionGet (row, column);
      if ((row >= 2) && (column >= 1))
      {
        mask = bitbrdMaskFromPositionGet (row - 2, column - 1);
        knightAttack[index] |= mask;
      }


      if ((row >= 1) && (column >= 2))
      {
        mask = bitbrdMaskFromPositionGet (row - 1, column - 2);
        knightAttack[index] |= mask;
      }

      if ((row <= 6) && (column >= 2))
      {
        mask = bitbrdMaskFromPositionGet (row + 1, column - 2);
        knightAttack[index] |= mask;
      }

      if ((row <= 5) && (column >= 1))
      {
        mask = bitbrdMaskFromPositionGet (row + 2, column - 1);
        knightAttack[index] |= mask;
      }

      if ((row <= 5) && (column <= 6))
      {
        mask = bitbrdMaskFromPositionGet (row + 2, column + 1);
        knightAttack[index] |= mask;
      }

      if ((row <= 6) && (column <= 5))
      {
        mask = bitbrdMaskFromPositionGet (row + 1, column + 2);
        knightAttack[index] |= mask;
      }

      if ((row >= 1) && (column <= 5))
      {
        mask = bitbrdMaskFromPositionGet (row - 1, column + 2);
        knightAttack[index] |= mask;
      }

      if ((row >= 2) && (column <= 6))
      {
        mask = bitbrdMaskFromPositionGet (row - 2, column + 1);
        knightAttack[index] |= mask;
      }

    }
  }
  /* Generate bit mask for attack from black and white pawns.
  */
  for (row = 0; row < BRDS; row++)
  {
    for (column = 0; column < BRDS; column++)
    {
      index = bitbrdIndexFromPositionGet (row, column);
      if ((row <= 5) && (column >= 1))
      {
        mask = bitbrdMaskFromPositionGet (row + 1, column - 1);
        blackPawnAttack[index] |= mask;
      }
      if ((row <= 5) && (column <= 6))
      {
        mask = bitbrdMaskFromPositionGet (row + 1, column + 1);
        blackPawnAttack[index] |= mask;
      }
      if (row == 3)
      {
        if (column >= 1)
        {
          mask = bitbrdMaskFromPositionGet (row, column - 1);
          blackPawnEnPassantAttack[index] |= mask;
        }
        if (column <= 6)
        {
          mask = bitbrdMaskFromPositionGet (row, column + 1);
          blackPawnEnPassantAttack[index] |= mask;
        }
      }


      if ((row >= 2) && (column >= 1))
      {
        mask = bitbrdMaskFromPositionGet (row - 1, column - 1);
        whitePawnAttack[index] |= mask;
      }
      if ((row >= 2) && (column <= 6))
      {
        mask = bitbrdMaskFromPositionGet (row - 1, column + 1);
        whitePawnAttack[index] |= mask;
      }
      if (row == 4)
      {
        if (column >= 1)
        {
          mask = bitbrdMaskFromPositionGet (row, column - 1);
          whitePawnEnPassantAttack[index] |= mask;
        }
        if (column <= 6)
        {
          mask = bitbrdMaskFromPositionGet (row, column + 1);
          whitePawnEnPassantAttack[index] |= mask;
        }
      }
    }
  }

  /* Generate bit mask for capture from black and white pawns.
  ** All other pieces have identical attack/capture masks, but pawns don't.
  */
  for (row = 0; row < BRDS; row++)
  {
    for (column = 0; column < BRDS; column++)
    {
      index = bitbrdIndexFromPositionGet (row, column);
      if ((row > 0) && (column > 0))
      {
        mask = bitbrdMaskFromPositionGet (row - 1, column - 1);
        blackPawnCapture[index] |= mask;
      }
      if ((row > 0) && (column < 7))
      {
        mask = bitbrdMaskFromPositionGet (row - 1, column + 1);
        blackPawnCapture[index] |= mask;
      }
      /* En Passant Capture.
      */
      if ((row == 3) && (column > 0))
      {
        mask = bitbrdMaskFromPositionGet (row, column - 1);
        blackPawnCapture[index] |= mask;
      }
      if ((row == 3) && (column < 7))
      {
        mask = bitbrdMaskFromPositionGet (row, column + 1);
        blackPawnCapture[index] |= mask;
      }

      if ((row < (BRDS-1)) && (column > 0))
      {
        mask = bitbrdMaskFromPositionGet (row + 1, column - 1);
        whitePawnCapture[index] |= mask;
      }
      if ((row < (BRDS-1)) && (column < 7))
      {
        mask = bitbrdMaskFromPositionGet (row + 1, column + 1);
        whitePawnCapture[index] |= mask;
      }

      /* En Passant Capture.
      */
      if ((row == 4) && (column > 0))
      {
        mask = bitbrdMaskFromPositionGet (row, column - 1);
        whitePawnCapture[index] |= mask;
      }
      if ((row == 4) && (column < 7))
      {
        mask = bitbrdMaskFromPositionGet (row, column + 1);
        whitePawnCapture[index] |= mask;
      }
    }
  }




  /* Generate bit mask for attack from a King.
  */
  for (row = 0; row < BRDS; row++)
  {
    for (column = 0; column < BRDS; column++)
    {
      index = bitbrdIndexFromPositionGet (row, column);
      if ((row <= 6) && (column >= 1)) /* Upper Left Diagonal */
      {
        mask = bitbrdMaskFromPositionGet (row + 1, column - 1);
        kingAttack[index] |= mask;
      }
      if ((row <= 6) && (column <= 6)) /* Upper Right Diagonal */
      {
        mask = bitbrdMaskFromPositionGet (row + 1, column + 1);
        kingAttack[index] |= mask;
      }
      if ((row >= 1) && (column <= 6)) /* Lower Right Diagonal */
      {
        mask = bitbrdMaskFromPositionGet (row - 1, column + 1);
        kingAttack[index] |= mask;
      }
      if ((row >= 1) && (column >= 1)) /* Lower Left Diagonal */
      {
        mask = bitbrdMaskFromPositionGet (row - 1, column - 1);
        kingAttack[index] |= mask;
      }

      if (column >= 1) /* Left Horizontal */
      {
        mask = bitbrdMaskFromPositionGet (row, column - 1);
        kingAttack[index] |= mask;
      }
      if (column <= 6) /* Right Horizontal */
      {
        mask = bitbrdMaskFromPositionGet (row, column + 1);
        kingAttack[index] |= mask;
      }
      if (row >= 1) /* Down Vertical */
      {
        mask = bitbrdMaskFromPositionGet (row - 1, column);
        kingAttack[index] |= mask;
      }
      if (row <= 6) /* Up Vertical */
      {
        mask = bitbrdMaskFromPositionGet (row + 1, column);
        kingAttack[index] |= mask;
      }
    }
  }

  /* Find all diagonals for this position.
  */
  for (row = 0; row < BRDS; row++)
  {
    for (column = 0; column < BRDS; column++)
      {
      unsigned int i, j;

      index = bitbrdIndexFromPositionGet (row, column);

      /* Lower Left Diagonal
      */
      i = row;
      j = column;
      do
      {
        if ((i == 0) || (j == 0))
                            break;
        i--;
        j--;
        mask = bitbrdMaskFromPositionGet (i, j);
        diagonalAttack[index] |= mask;


      } while (1);
      /* Upper Left Diagonal
      */
      i = row;
      j = column;
      do
      {
        if ((i == 7) || (j == 0))
                            break;

        i++;
        j--;
        mask = bitbrdMaskFromPositionGet (i, j);
        diagonalAttack[index] |= mask;


      } while (1);

      /* Upper Right Diagonal
      */
      i = row;
      j = column;
      do
      {
        if ((i == 7) || (j == 7))
                            break;

        i++;
        j++;
        mask = bitbrdMaskFromPositionGet (i, j);
        diagonalAttack[index] |= mask;


      } while (1);

      /* Lower Right Diagonal
      */
      i = row;
      j = column;
      do
      {
        if ((i == 0) || (j == 7))
                            break;

        i--;
        j++;
        mask = bitbrdMaskFromPositionGet (i, j);
        diagonalAttack[index] |= mask;


      } while (1);
    }
  }

  /* Find all vertical and horizontal lines for this position.
  */
  for (row = 0; row < BRDS; row++)
  {
    for (column = 0; column < BRDS; column++)
      {
      unsigned int i, j;

      index = bitbrdIndexFromPositionGet (row, column);

      /* Left
      */
      i = row;
      j = column;
      do
      {
        if (j == 0)
                   break;

        j--;
        mask = bitbrdMaskFromPositionGet (i, j);
        udlrAttack[index] |= mask;


      } while (1);

      /* Right
      */
      i = row;
      j = column;
      do
      {
        if (j == 7)
                  break;

        j++;
        mask = bitbrdMaskFromPositionGet (i, j);
        udlrAttack[index] |= mask;


      } while (1);

      /* Up
      */
      i = row;
      j = column;
      do
      {
        if (i == 7)
                  break;

        i++;
        mask = bitbrdMaskFromPositionGet (i, j);
        udlrAttack[index] |= mask;


      } while (1);

      /* Down
      */
      i = row;
      j = column;
      do
      {
        if (i == 0)
                  break;


        i--;
        mask = bitbrdMaskFromPositionGet (i, j);
        udlrAttack[index] |= mask;


      } while (1);
    }
  }

  /* Generate open lanes masks from any one square to any other square.
  ** This mask is used for checking if the king is under attack.
  */
  for (int king_index = 0; king_index < BRDS*BRDS; king_index++)
  {
    const int king_row = king_index >> 3;
    const int king_column = king_index & 7;

    for (index = 0; index < BRDS*BRDS; index++)
    {
      /* If these two squares are not connected via diagonal or vertical or 
      ** horizontal files then skip them.
      */
      if (0 == ((diagonalAttack[king_index] | udlrAttack[king_index]) & (1LLU << index)))
                        continue;

      static const int delta[BRDS][BRDS] =
             {{ 0, 1, 1, 1, 1, 1, 1, 1},
              {-1, 0, 1, 1, 1, 1, 1, 1},
              {-1,-1, 0, 1, 1, 1, 1, 1},
              {-1,-1,-1, 0, 1, 1, 1, 1},
              {-1,-1,-1,-1, 0, 1, 1, 1},
              {-1,-1,-1,-1,-1, 0, 1, 1},
              {-1,-1,-1,-1,-1,-1, 0, 1},
              {-1,-1,-1,-1,-1,-1,-1, 0}};
      const int attacker_row = (int) (index >> 3);
      const int attacker_column = (int) (index & 7);
      const int row_delta = delta[king_row][attacker_row];
      const int column_delta = delta[king_column][attacker_column];
      int i = king_row + row_delta;
      int j = king_column + column_delta;

      while (1)
      {
        if ((i == attacker_row) && (j == attacker_column))
                                                  break;

        attackLanes[king_index][index] |= POSITION_TO_BITMASK_LOOKUP(i,j);

        i += row_delta;
        j += column_delta;
      }
    }
  }

  /* Make a copy of the attack vectors into a combined vector 
  ** structure for more efficient retrieval.
  */
  for (int i = 0; i < BRDS*BRDS; i++)
  {
    aggregateAttack[i].udlrAttack = udlrAttack[i];
    aggregateAttack[i].diagonalAttack = diagonalAttack[i];
    aggregateAttack[i].knightAttack = knightAttack[i];
    aggregateAttack[i].kingAttack = kingAttack[i];
    aggregateAttack[i].whitePawnAttack = whitePawnAttack[i];
    aggregateAttack[i].blackPawnAttack = blackPawnAttack[i];
  }

  (void) atexit (cleanup);
  diagonalSquaresMapCreate ();
  udlrSquaresMapCreate ();
  dangerMapCreate ();
}


/******************************************************************************
** Display the bit board on the console.
** The function converts the bit board to board, and then calls the
** movegenUtilBoardPrint(brd) function.
**
** This function is ony used for debugging.
**
******************************************************************************/
__attribute__((noinline))
void bitbrdPrint (const unsigned long long *const piece)
{
  brd_t brd;
  unsigned char p1;
  unsigned long mask;
  unsigned int index;

  memset (&brd, 0, sizeof(brd_t));
  for (p1 = 0; p1 < 16; p1++)
  {
    mask = piece[p1];
    if (0 == mask)
                continue;
    for (index = 0; index < 64; index++)
    {
      if ((1LLU << index) & mask)
      {
        brd.rc[index >> 3][index & 7] = p1;
      }
    }

  }
  brdutilBoardPrint(&brd);
}
