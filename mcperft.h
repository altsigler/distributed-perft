/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#ifndef MCPERFT_H_INCLUDED
#define MCPERFT_H_INCLUDED

/* This file deines constants that can be tuned by the user.
*/

/* Maximum positions and moves for the unique position database
** depth 7, 8, and 9. 
**
** At this time the only supported database depths are  7, 8, or 9 plies.
**
** The database depth is set with "mcperft create-db [ply]" command.
** If the [ply] is omitted then the depth is set to 7 plies.
**
** Setting depth to 7 can be used on 16GB DRAM machines for testing.
** Setting depth to 8 requires 128GB DRAM.
** Setting depth to 9 requires 1TB DRAM.
** 
** Note that this value is not the same as perft count depth. 
** The databse of 7, 8, or 9 can be used for any perft depth search,
** however in order to complete "perft 14" or "perft 15" in 
** reasonable time the database depth needs to be set to 9.
**
** The 1TB memory footprint for database of depth 9 is needed only 
** for database creation. The move counting can be done on machines
** with 1GB DRAM, and the final move count aggregation can be done 
** on a 128GB machine.
**
** The below definitions are tuned for the standard starting 
** chess position. The number of possible moves is relatively 
** small at the beginning of the game. 
** If this software is ever used for middle game positions then
** the values below will need to be increased significantly.
*/ 
#define CHESS_MAX_POSITIONS_7  110'000'000LLU
#define CHESS_MAX_MOVES_7 300'000'000LLU

#define CHESS_MAX_POSITIONS_8  1'100'000'000LLU
#define CHESS_MAX_MOVES_8 3'000'000'000LLU

#define CHESS_MAX_POSITIONS_9  12'000'000'000LLU
#define CHESS_MAX_MOVES_9 36'000'000'000LLU

#endif /* MCPERFT_H_INCLUDED */
