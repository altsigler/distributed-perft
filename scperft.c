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

#include "onecore_api.h"
#include "bytebrd_api.h"



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
  unsigned int depth = 7;
  char position[256] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

  if (argc > 1)
  {
    depth = (unsigned int) atoi (argv[1]);

    if (argc == 2)
    {
      printf ("Starting scperft with depth %u and with the standard starting position.\n", depth);
    }

    if (depth == 0)
    {
      printf ("Depth must be a non-zero value. Exiting....\n");
      printf ("Ivalid arguments. Example Usage:\n");
      printf ("scperft\n");
      printf ("scperft 7\n");
      printf ("scperft 8 \"%s\"\n", position);
      exit (0);
    }

    if (argc == 3)
    {
      strcpy (position, argv[2]);
      printf ("Starting \"scperft %u %s\"\n", depth, position);
    } 

    if (argc > 3)
    {
      printf ("Ivalid arguments. Example Usage:\n");
      printf ("scperft\n");
      printf ("scperft 7\n");
      printf ("scperft 8 \"%s\"\n", position);
      exit (0);
    }
  } 

  perftInit ();

  if (argc == 1)
  {
    brd_t brd;
    brdCtrlInfo_t info;

    brdutilStartPositionCreate (&brd, &info);
    do
    {
      unsigned int perft_depth;
      unsigned int hash_enable;

      const int rc = brdutilUciPerftDepthGet (&brd, &info,
                                               &perft_depth, 
                                               &hash_enable);
      if (rc <= 0)
      {
        break;
      }
      if (rc == 1)
      {
        const unsigned long long num_moves = onecorePerft (perft_depth, &brd, &info, 1);
        printf ("Nodes searched: %llu\n", num_moves);
        fflush (stdout);
        continue;
      }

      if (rc == 2)
      {
        if (hash_enable)
        {
          onecoreScalingOverride (4, 0, 0, 0);
        } else
        {
          onecoreScalingOverride (0, 0, 0, 0);
        }
      }
    } while (1);
  } else
  {
    brd_t brd;
    brdCtrlInfo_t info;

    brdutilFenToBrdConvert (position,
                        &brd, &info);
    brdutilFenPrint (&brd, &info);
    brdutilBoardInfoPrint (&info);
    brdutilBoardPrint (&brd);

    onecorePerft (depth, &brd, &info, 0);
  }
}
