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
#ifdef __linux__
#include <sys/mman.h>
#endif

#include "bytebrd_api.h"


/********************************************************************
** Create a board with the test starting position.
**
** brd - (output) 8x8 chess board.
** info - (output) Additional game description.
********************************************************************/
void middlePos1Create (brd_t *brd, brdCtrlInfo_t *info)
{
  memset (brd, 0, sizeof(brd_t));
  memset (info, 0, sizeof (brdCtrlInfo_t));

  info->next_move = MOVE_WHITE;
  info->white_long_castle_eligible = 0;
  info->white_short_castle_eligible = 0;
  info->black_long_castle_eligible = 0;
  info->black_short_castle_eligible = 0;

  /* Starting position of white pieces.
  */
  brd->rc[0][6] = S_KING | S_WHITE;

  brd->rc[2][3] = S_QUEEN | S_WHITE;

  brd->rc[0][3] = S_ROOK | S_WHITE;
  brd->rc[0][4] = S_ROOK | S_WHITE;

  brd->rc[2][2] = S_KNIGHT | S_WHITE;
  brd->rc[2][5] = S_KNIGHT | S_WHITE;

  brd->rc[3][5] = S_BISHOP | S_WHITE;

  brd->rc[2][0] = S_PAWN | S_WHITE;
  brd->rc[1][1] = S_PAWN | S_WHITE;
  brd->rc[3][1] = S_PAWN | S_WHITE;
  brd->rc[1][5] = S_PAWN | S_WHITE;
  brd->rc[1][6] = S_PAWN | S_WHITE;
  brd->rc[2][7] = S_PAWN | S_WHITE;


    /* Starting position of black pieces.
  */
  brd->rc[7][6] = S_KING | S_BLACK;

  brd->rc[7][3] = S_QUEEN | S_BLACK;

  brd->rc[7][0] = S_ROOK | S_BLACK;
  brd->rc[7][5] = S_ROOK | S_BLACK;

  brd->rc[5][2] = S_KNIGHT | S_BLACK;
  brd->rc[5][5] = S_KNIGHT | S_BLACK;

  brd->rc[6][3] = S_BISHOP | S_BLACK;

  brd->rc[5][0] = S_PAWN | S_BLACK;
  brd->rc[6][1] = S_PAWN | S_BLACK;
  brd->rc[4][3] = S_PAWN | S_BLACK;
  brd->rc[5][4] = S_PAWN | S_BLACK;
  brd->rc[6][5] = S_PAWN | S_BLACK;
  brd->rc[6][6] = S_PAWN | S_BLACK;
  brd->rc[6][7] = S_PAWN | S_BLACK;
}

/********************************************************************
** Create a board with the test starting position.
**
** brd - (output) 8x8 chess board.
** info - (output) Additional game description.
********************************************************************/
void middlePos2Create (brd_t *brd, brdCtrlInfo_t *info)
{
  memset (brd, 0, sizeof(brd_t));
  memset (info, 0, sizeof (brdCtrlInfo_t));

  info->next_move = MOVE_WHITE;
  info->white_long_castle_eligible = 1;
  info->white_short_castle_eligible = 1;
  info->black_long_castle_eligible = 1;
  info->black_short_castle_eligible = 1;

  /* Starting position of white pieces.
  */
  brd->rc[0][4] = S_KING | S_WHITE;

  brd->rc[0][3] = S_QUEEN | S_WHITE;

  brd->rc[0][0] = S_ROOK | S_WHITE;
  brd->rc[0][7] = S_ROOK | S_WHITE;

  brd->rc[2][2] = S_KNIGHT | S_WHITE;
  brd->rc[2][5] = S_KNIGHT | S_WHITE;

  brd->rc[0][2] = S_BISHOP | S_WHITE;
  brd->rc[4][1] = S_BISHOP | S_WHITE;

  brd->rc[2][0] = S_PAWN | S_WHITE;
  brd->rc[1][1] = S_PAWN | S_WHITE;
  brd->rc[1][2] = S_PAWN | S_WHITE;
  brd->rc[1][3] = S_PAWN | S_WHITE;
  brd->rc[3][4] = S_PAWN | S_WHITE;
  brd->rc[1][5] = S_PAWN | S_WHITE;
  brd->rc[1][6] = S_PAWN | S_WHITE;
  brd->rc[1][7] = S_PAWN | S_WHITE;


    /* Starting position of black pieces.
  */
  brd->rc[7][4] = S_KING | S_BLACK;

  brd->rc[7][3] = S_QUEEN | S_BLACK;

  brd->rc[7][0] = S_ROOK | S_BLACK;
  brd->rc[7][7] = S_ROOK | S_BLACK;

  brd->rc[5][2] = S_KNIGHT | S_BLACK;
  brd->rc[5][5] = S_KNIGHT | S_BLACK;

  brd->rc[7][2] = S_BISHOP | S_BLACK;
  brd->rc[4][2] = S_BISHOP | S_BLACK;

  brd->rc[6][0] = S_PAWN | S_BLACK;
  brd->rc[6][1] = S_PAWN | S_BLACK;
  brd->rc[6][2] = S_PAWN | S_BLACK;
  brd->rc[6][3] = S_PAWN | S_BLACK;
  brd->rc[4][4] = S_PAWN | S_BLACK;
  brd->rc[6][5] = S_PAWN | S_BLACK;
  brd->rc[6][6] = S_PAWN | S_BLACK;
  brd->rc[6][7] = S_PAWN | S_BLACK;
}


