/*
 * Copyright (c) 2026 Andrey Tsigler
 *
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file or at https://opensource.org/licenses/MIT.
 */
#ifndef MCPERFT_API_H_INCLUDED
#define MCPERFT_API_H_INCLUDED

#include "bytebrd_api.h"

/******************************************************************************
** Initialize the application.
**
** Return Values:
******************************************************************************/
void mcperftInit(void);

/********************************************************************
** Generate the position database.
********************************************************************/
unsigned int mcperftBoardDbGenerate (unsigned int ply_depth);

/********************************************************************
** Create files containing fen positions for each ply in the database.
**
** Return Codes
**  None
**
********************************************************************/
void mcperftFenGenerate (void);

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
                        const unsigned int split_factor);

/********************************************************************
** Perform deep search on the specified workload.
**
** workload_file -  Work load file name.
**
** Return Codes
**  Position Status
**
********************************************************************/
void mcperftCount (const char *workload_file);

/********************************************************************
** Analyze all the result files and compute the final perft value.
**
** Return Codes
**  None
**
********************************************************************/
void mcperftAggregate (void);



#endif /* MCPERFT_API_H_INCLUDED */
