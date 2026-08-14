/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <assert.h>
#include <errno.h>
#include "bytebrd_api.h"
#include "mcperft_api.h"
#include "mcperft_defs.h"
#include "mcperft.h"

/******************************************************************************
** Initialize the application.
**
** Return Values:
******************************************************************************/
void mcperftInit(void)
{
  /* Enable printf() to format integers using comma separators. For example
  ** printf ("%'u", val) prints 100,000,000 instead of 100000000.
  ** Note the ' between % and u. This triggers printf() to use
  ** the country code formatting conventions.
  */
  setenv ("LC_ALL","en_US.UTF-8",1);
  setlocale (LC_NUMERIC, "");

  bytebrdInit();
}

/********************************************************************
** Generate the board database for the tandard starting position.
**
********************************************************************/
unsigned int mcperftBoardDbGenerate (unsigned int ply_depth)
{
  brd_t brd;
  brdCtrlInfo_t info;
  unsigned long long max_positions;
  unsigned long long max_moves;

  brdutilStartPositionCreate(&brd, &info);
  brdutilBoardPrint (&brd);

  if (ply_depth == 7)
  {
    max_positions = CHESS_MAX_POSITIONS_7;
    max_moves = CHESS_MAX_MOVES_7;
  } else if (ply_depth == 8)
  {
    max_positions = CHESS_MAX_POSITIONS_8;
    max_moves = CHESS_MAX_MOVES_8;
  } else if (ply_depth == 9)
  {
    max_positions = CHESS_MAX_POSITIONS_9;
    max_moves = CHESS_MAX_MOVES_9;
  } else
  {
    printf ("ERROR: Unexpected ply depth %u.\n", ply_depth);
    exit (-1);
  }

  brdDbGenerate (ply_depth, max_positions, max_moves, &brd, &info);

  return 0;
}
/********************************************************************
** Perform deep search on the specified workload.
**
** workload_file -  Work load file name.
**
** Return Codes
**  Position Status
**
********************************************************************/
void mcperftCount (const char *workload_file)
{
  printf ("Performing deep search for positions in workload file:%s\n", 
                    workload_file);
  brdDbCount (workload_file);
}

/********************************************************************
** Create files containing fen positions for each ply in the database.
**
** Return Codes
**  None
**
********************************************************************/
void mcperftFenGenerate (void)
{
  brdDbFenGenerate();
}

/********************************************************************
** Generate Deep Search worklod files for the specified search depth.
** The second parameter indicates into how many work files to split
** the deep search. Each of the sub-files contains part of the 
** deep search workload.
**
** depth -  Must be 1 or greater.
** split_factor  - Must be 1 to MAX_SEARCH_WORKLOADS.
**
** Return Codes
**  Position Status
**
********************************************************************/
void mcperftCountSetup (const unsigned int depth,
                        const unsigned int split_factor)
{
  if (depth < 1)
  {
    printf ("ERROR: Unsupprted depth:%'u\n", depth);
    exit(-1);
  }

  if ((split_factor < 1) || (split_factor > MAX_SEARCH_WORKLOADS))
  {
    printf ("ERROR: Unsupported split count:%'u\n", split_factor);
    exit (-1);
  }

  printf ("Generating Move Count Workloads for \"perft %u\" split to %u search machines...\n",
                    depth, split_factor);

  brdDbCountSetup (depth, split_factor);
}

                                 
/********************************************************************
** Analyze all the result files and compute the final perft value.
**
** Return Codes
**  None
**
********************************************************************/
void mcperftAggregate (void)
{
  constexpr unsigned _BitInt(128) known_perft_results[16] = 
  {
    0, 
    20,
    400,
    8'902,
    197'281,
    4'865'609,
    119'060'324,
    3'195'901'860,
    84'998'978'956,
    2'439'530'234'167,
    69'352'859'712'417,
    2'097'651'003'696'806,
    62'854'969'236'701'747,
    1'981'066'775'000'396'239,
    61'885'021'521'585'529'237wb,
    2'015'099'950'053'364'471'960wb
  };
  unsigned _BitInt(128) perft_result;
  unsigned _BitInt(128) ply1_perft_result[MAX_BRD_MOVES];
  unsigned int depth;
  char buf[256];

  brdDbAggregate (&depth, &perft_result, ply1_perft_result);

  printf ("Perft Depth:%u\n", depth);

  /* Get the move list for ply 1.
  */
  {
    brd_t brd;
    brdCtrlInfo_t info;

    brdutilStartPositionCreate(&brd, &info);

    bytebrdMove_t move_list[MAX_BRD_MOVES];
    unsigned int top_num_moves;
    unsigned int mover_lost;

    top_num_moves = bytebrdNextMoveGet (&brd, &info, move_list, &mover_lost);

    for (unsigned int i = 0; i < top_num_moves; i++)
    {
      constexpr char piece_name[] = {0,0,'n','b','r','q',0,0};

      printf ("%2u - ", i+1);

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
      printf (" - %s", int128ToStr(ply1_perft_result[i], buf, sizeof(buf)));
      printf ("\n");
    }
    printf ("\n");
  }

  printf ("Perft Count:%s\n", int128ToStr(perft_result, buf, sizeof(buf)));
  if (depth < 16)
  {
    if (perft_result == known_perft_results[depth])
    {
      printf ("SUCCESS!! - Result matches known perft value for this depth.\n");
    } else
    {
      printf ("FAILURE - Expected perft value is:%s\n", 
                        int128ToStr(known_perft_results[depth], buf, sizeof(buf)));
    }
  } else
  {
    printf ("Unknown perft value for depth:%u\n", depth);
  }
}
