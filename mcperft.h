/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#ifndef MCPERFT_H_INCLUDED
#define MCPERFT_H_INCLUDED

/* This file defines constants that can be tuned by the user.
*/


/* 
** Sort block size in bytes. 
** On platforms with smaller DRAM size, the code allocates only half of
** the total DRAM for the sort blocks, so the actual sort block size may be 
** smaller than the value specified below.
**
** The sort block is used for different data types.
**
** As positions are generated for the new ply, they are added to this block.
** After the block fills up, the positions are sorted, and then written to 
** a file.
** After all positions in the ply are processed, we may end up with one or 
** more sorted position block files. These files are then merged together 
** into the ply position file. The merge procedure removes duplicate positions
** and updates the previous ply move database.
** The sort block files are temporary, and are deleted after the ply position
** file is generated.
*/
//#define MAX_SORT_BLOCK_SIZE (20LLU*1024LLU*1024LLU*1024LLU)
#define MAX_SORT_BLOCK_SIZE (40LLU*1024LLU*1024LLU*1024LLU)

#endif /* MCPERFT_H_INCLUDED */
