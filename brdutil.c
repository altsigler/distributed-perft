/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include "brdutil_api.h"


/******************************************************
*******************************************************
**
** API Functions
**
*******************************************************
******************************************************/

/********************************************************************
** Create a board with the default starting position.
**
** brd - (output) 8x8 chess board. 
** info - (output) Additional game description.
********************************************************************/
void brdutilStartPositionCreate (brd_t *brd, brdCtrlInfo_t *info)
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

  brd->rc[0][1] = S_KNIGHT | S_WHITE;
  brd->rc[0][6] = S_KNIGHT | S_WHITE;

  brd->rc[0][2] = S_BISHOP | S_WHITE;
  brd->rc[0][5] = S_BISHOP | S_WHITE;

  brd->rc[1][0] = S_PAWN | S_WHITE;
  brd->rc[1][1] = S_PAWN | S_WHITE;
  brd->rc[1][2] = S_PAWN | S_WHITE;
  brd->rc[1][3] = S_PAWN | S_WHITE;
  brd->rc[1][4] = S_PAWN | S_WHITE;
  brd->rc[1][5] = S_PAWN | S_WHITE;
  brd->rc[1][6] = S_PAWN | S_WHITE;
  brd->rc[1][7] = S_PAWN | S_WHITE;

    /* Starting position of black pieces.
  */
  brd->rc[7][4] = S_KING | S_BLACK;

  brd->rc[7][3] = S_QUEEN | S_BLACK;

  brd->rc[7][0] = S_ROOK | S_BLACK;
  brd->rc[7][7] = S_ROOK | S_BLACK;

  brd->rc[7][1] = S_KNIGHT | S_BLACK;
  brd->rc[7][6] = S_KNIGHT | S_BLACK;

  brd->rc[7][2] = S_BISHOP | S_BLACK;
  brd->rc[7][5] = S_BISHOP | S_BLACK;

  brd->rc[6][0] = S_PAWN | S_BLACK;
  brd->rc[6][1] = S_PAWN | S_BLACK;
  brd->rc[6][2] = S_PAWN | S_BLACK;
  brd->rc[6][3] = S_PAWN | S_BLACK;
  brd->rc[6][4] = S_PAWN | S_BLACK;
  brd->rc[6][5] = S_PAWN | S_BLACK;
  brd->rc[6][6] = S_PAWN | S_BLACK;
  brd->rc[6][7] = S_PAWN | S_BLACK;
}

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
           char *const fen_out)
{
  char skip;
  unsigned char p1;
  char fen[16] = {0,'P','N','B','R','Q','K',0,0,'p','n','b','r','q','k',0};
  int castle_count = 0;
  unsigned int next_char = 0;

  for (int row = 7; row >= 0; row--)
  {
    skip = 0;
    for (int column = 0; column < 8; column++)
    {
      p1 = brd->rc[row][column];
      if (p1 == 0)
      {
        skip++;
      }
      if ((p1 != 0) && (0 != skip))
      {
        fen_out[next_char++] = '0' + skip;
        skip = 0;
      }
      if (fen[p1] != 0)
      {
        fen_out [next_char++] = fen[p1];
      }
    }
    if (skip != 0)
    {
      fen_out[next_char++] = '0' + skip;
    }

    if (row > 0)
    {
      fen_out[next_char++] = '/';
    }
  }
  fen_out[next_char++] = ' ';

  if (info->next_move == MOVE_WHITE)
  {
    fen_out[next_char++] = 'w';
    fen_out[next_char++] = ' ';
  } else
  {
    fen_out[next_char++] = 'b';
    fen_out[next_char++] = ' ';
  }

   
   if (info->white_short_castle_eligible)
   {
     fen_out[next_char++] = 'K';
     castle_count++;
   }
   if (info->white_long_castle_eligible)
   {
     fen_out[next_char++] = 'Q';
     castle_count++;
   }
   if (info->black_short_castle_eligible)
   {
     fen_out[next_char++] = 'k';
     castle_count++;
   }
   if (info->black_long_castle_eligible)
   {
     fen_out[next_char++] = 'q';
     castle_count++;
   }
   if (0 == castle_count)
   {
     fen_out[next_char++] = '-';
   }

   fen_out[next_char++] = ' ';

  if (info->en_passant_capture_eligible)
  {
    fen_out[next_char++] = (char) colName[info->en_passant_column];
    if (info->next_move == MOVE_WHITE)
    {
      fen_out[next_char++] = (char) rowName[info->en_passant_row + 1];
    } else
    {
      fen_out[next_char++] = (char) rowName[info->en_passant_row - 1];
    }
    
  } else
  {
    fen_out[next_char++] = '-';
  }

  fen_out[next_char++] = ' ';
  fen_out[next_char++] = '0';
  fen_out[next_char++] = ' ';
  fen_out[next_char++] = '1';

  fen_out[next_char++] = '\n';
  fen_out[next_char++] = 0;
}

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
		   const brdCtrlInfo_t *const info)
{
  char fen[128];
  brdutilBrdToFenConvert (brd, info, fen);
  printf ("%s", fen);
}

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
		   brdCtrlInfo_t *const info)
{
  unsigned int index = 0;
  char val;
  int column;
  int skip;

  memset (brd, 0, sizeof (brd_t));

  for (int row = 7; row >= 0; row--)
  {
    column = 0;
    do 
    {
      val = fen[index++];
      switch (val)
      {
        case 'r':
          brd->rc[row][column++] = S_ROOK | S_BLACK;
        break;

        case 'n':
          brd->rc[row][column++] = S_KNIGHT | S_BLACK;
        break;

        case 'b':
          brd->rc[row][column++] = S_BISHOP | S_BLACK;
        break;

        case 'q':
          brd->rc[row][column++] = S_QUEEN | S_BLACK;
        break;

        case 'k':
          brd->rc[row][column++] = S_KING | S_BLACK;
        break;

        case 'p':
          brd->rc[row][column++] = S_PAWN | S_BLACK;
        break;

        case 'R':
          brd->rc[row][column++] = S_ROOK | S_WHITE;
        break;

        case 'N':
          brd->rc[row][column++] = S_KNIGHT | S_WHITE;
        break;

        case 'B':
          brd->rc[row][column++] = S_BISHOP | S_WHITE;
        break;

        case 'Q':
          brd->rc[row][column++] = S_QUEEN | S_WHITE;
        break;

        case 'K':
          brd->rc[row][column++] = S_KING | S_WHITE;
        break;

        case 'P':
          brd->rc[row][column++] = S_PAWN | S_WHITE;
        break;

        case '/':
        break; 

        case ' ':
        break; 

        default:
          skip = (val - '1') + 1;
          assert ((skip >= 1) && (skip <= 8));
          column += skip;
        break;
      }

    } while ((val != '/') && (val != ' '));
  }


  val = fen[index++];
  assert ((val == 'b') || (val == 'w'));

  if (val == 'w')
        info->next_move = MOVE_WHITE;
     else
        info->next_move = MOVE_BLACK;

  val = fen[index++];
            
  info->white_long_castle_eligible = 0; /* 0-Not Eligible, 1-Eligible */
  info->white_short_castle_eligible = 0; /* 0-Not Eligible, 1-Eligible */
  info->black_long_castle_eligible = 0; /* 0-Not Eligible, 1-Eligible */
  info->black_short_castle_eligible = 0; /* 0-Not Eligible, 1-Eligible */

  if (val != '-')
  {
    val = fen[index++];
    while (val != ' ')
    {
      if (val == 'k')
                    info->black_short_castle_eligible = 1;

      if (val == 'q')
                    info->black_long_castle_eligible = 1;

      if (val == 'K')
                    info->white_short_castle_eligible = 1;

      if (val == 'Q')
                    info->white_long_castle_eligible = 1;

      val = fen[index++];
    }
  }

  val = fen[index++];

  info->en_passant_capture_eligible = 0;
  info->en_passant_row = 0;
  info->en_passant_column = 0;

  if (val != '-')
  {
    info->en_passant_capture_eligible = 1;
    info->en_passant_column = ((unsigned int) (val - 'a')) & 0x7;

    val = fen[index++];

    info->en_passant_row = ((unsigned int) (val - '1')) & 0x7;

    if (info->next_move == MOVE_WHITE)
    {
      info->en_passant_row--;
    } else
    {
      info->en_passant_row++;
    }
  }

}