/**************************
** End Game Position Tests.
**************************/


/* King vs King
*/
void test_e_4(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
                 __FUNCTION__, __LINE__);

  brdutilFenToBrdConvert ("6k1/8/8/8/8/8/8/6K1 w - -",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);

//  assert (31'152'946 == bytebrdPerft (7, &brd, &info, 0));
//  assert (393'333'897 == bytebrdPerft (8, &brd, &info, 0));
//  assert (4'356'693'377 == bytebrdPerft (9, &brd, &info, 0));
//  assert (54'616'238'950 == bytebrdPerft (10, &brd, &info, 0));

//    assert (398'754'564 == bytebrdPerft (11, &brd, &info, 0));
    assert (2'644'397'209 == bytebrdPerft (12, &brd, &info, 0));
//    assert (17'748'285'581 == bytebrdPerft (13, &brd, &info, 0));
//   bytebrdPerft (14, &brd, &info, 0);

}


/* King/Pawn End Game.
*/
void test_e_3(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
                 __FUNCTION__, __LINE__);

  brdutilFenToBrdConvert ("6k1/1p3ppp/p3p3/3p4/1P6/P6P/1P3PP1/6K1 w - -",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
//  assert (31'152'946 == bytebrdPerft (7, &brd, &info, 0)); 
//  assert (393'333'897 == bytebrdPerft (8, &brd, &info, 0)); 
  assert (4'356'693'377 == bytebrdPerft (9, &brd, &info, 0)); 
//  assert (54'616'238'950 == bytebrdPerft (10, &brd, &info, 0)); 

}

void test_e_2(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
		 __FUNCTION__, __LINE__); 

  brdutilFenToBrdConvert ("6k1/8/8/8/3N4/3B4/8/6K1 b - -",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);

//  assert (119'225'679 == bytebrdPerft (8, &brd, &info, 0));
//  assert (645'210'465 == bytebrdPerft (9, &brd, &info, 0));
  assert (13'017'442'865 == bytebrdPerft (10, &brd, &info, 0));
//  assert (71'415'699'101 == bytebrdPerft (11, &brd, &info, 0));
//  bytebrdPerft (12, &brd, &info, 0);
}

void test_e_1(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
         __FUNCTION__, __LINE__);

  brdutilFenToBrdConvert ("r5kb/8/8/8/8/3Q4/8/6K1 b - -",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);

  assert (2'504'283'977 == bytebrdPerft (7, &brd, &info, 0));
// assert (59'900'786'279 ==  bytebrdPerft (8, &brd, &info, 0));
// bytebrdPerft (10, &brd, &info, 0);
}


/**************************
** Middle Game Position Tests.
**************************/
void test_m_1(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
		 __FUNCTION__, __LINE__); 

  middlePos1Create(&brd, &info);
  brdutilBoardPrint (&brd);
  brdutilFenPrint (&brd, &info);

  bytebrdPerft (6, &brd, &info, 0);
//  bytebrdPerft (7, &brd, &info, 0);
}

void test_m_2(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
                 __FUNCTION__, __LINE__);

  middlePos2Create(&brd, &info);
  brdutilBoardPrint (&brd);
  brdutilFenPrint (&brd, &info);

  // bytebrdPerft (6, &brd, &info, 0);
  bytebrdPerft (7, &brd, &info, 0);

}

