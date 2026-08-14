/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <locale.h>
#include <pthread.h>

#include "bytebrd_api.h"
#include "movegen.h"

/********************************************************************
** Recursive move generator.
**
** depth - (input) - Depth to which to search.
** brd - (input) 8x8 chess board.
** info - (input) Additional game description.
********************************************************************/
unsigned long long recursiveMove (const unsigned int depth,
           const brd_t *const brd,
           const brdCtrlInfo_t *const info,
           const unsigned int ply)
{
  bytebrdMove_t move_list[MAX_BRD_MOVES];
  unsigned long long total_positions = 0;
  unsigned int num_moves;
  unsigned int mover_lost;

  num_moves = bytebrdNextMoveGet (brd, info, move_list, &mover_lost);
  if (ply == (depth-1))
  {
    return (unsigned long long) num_moves;
  }

  for (unsigned int i = 0; i < num_moves; i++)
  {
    brd_t next_brd = *brd;
    brdCtrlInfo_t next_info;

    bytebrdUtilMoveMake (&next_brd, &move_list[i], info, &next_info);
    total_positions += recursiveMove (depth, &next_brd, &next_info, ply + 1);
  }

  return total_positions;
}

/********************************************************************
** Test the bytebrdNextMoveGet() API by using it to implement
** a perft test. 
** This code runs slower than the dedicated bytebrdPerft(), 
** but should find exactly same number of moves.
**
** depth - (input) - Depth to which to search.
** brd - (input) 8x8 chess board.
** info - (input) Additional game description.
********************************************************************/
unsigned long long nextMoveGetAPITestPerft (const unsigned int depth,
           const brd_t *const brd,
           const brdCtrlInfo_t *const info)
{
  bytebrdMove_t move_list[MAX_BRD_MOVES];
  unsigned int num_moves;
  unsigned int mover_lost;
  unsigned long long positions_per_move;
  unsigned long long total_positions = 0;
  constexpr char piece_name[] = {0,0,'n','b','r','q',0,0};


  /* The input position is assumed to be valid.
  */

  if (depth == 0)
                return 0;

  printf ("Starting bytebrdNextMoveGet() test for depth:%u\n", depth);

  num_moves = bytebrdNextMoveGet (brd, info, move_list, &mover_lost);

  if (num_moves == 0)
  {
    if (mover_lost)
    {
      printf ("No Moves Availabe. Position is in checkmate.\n");
    } else
    {
      printf ("No Moves Availabe. Position is in stalemate.\n");
    }
    return 0;
  }

  for (unsigned int i = 0; i < num_moves; i++)
  {
    brd_t next_brd = *brd;
    brdCtrlInfo_t next_info;

    bytebrdUtilMoveMake (&next_brd, &move_list[i], info, &next_info);
    if (depth > 1)
    {
      positions_per_move = recursiveMove (depth - 1, &next_brd, &next_info, 0);
    } else
    {
      positions_per_move = 1;
    }
    total_positions += positions_per_move;

    printf ("%u - ", i+1);

    printf ("%c%c",
                colName[move_list[i].from_c],
                rowName[move_list[i].from_r]);
    printf ("%c%c",
                colName[move_list[i].to_c],
                rowName[move_list[i].to_r]);
    if (move_list[i].pawn_promotion)
    {
      printf ("%c", piece_name[PIECE_GET(move_list[i].promoted_piece)]);
    }
    printf (": %'llu\n", positions_per_move);

  }

  printf ("Total positions at depth %u is:%'llu\n", depth, total_positions);

  return total_positions;
}


void code_coverage_test(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
         __FUNCTION__, __LINE__);


  brdutilStartPositionCreate(&brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);

  assert (20 == bytebrdPerft (1, &brd, &info, 1));
  assert (400 == bytebrdPerft (2, &brd, &info, 1));
  assert (0 == bytebrdPerft (0, &brd, &info, 1));