/********************************************************************
** Get system up time in milliseconds.
** The up time is measured since the computer powered up.
**
** Return Value:
** Time in milliseconds.
********************************************************************/
unsigned long long brdutilUpTimeMillisecondsGet(void)
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


/*********************************************************************
** Convert a 128 bit integer into printable string.
**
**
*********************************************************************/
char *int128ToStr(unsigned _BitInt(128) val,
                         char *const buf,
                         const unsigned int buf_size)
{
  unsigned int i = buf_size - 1;
  char tmp_buf[5];
  unsigned int tmp_buf_size;
  buf[i--] = 0;

  do
  {
    const unsigned int rem = (unsigned int) (val % 1000);
    val /= 1000;

    if (val == 0)
    {
      if (rem >= 100)
      {
        tmp_buf_size = 3;
      } else if (rem >= 10)
      {
        tmp_buf_size = 2;
      } else
      {
        tmp_buf_size = 1;
      }
      i -= tmp_buf_size;
      sprintf (tmp_buf, "%u",  rem);
      memcpy (&buf[i], tmp_buf,  tmp_buf_size);
      break;
    } else
    {
      tmp_buf_size = 4;
      i -= tmp_buf_size;
      sprintf (tmp_buf, ",%03u", rem);
      memcpy (&buf[i], tmp_buf,  tmp_buf_size);
    }
  } while (1);

  return (&buf[i]);
}