void test_m_3(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
                 __FUNCTION__, __LINE__);

  brdutilFenToBrdConvert ("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);

  assert (8'031'647'685 == bytebrdPerft (6, &brd, &info, 0));
//  assert (374'190'009'323 == bytebrdPerft (7, &brd, &info, 0));
//  bytebrdPerft (8, &brd, &info, 0);
//  bytebrdPerft (10, &brd, &info, 0);

}

void test_m_4(void)
{ 
  brd_t brd;
  brdCtrlInfo_t info;
  
  printf ("%s - %d - Starting...\n",
                 __FUNCTION__, __LINE__);
  
  brdutilFenToBrdConvert ("r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  
  assert (6'923'051'137 == bytebrdPerft (6, &brd, &info, 0));
  
} 


/**************************
** Starting Position Tests.
**************************/
void test_1(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
		 __FUNCTION__, __LINE__); 

  brdutilStartPositionCreate(&brd, &info);
// info.next_move = MOVE_BLACK;
  brdutilBoardPrint (&brd);

//  bytebrdPerft (1, &brd, &info, 0);
//  assert (400 == bytebrdPerft (2, &brd, &info, 1));
//  assert (400 == bytebrdPerft (2, &brd, &info, 0));
//  assert (8'902 == bytebrdPerft (3, &brd, &info, 0));
//  assert (197'281 == bytebrdPerft (4, &brd, &info, 0));
//  assert (4'865'609 == bytebrdPerft (5, &brd, &info, 0));
//  assert (119'060'324 == bytebrdPerft (6, &brd, &info, 0));
  assert (3'195'901'860 == bytebrdPerft (7, &brd, &info, 0));
//  assert (84'998'978'956 == bytebrdPerft (8, &brd, &info, 0));
//  assert (2'439'530'234'167 == bytebrdPerft (9, &brd, &info, 0));
//  assert (69'352'859'712'417 == bytebrdPerft (10, &brd, &info, 0));
//  bytebrdPerft (11, &brd, &info, 0);
}

void test_2(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
		 __FUNCTION__, __LINE__); 

#if 0
  brdutilFenToBrdConvert ("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
#endif

  brdutilStartPositionCreate(&brd, &info);

  brd.rc[3][0] = brd.rc[1][0];
  brd.rc[1][0] = 0;
  info.next_move = MOVE_BLACK;

  brd.rc[5][0] = brd.rc[6][0];
  brd.rc[6][0] = 0;
  info.next_move = MOVE_WHITE;

  brd.rc[4][0] = brd.rc[3][0];
  brd.rc[3][0] = 0;
  info.next_move = MOVE_BLACK;


  brd.rc[4][1] = brd.rc[6][1];
  brd.rc[6][1] = 0;
  info.next_move = MOVE_WHITE;
  info.next_move = MOVE_WHITE;
  info.en_passant_capture_eligible = 1;
  info.en_passant_row = 4;
  info.en_passant_column = 1;

#if 0

  brd.rc[6][3] = brd.rc[4][4];
  brd.rc[4][4] = 0;
  info.next_move = MOVE_BLACK;

  brd.rc[2][6] = brd.rc[6][2];
  brd.rc[6][2] = 0;
  info.next_move = MOVE_WHITE;
#endif

  brdutilBoardPrint (&brd);

  bytebrdPerft (1, &brd, &info, 0);
//  bytebrdPerft (2, &brd, &info, 0);
//  bytebrdPerft (3, &brd, &info, 0);
//  bytebrdPerft (4, &brd, &info, 0);
//  bytebrdPerft (5, &brd, &info, 0);
//  bytebrdPerft (6, &brd, &info, 0);
//  bytebrdPerft (7, &brd, &info, 0);
//  bytebrdPerft (8, &brd, &info, 0);
}

/**************************
** Pawn Promotion Test - Start Positio plus pawns pushed forward.
**************************/
void test_3(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
         __FUNCTION__, __LINE__);

  brdutilStartPositionCreate(&brd, &info);
  brd.rc[5][3] = brd.rc[1][3];
  brd.rc[1][3] = 0;

  brd.rc[2][4] = brd.rc[6][4];
  brd.rc[6][4] = 0;
  brdutilBoardPrint (&brd);


 // bytebrdPerft (6, &brd, &info, 0);
  bytebrdPerft (7, &brd, &info, 0);
}

/******************************************************
** Test various positions specified using FEN format.
******************************************************/
void test_4(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
         __FUNCTION__, __LINE__);

#if 0
  brdutilFenToBrdConvert ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
#endif

#if 0
  brdutilFenToBrdConvert ("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
#endif

#if 0
  brdutilFenToBrdConvert ("rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq c6 0 2",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
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
//  assert (1'612'761'451'963 == bytebrdPerft (7, &brd, &info, 0)); 
#endif

#if 1
  brdutilFenToBrdConvert ("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8 ",
                            &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
  assert (89'941'194 == bytebrdPerft (5, &brd, &info, 0));
#endif

//  brdutilFenPrint (&brd, &info);
//  brdutilBoardPrint (&brd);

 //  bytebrdPerft (1, &brd, &info, 0);
 //  bytebrdPerft (2, &brd, &info, 0);
 //  bytebrdPerft (4, &brd, &info, 0);
 // bytebrdPerft (5, &brd, &info, 0);
 // bytebrdPerft (6, &brd, &info, 0);
 // bytebrdPerft (7, &brd, &info, 0);
}

/******************************************************
** No Pawn Test.
******************************************************/
void test_5(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
         __FUNCTION__, __LINE__);

#if 1
  brdutilFenToBrdConvert ("rnbqkbnr/8/8/8/8/8/8/RNBQKBNR w KQkq - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
#endif

//  bytebrdPerft (1, &brd, &info, 0);
//  bytebrdPerft (2, &brd, &info, 0);
//  bytebrdPerft (3, &brd, &info, 0);
//  bytebrdPerft (4, &brd, &info, 0);
//  bytebrdPerft (5, &brd, &info, 0);
  assert (8'509'434'052 == bytebrdPerft (6, &brd, &info, 0));
//  assert (390'020'558'283 == bytebrdPerft (7, &brd, &info, 0));

}


/******************************************************
** 10 Knights Test
******************************************************/
void test_6(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
         __FUNCTION__, __LINE__);

#if 1
  brdutilFenToBrdConvert ("1n2k1n1/nnnnnnnn/8/8/8/8/NNNNNNNN/1N2K1N1 w ---- - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
#endif

//  bytebrdPerft (1, &brd, &info, 0);
//  bytebrdPerft (2, &brd, &info, 0);
//  bytebrdPerft (3, &brd, &info, 0);
//  bytebrdPerft (4, &brd, &info, 0);
//  bytebrdPerft (5, &brd, &info, 0);
  assert (4'250'961'040 == bytebrdPerft (6, &brd, &info, 0));
//  assert (173'882'876'905 == bytebrdPerft (7, &brd, &info, 0));

}

/******************************************************
** 10 Bishops Test
******************************************************/
void test_7(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
         __FUNCTION__, __LINE__);

#if 1
  brdutilFenToBrdConvert ("2b1kb2/bbbbbbbb/8/8/8/8/BBBBBBBB/2B1KB2 w ---- - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
#endif

//  bytebrdPerft (1, &brd, &info, 0);
//  bytebrdPerft (2, &brd, &info, 0);
//  bytebrdPerft (3, &brd, &info, 0);
//  bytebrdPerft (4, &brd, &info, 0);
//  assert (499'636'594 == bytebrdPerft (5, &brd, &info, 0));
  assert (25'561'635'836 == bytebrdPerft (6, &brd, &info, 0));
//  assert (1'312'333'356'411 == bytebrdPerft (7, &brd, &info, 0));

}

/******************************************************
** 10 Rooks Test
******************************************************/
void test_8(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
         __FUNCTION__, __LINE__);

#if 1
  brdutilFenToBrdConvert ("2r1kr2/rrrrrrrr/8/8/8/8/RRRRRRRR/2R1KR2 w ---- - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
#endif

//  bytebrdPerft (1, &brd, &info, 0);
//  bytebrdPerft (2, &brd, &info, 0);
//  bytebrdPerft (3, &brd, &info, 0);
//  bytebrdPerft (4, &brd, &info, 0);
//  bytebrdPerft (5, &brd, &info, 0);
    bytebrdPerft (6, &brd, &info, 0);
//  bytebrdPerft (7, &brd, &info, 0);

}

/******************************************************
** 10 Queens Test
******************************************************/
void test_9(void)
{
  brd_t brd;
  brdCtrlInfo_t info;

  printf ("%s - %d - Starting...\n",
         __FUNCTION__, __LINE__);

#if 1
  brdutilFenToBrdConvert ("2q1kq2/qqqqqqqq/8/8/8/8/QQQQQQQQ/2Q1KQ2 w ---- - 0 1",
                        &brd, &info);
  brdutilFenPrint (&brd, &info);
  brdutilBoardInfoPrint (&info);
  brdutilBoardPrint (&brd);
#endif

//    assert (101 == bytebrdPerft (1, &brd, &info, 0));
//    assert (9'375 == bytebrdPerft (2, &brd, &info, 0));
//    assert (896'887 == bytebrdPerft (3, &brd, &info, 0));
//    assert (82'582'230 == bytebrdPerft (4, &brd, &info, 0));
    assert (7'787'814'083 == bytebrdPerft (5, &brd, &info, 0));
//    assert (711'551'147'012 == bytebrdPerft (6, &brd, &info, 0));
//  bytebrdPerft (7, &brd, &info, 0);
//  bytebrdPerft (8, &brd, &info, 0);
//  bytebrdPerft (9, &brd, &info, 0);

}



void run_tests(void)
{
  test_1();  /* Standard Starting Position */
//  test_2();  /* Castle Test */
//  test_3();  /* Pawn Promotion Test */
//  test_4();  /* Fen Tests */
//  test_5();  /* No Pawn Test */
//  test_6();  /* 10 Knights Test */
//  test_7();  /* 10 Bishops Test */
//  test_8();  /* 10 Rook Test */
//  test_9();  /* 10 Queen Test */
//  test_m_1(); /* Developed Middle Game */
//  test_m_2(); /* Early Middle Game */
  test_m_3(); /* Kiwi */
  test_m_4(); /* Leaderboard Test */
  test_e_1(); /* End Game with King/Rook/Bishop vs King/Queen */
//  test_e_2(); /* End Game with King vs King/Bishop/Knight */
  test_e_3(); /* End Game with King/Pawns vs King/Pawns */
  test_e_4(); /* End Game with only two kings */

}

void perftInit (void)
{
  /* Enable printf() to format integers using comma separators. For example
  ** printf ("%'u", val) prints 100,000,000 instead of 100000000.
  ** Note the ' between % and u. This triggers printf() to use
  ** the country code formatting conventions.
  */
  setenv ("LC_ALL","en_US.UTF-8",1);
  setlocale (LC_NUMERIC, "");

#if 1
#ifdef __linux__
  {
    /* Locking the thread to core 0 produces more consistent and faster results.
    ** This makes it easier to see when code changes make a difference
    ** in system performance.
    */
    cpu_set_t cpu_set;
    const unsigned int my_core_number = 0;

    CPU_ZERO (&cpu_set);
    CPU_SET (my_core_number, &cpu_set);
    (void) pthread_setaffinity_np(pthread_self(),sizeof(cpu_set),&cpu_set);
  }
#endif
#endif

  bytebrdInit ();
}

int main (int argc, char *argv[])
{
  unsigned int depth;
  char position[256] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

  if (argc == 1)
  {
    printf ("Starting perft with no arguments. Running default test(s)...\n");
  } else 
  {
    depth = (unsigned int) atoi (argv[1]);

    if (argc == 2)
    {
      printf ("Starting perft with depth %u and with the standard starting position.\n", depth);
    }

    if (depth == 0)
    {
      printf ("Depth must be a non-zero value. Exiting....\n");
      printf ("Ivalid arguments. Example Usage:\n");
      printf ("perft\n");
      printf ("perft 7\n");
      printf ("perft 8 \"%s\"\n", position);
      exit (0);
    }

    if (argc == 3)
    {
      strcpy (position, argv[2]);
      printf ("Starting \"perft %u %s\"\n", depth, position);
    } 

    if (argc > 3)
    {
      printf ("Ivalid arguments. Example Usage:\n");
      printf ("perft\n");
      printf ("perft 7\n");
      printf ("perft 8 \"%s\"\n", position);
      exit (0);
    }
  } 

  perftInit ();

  if (argc == 1)
  {
    run_tests();
  } else
  {
    brd_t brd;
    brdCtrlInfo_t info;

    brdutilFenToBrdConvert (position,
                        &brd, &info);
    brdutilFenPrint (&brd, &info);
    brdutilBoardInfoPrint (&info);
    brdutilBoardPrint (&brd);

    bytebrdPerft (depth, &brd, &info, 0);
  }
  printf ("Finished  perft.\n");
}