#if 1
  /* Double pin tests.
  */
  brdutilFenToBrdConvert ("3k4/3r4/8/8/b5b1/2N1N3/2PnP3/3K4 b - -",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (2'175'864'716 == bytebrdPerft (7, &brd, &info, 0));

  brdutilFenToBrdConvert ("8/2p5/P2p4/KP5r/PR3p1k/8/4P1P1/r7 w - -",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (407'601'368 == bytebrdPerft (7, &brd, &info, 0));

#endif

#if 1
  brdutilFenToBrdConvert ("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);

  bytebrdPerft (1, &brd, &info, 0);
  bytebrdPerft (2, &brd, &info, 0);
  assert (8031647685 == bytebrdPerft (6, &brd, &info, 0)); // Expected: 8,031,647,685
#endif

#if 1
  brdutilFenToBrdConvert ("n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (71179139 == bytebrdPerft (6, &brd, &info, 0)); // Expected: 71,179,139
#endif

#if 1
  brdutilFenToBrdConvert ("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (178633661 == bytebrdPerft (7, &brd, &info, 0)); // Expected: 178,633,661
#endif

#if 1
  brdutilFenToBrdConvert ("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (706045033 == bytebrdPerft (6, &brd, &info, 0)); // Expected: 706,045,033
#endif

#if 1
  brdutilFenToBrdConvert ("r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (706045033 == bytebrdPerft (6, &brd, &info, 0)); // Expected: 706,045,033
#endif

#if 1
  brdutilFenToBrdConvert ("k1qrq3/rq4Qq/3q1Q2/Q6q/3Q4/1Q4Q1/R1q1Q2Q/KQR2q1q w - -",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (26145195702 == bytebrdPerft (5, &brd, &info, 0)); // Expected: 26,145,195,702
#endif

#if 1
  brdutilFenToBrdConvert ("8/3K4/2p5/p2b2r1/5k2/8/8/1q6 b - - 1 67",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (279 == bytebrdPerft (2, &brd, &info, 0)); // Expected: 279
#endif

#if 1
  brdutilFenToBrdConvert ("8/7p/p5pb/4k3/P1pPn3/8/P5PP/1rB2RK1 b - d3 0 28",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (38633283 == bytebrdPerft (6, &brd, &info, 0)); // Expected: 38,633,283
#endif

#if 1
  brdutilFenToBrdConvert ("rnbqkb1r/ppppp1pp/7n/4Pp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (11139762 == bytebrdPerft (5, &brd, &info, 0)); // Expected: 11,139,762
#endif

#if 1
  brdutilFenToBrdConvert ("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (11030083 == bytebrdPerft (6, &brd, &info, 0)); // Expected: 11,030,083
  assert (178633661 == bytebrdPerft (7, &brd, &info, 0)); // Expected: 178,633,661
#endif

#if 1
  brdutilFenToBrdConvert ("4r1k1/p4pp1/2n2n1B/2b5/N6Q/P2q1N2/1r4PP/R4R1K b - - 1 23",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (27'076'678'136 == bytebrdPerft (6, &brd, &info, 0));
#endif

#if 1
  brdutilFenToBrdConvert ("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (89'941'194 == bytebrdPerft (5, &brd, &info, 0));
#endif

#if 1
  brdutilFenToBrdConvert ("rnbq1k1r/1p1Pbpp1/2p5/8/2B5/p6p/PPP1NnPP/RNBQK2R w KQ - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (102'062'541 == bytebrdPerft (5, &brd, &info, 0));
#endif

#if 1
  brdutilFenToBrdConvert ("rnbqk2r/pp2bppp/P1p4P/8/2B5/8/1PP1NnP1/RNBQK2R w KQkq - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (141'696'521 == bytebrdPerft (5, &brd, &info, 0));
#endif

#if 1
  brdutilFenToBrdConvert ("rnbq21r/pp1Pbppp/2p5/8/2B5/8/PkP1NnPP/RN1QK2R w KQ - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (79'930'420 == bytebrdPerft (5, &brd, &info, 0));
#endif

#if 1
  brdutilFenToBrdConvert ("rnbq21r/pp1Pbppp/2p5/8/2B5/8/PPP1NnkP/RN1QK2R w KQ - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (62'279'306 == bytebrdPerft (5, &brd, &info, 0));
#endif

#if 1
  brdutilFenToBrdConvert ("rn1qk2r/pK2bppp/2p5/8/2B5/8/PPP1NnPP/RNBQ3R w kq - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (88'326'080 == bytebrdPerft (5, &brd, &info, 0));
#endif

#if 1
  brdutilFenToBrdConvert ("rn1qk2r/pp2bpKp/2p5/8/2B5/8/PPP1NnPP/RNBQ3R w kq - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (81'774'769 == bytebrdPerft (5, &brd, &info, 0));
#endif

#if 1
  brdutilFenToBrdConvert ("rn1qk2r/pp2bppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R b kq - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (82'603'971 == bytebrdPerft (5, &brd, &info, 0));
#endif

#if 1
  brdutilFenToBrdConvert ("r3k2r/pp1qbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R b kq - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (117'033'229 == bytebrdPerft (5, &brd, &info, 0));
#endif

#if 1
  brdutilFenToBrdConvert ("r3k2r/pP1qbpPp/2p5/8/2B5/8/P1P1Nn1P/RNBQK2R w kq - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (139'590'926 == bytebrdPerft (5, &brd, &info, 0));
#endif


#if 1
  brdutilFenToBrdConvert ("rn1qk2r/pp2bpKp/2p5/8/2B5/8/PPP1NnPP/RNBQ3R w kq - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (81'774'769 == nextMoveGetAPITestPerft (5, &brd, &info));
#endif

#if 1
  brdutilFenToBrdConvert ("rn1qk2r/pp2bppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R b kq - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (82'603'971 == nextMoveGetAPITestPerft (5, &brd, &info));
#endif

#if 1
  brdutilFenToBrdConvert ("r3k2r/pp1qbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R b kq - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (117'033'229 == nextMoveGetAPITestPerft (5, &brd, &info));
#endif

#if 1
  brdutilFenToBrdConvert ("r3k2r/pP1qbpPp/2p5/8/2B5/8/P1P1Nn1P/RNBQK2R w kq - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (139'590'926 == nextMoveGetAPITestPerft (5, &brd, &info));
#endif

}

void bytebrd_test(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

#if 1
  brdutilStartPositionCreate(&brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (119'060'324 == nextMoveGetAPITestPerft (6, &brd, &info));
  assert (0 == bytebrdUtilOpponentInCheck(&brd, &info));

  info.next_move = MOVE_BLACK;
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (0 == bytebrdUtilOpponentInCheck(&brd, &info));
#endif
}

/* Achieve 100% code coverage in move selection code 
*/
void run_tests(void)
{
  /* Test the bitbrdPrint() function.
  ** This function is norally used only for debugging in movegen.c, so we need to 
  ** set up the bit board in order to test the function.
  */
  {
    /* Set up the bit board.
    */
    brd_t brd;
    brdCtrlInfo_t info;
    bitBrd_t bit_brd;

    brdutilStartPositionCreate(&brd, &info);
    bitbrdClear (&bit_brd);
    for (unsigned int row = 0; row < BRDS; row++)
    {
      for (unsigned int column = 0; column < BRDS; column++)
      {
        if (S_EMPTY != brd.rc[row][column])
           bitbrdPieceSet(&bit_brd, brd.rc[row][column], row, column);
      }
    }

    bitbrdPrint (&bit_brd);
  }

  /* Perft tests.
  */
  code_coverage_test (); 

  /* Bytebrd Tests.
  */
  bytebrd_test ();

}

void codecovInit (void)
{
  /* Enable printf() to format integers using comma separators. For example
  ** printf ("%'u", val) prints 100,000,000 instead of 100000000.
  ** Note the ' between % and u. This triggers printf() to use
  ** the country code formatting conventions.
  */
  setenv ("LC_ALL","en_US.UTF-8",1);
  setlocale (LC_NUMERIC, "");

  bytebrdInit ();
}

int main ()
{
  printf ("Starting Code Coverage Test...\n");
  codecovInit ();
  run_tests();
  printf ("Finished  Code Coverage Test.\n");
}