/********************************************************************
** Interface with external agent using the UCI protocol.
** This function returns when the external agent issues the 
** go perft <depth> command.
**
** The caller should initialize the board to the standard starting 
** position before calling this function for the first time.
** The caller should not modify the board on subsequent calls.
**
** brd - (output) 8x8 chess board.
** info - (output) Additional game description.
** perft_depth - (output) Set to non 0 when perft needs to be performed.
** hash_enable - (output) 1-Enable Hash, 0-Disable Hash
**
** Return Codes
**  -1 - Exit
**   1 - Compute the Perft value. 
**   2 - Enable/Disable Hash.
**
********************************************************************/
int brdutilUciPerftDepthGet (
		   brd_t *const brd, 
		   brdCtrlInfo_t *const info,
           unsigned int *const perft_depth,
           unsigned int *const hash_enable)
{
  char buf[256];

  do 
  {
    char *str = fgets (buf, sizeof(buf), stdin);

    if (str == 0)
    {
      exit (-1);
    }

    if (0 == strncmp(buf, "uci", 3))
    {
      printf ("id name distributed-perft\n");
      printf ("id author Andrey Tsigler\n");
      printf ("uciok\n");
      fflush (stdout);
      continue;
    }

    if (0 == strncmp(buf, "isready", 7))
    {
      printf ("  readyok\n");
      fflush (stdout);
      continue;
    }

    if (0 == strncmp(buf, "quit", 4))
    {
      return -1;
    }

    if (0 == strncmp (buf, "position fen", 12))
    {
      brdutilFenToBrdConvert (&buf[13], brd, info);
      continue; 
    }

    if (0 == strncmp (buf, "go perft", 8))
    {
      const unsigned int depth = (unsigned int) atoi (&buf[9]);

      if (depth < 1)
      {
        printf ("ERROR: depth %d is not supported.\n", depth);
        exit (-1);
      }
      *perft_depth = depth;
      return 1;

    }

    if (0 == strncmp (buf, "setoption name CacheEnable value 1", 34))
    {
      *hash_enable = 1;
      return 2;
    }

    if (0 == strncmp (buf, "setoption name CacheEnable value 0", 34))
    {
      *hash_enable = 0;
      return 2;
    }
  } while (1);

}

/*********************************************************************
**********************************************************************
** Debug Functions
**********************************************************************
*********************************************************************/

/*********************************************************************
** Display the board control information.
**
** info - (Input) Board Control Information.
**
*********************************************************************/
void brdutilBoardInfoPrint (const brdCtrlInfo_t *const info)
{
  printf ("Whose Move:%s\n", (info->next_move == MOVE_WHITE)?"White":"Black");
  printf ("White Short Castle:%s\n", (info->white_short_castle_eligible)?"Eligible":"Not Eligible");
  printf ("White Long Castle:%s\n", (info->white_long_castle_eligible)?"Eligible":"Not Eligible");
  printf ("Black Short Castle:%s\n", (info->black_short_castle_eligible)?"Eligible":"Not Eligible");
  printf ("Black Long Castle:%s\n", (info->black_long_castle_eligible)?"Eligible":"Not Eligible");
  printf ("En Passant Capture:%s\n", (info->en_passant_capture_eligible)?"Eligible":"Not Eligible");
  if (info->en_passant_capture_eligible)
  {
    printf ("En Passant Pawn Position:%c%c\n", 
                colName[info->en_passant_column],
                rowName[info->en_passant_row]);
  }

}

/*********************************************************************
** Display the current position on the console.
**
** brd - (Input) Current position.
**
*********************************************************************/
void brdutilBoardPrint (const brd_t *const brd)
{
  int i, j;
  int v_color, h_color;

  v_color = S_WHITE;
  for (j = (BRDS-1); j >= 0; j--)
  {
    printf ("%d ", j+1);
    h_color = v_color;
    for (i = 0; i < BRDS; i++)
    {
      if (brd->rc[j][i] == 0)
      {
        if (h_color == S_BLACK)
        {
          printf ("  ");
        } else
        {
          printf ("--");
        };
      } else
      {
        if ((brd->rc[j][i] & S_COLOR_MASK) == S_WHITE)
        {
          printf ("\033[7m");
        }
        switch (brd->rc[j][i] & S_PIECE_MASK)
        {
          case S_PAWN:
            printf ("p");
          break;
          case S_KNIGHT:
            printf ("N");
          break;
          case S_BISHOP:
            printf ("B");
          break;
          case S_ROOK:
            printf ("R");
          break;
          case S_QUEEN:
            printf ("Q");
          break;
          case S_KING:
            printf ("K");
          break;
        }
        if ((brd->rc[j][i] & S_COLOR_MASK) == S_WHITE)
        {
          printf ("\033[0m");
        }
        if (h_color == S_BLACK)
        {
          printf (" ");
        } else
        {
          printf ("-");
        };
      }
      h_color = (h_color == S_BLACK)?S_WHITE:S_BLACK;
    }
    printf ("\n");
    v_color = (v_color == S_BLACK)?S_WHITE:S_BLACK;
  }
  printf ("  ");
  for (i = 0; i < BRDS; i++)
  {
    printf ("%c ", colName[i]);
  }
  printf ("\n\n");

}

