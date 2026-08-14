/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#ifndef BRDUTIL_API_H_INCLUDED
#define BRDUTIL_API_H_INCLUDED

/* Chess Piece Color 
*/
typedef enum {
     MOVE_WHITE= 0,
     MOVE_BLACK= 1
}  color_e;
#define MOVE_COLORS 2

/* Numerical identifier for each piece type.
*/
#define S_EMPTY   0  /* Square is empty. */
#define S_PAWN    1  /* Pawn */
#define S_KNIGHT  2  /* Knight */
#define S_BISHOP  3  /* Bishop */
#define S_ROOK    4  /* Rook */
#define S_QUEEN   5  /* Queen */
#define S_KING    6  /* King */

/* The mask for retrieving the square occupant from the square
** information byte.
*/
#define S_PIECE_MASK 0x07


/* The mask for retrieving the piece color from the square 
** information byte.
*/
#define S_COLOR_MASK 0x08
#define S_WHITE      (MOVE_WHITE << 3)
#define S_BLACK      (MOVE_BLACK << 3)

#define PIECE_GET(_p)  ((_p) & S_PIECE_MASK)
#define COLOR_GET(_p)  ((_p) & S_COLOR_MASK)


/* The Chess Board is 8x8 squares.
** The first index in the rc array is row, and the second is column.
** The content of each square is the piece type and piece color.
*/
#define BRDS 8

/* The maximum number of moves that a reachable position can have.
*/
#define MAX_BRD_MOVES 218

constexpr unsigned char colName[BRDS]={'a','b','c','d','e','f','g','h'};
constexpr unsigned char rowName[BRDS]={'1','2','3','4','5','6','7','8'};

typedef struct
{
  unsigned short next_move:1;  /* MOVE_BLACK -Black to move next, MOVE_WHITE -White to move next */
  unsigned short white_long_castle_eligible:1; /* 0-Not Eligible, 1-Eligible */
  unsigned short white_short_castle_eligible:1; /* 0-Not Eligible, 1-Eligible */
  unsigned short black_long_castle_eligible:1; /* 0-Not Eligible, 1-Eligible */
  unsigned short black_short_castle_eligible:1; /* 0-Not Eligible, 1-Eligible */
  unsigned short en_passant_capture_eligible:1; /* 0-Not Eligible, 1-Eligible */
  unsigned short en_passant_row:3; /* 0 to 7 */
  unsigned short en_passant_column:3; /* 0(a) to 7(h) */
} brdCtrlInfo_t;


typedef struct 
{
  /*               1-8   a-h  */
  unsigned char rc[BRDS][BRDS];
} brd_t __attribute__ ((aligned (8)));

/********************************************************************
** Create a board with the default starting position.
**
** brd - (output) 8x8 chess board. 
** info - (output) Additional game description.
********************************************************************/
void brdutilStartPositionCreate (brd_t *brd, brdCtrlInfo_t *info);


/********************************************************************
** Convert FEN string to a board structure.
**
** fen - (input) Number of plies to search.
** brd - (output) 8x8 chess board.
** info - (output) Additional game description.
**
** Return Codes
**  None
**
********************************************************************/
void brdutilFenToBrdConvert (const char *const fen,
           brd_t *const brd,
           brdCtrlInfo_t *const info);

/********************************************************************
** Convert board position into a FEN string.
**          
** brd - (output) 8x8 chess board.
** info - (output) Additional game description.
** fen_out - (output) STring containing the FEN position.
**  
** Return Codes
**  None
**    
********************************************************************/
void brdutilBrdToFenConvert (
           const brd_t *const brd,
           const brdCtrlInfo_t *const info,
           char *const fen_out);


/********************************************************************
** Print out the FEN string for the specified board.
**
** brd - (output) 8x8 chess board.
** info - (output) Additional game description.
**
** Return Codes
**  None
**
********************************************************************/
void brdutilFenPrint (
           const brd_t *const brd,
           const brdCtrlInfo_t *const info);

/********************************************************************
** Get system up time in milliseconds.
** The up time is measured since the computer powered up.
**
** Return Value:
** Time in milliseconds.
********************************************************************/
unsigned long long brdutilUpTimeMillisecondsGet(void);

/*********************************************************************
** Convert a 128 bit integer into printable string.
**
**
*********************************************************************/
char *int128ToStr(unsigned _BitInt(128) val,
                         char *const buf,
                         const unsigned int buf_size);

/********************************************************************
** Interface with external agent using the UCI protocol.
** This function returns when the external agent issues the 
** go perft <depth> command.
**
** The caller should initialize the board to the standard starting 
** position before calling this function.
**
** brd - (output) 8x8 chess board.
** info - (output) Additional game description.
** perft_depth - (output) Set to non 0 when perft needs to be performed.
** hash_enable - (output) 1-Enable Hash, 0-Disable Hash
**
** Return Codes
**  -1 - Exit
**  > 0 Perft depth.
**
********************************************************************/
int brdutilUciPerftDepthGet (
           brd_t *const brd, 
           brdCtrlInfo_t *const info,
           unsigned int *const perft_depth,
           unsigned int *const hash_enable);

/*********************************************************************
**********************************************************************
** Debug Functions
**********************************************************************
*********************************************************************/

/*********************************************************************
** Display the current position on the console.
**
** brd - (Input) Current position.
**
*********************************************************************/
void brdutilBoardPrint (const brd_t *const brd);

/*********************************************************************
** Display the board control information.
**
** info - (Input) Board Control Information.
**
*********************************************************************/
void brdutilBoardInfoPrint (const brdCtrlInfo_t *const info);


#endif /* BRDUTIL_API_H_INCLUDED */
