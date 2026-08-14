/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "mcperft_api.h"

int main (int argc, char *argv[])
{
  (void) argc;

  printf ("Starting %s...\n", argv[0]);
  mcperftInit ();

  if (argc > 1)
  {
    if (0 == strncmp (argv[1], "generate-fen", 12))
    {
      printf ("Creating files for each ply containing unique positions in FEN notation.\n");
      printf ("The FEN lists for each ply are not sorted.\n");
      mcperftFenGenerate ();
    } else
    if (0 == strncmp (argv[1], "create-db", 10))
    {
      unsigned int ply_depth = 7;
      if (argc == 3)
      {
        ply_depth = (unsigned int) atoi (argv[2]);
      }
      if ((ply_depth < 7) || (ply_depth > 9))
      {
        printf ("ERROR: The database ply %u is invalid. Valid values are 7, 8, or 9.\n", ply_depth);
        exit (-1);
      }
      printf ("Creating Database of Unique Positions to ply %u...\n\n", ply_depth);
      mcperftBoardDbGenerate (ply_depth);
    } else
    if (0 == strncmp (argv[1], "count-setup", 12))
    {
      if ((argc < 3) || (argc > 4))
      {
        printf ("ERROR: count-setup must have one or two argument.\n");
        exit (-1);
      }
      const unsigned int depth = (unsigned int) atoi (argv[2]);
      if (depth < 1)
      {
        printf ("ERROR: Invalid depth:%u\n", depth);
        exit (-1);
      }

      const unsigned int split_factor =  (argc == 4)?(unsigned int) atoi(argv[3]):1;

      mcperftCountSetup (depth, split_factor);
    } else
    if (0 == strncmp (argv[1], "count", 6))
    {
      char workload_file_name[1024];

      if (argc == 2)
      {
        strcpy (workload_file_name, "workload_1");
      } else
      {
        strncpy (workload_file_name, argv[2], sizeof(workload_file_name) - 1);
      }

      mcperftCount (workload_file_name);
    } else 
    if (0 == strncmp (argv[1], "aggregate", 10))
    {
      mcperftAggregate ();
    } else
    {
      printf ("Unknown Command. Valid commands are:\n");
      printf ("%s create-db [ply]             - Create Position Database to specified ply. Default 7, valid values 7, 8, or 9.\n", argv[0]);
      printf ("%s count-setup <depth> [split] - Set up \"perft <depth>\" and split the workload into specified number of files. Default 1 file.\n", argv[0]);
      printf ("%s count [workload-file]       - Count positions for each workload in specified file. Default file 'workload_1'.\n", argv[0]);
      printf ("%s aggregate                   - Use computed move counts in board-db/results directory to calculate perft result.\n", argv[0]);
      printf ("%s generate-fen                - Create unsorted lists of uique positions for each ply in FEN format.\n", argv[0]);
    }
  } else
  {
    printf ("Invoked %s without arguments.\n", argv[0]);
    printf ("Automatically executing the following commands:\n");
    printf ("%s create-db 7\n", argv[0]);
    printf ("%s count-setup 10 1\n", argv[0]);
    printf ("%s count workload_1\n", argv[0]);
    printf ("%s aggregate\n", argv[0]);
    mcperftBoardDbGenerate (7);
    mcperftCountSetup (10, 1);
    mcperftCount ("workload_1");
    mcperftAggregate ();
  }

  printf ("Finished %s.\n", argv[0]);
}
