/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#ifndef ONECORE_API_H_INCLUDED
#define ONECORE_API_H_INCLUDED

#include "brdutil_api.h"

/********************************************************************
** Override scaling and operating mode for the scperft application.
**
** This function is not thread safe, and should only be called
** during initialization. 
**
** This function is not required to be called, and is used only when 
** default scaling parameters are not suitable or position caching 
** needs to be disabled.
**
** The caller is responsible for passing correct values. There is 
** no error checking of any king in this function.
**
** The caller may leave one or more databases at their default values
** by passing 0 as the new scaling value.
**
********************************************************************/
void onecoreScalingOverride (const unsigned int  brd_plies,
                             const unsigned long long db_entries,
                             const unsigned long long move_db_entries,
                             const unsigned long long hash_entries);

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
unsigned long long onecorePerft (const unsigned int depth,
                   const brd_t *const brd,
                   const brdCtrlInfo_t *const info,
                   const unsigned int silent);

#endif /* ONECORE_API_H_INCLUDED */
