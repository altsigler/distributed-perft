# Distributed Perft 
The perft, which stands for performance test, is a debugging and testing
tool for chess programming. The perft counts all possible moves from 
a specified position to specified number of plies. 

The Distributed Perft is intended to count the number of possible positions
at ply 15 from the standard starting position on an x86 machine.

The mcperft program, which is the multicore distributed perft implementation,
distributes the move counting to all cores on the same machine
and also can distribute move counting to multiple machines. 

In addition to the mcperft application, this project includes a 
very fast single core "scperft" move counter. 
The scperft function is used by the mcperft when distributing work to multiple cores.

## Distributed Perft Acrhitecture
The distributed move counting operation is divided into four stages. 
Generate a tree of unique positions, create workload files for one or more 
machines, perform a single-core perft computation on each workload, 
and aggregate all workloads into the final perft result. 
Below chapters describe each stage in more detail.

Each stage can be invoked independently by passing an argument to the "mcperft"
application. If the mcperft application is invoked without any arguments then
it automatically generates a position tree of depth 7, creates one workload 
file for "perft 10", computes the single-core "perft 3" for every workload, 
and aggregates results into the final "perft 10" position count. In order 
to run the complete test, the computer must have at least 16GB of DRAM in order 
to have enough space to generate the depth 7 unique position tree.

### Generate a tree of unique positions
The first stage generates a tree of unique positions up to ply 7, 8, or 9.
The depth of this tree is limited by how much DRAM is available on the 
computer generating the tree. The required memory is 16GB, 128GB, and 1TB
respectively.  

The reason for generating a tree of unique positions, is to greatly reduce the 
number of computations needed for the perft count. The duplicate positions 
start showing up at ply 3, where about 40 percent of the positions are duplicate.
The duplicate percentage goes up with every ply after that to about XX percent
at ply 9. This means that the number of positions we need to compute drops 
roughly in half for plies 3 through 9, which is a big savings in computation time.

The nodes in the position tree are chess positions. The links in the tree are legal
chess moves to the next position. For example the root node, which is ply 0, contains
the starting position. The ply 1 contains 20 nodes, which correspond to the legally reachable
positions from the standard starting position. Ply 2 has 400 positions, and ply 3 has 5,362
unique positions. There are 3,540 duplicate positions in ply 3, so there are a total of 
8,902 links between ply 2 and ply 3 with 5,362 links leading to unique positions 
and 3,540 leading to duplicate positions. 

This proceeds until ply 9 with the following unique/duplicate position counts:
Ply 4 (72,078/46,444), Ply 5 (822,518/974/871), Ply 6 (9,417,683/10,779,500),
Ply 7 (96,400,335/160,579,818), Ply 8 (988,192,872/1,593,495,486), 
Ply 9 (XXX/YYY).

Once the tree for ply 9 is generated it can 
be used on 128GB machine to perform workload generartion and final perft count 
aggregation. The single core move counting machines can have just 1GB of DRAM and
are not impacted by the position tree size.

If you have access to a 128GB machine then use the command "./mcperft create-db 8" to 
create the position database of depth 8. 
If the depth is omitted then the application uses depth 7 by default. 
The position database file is called position_db and is located in the board-db
subdirectory, which is created automatically in the current working directory.

The database only needs to be created one time. Subsequent commands to generate 
workloads for different perft depths and to trigger perft computation can be 
done using the same database. 

It is best to keep the database outside of the working directory, and simply set up 
a symbolic link to it from the board-db subdirectory. This avoids accidental deletion
of the database, such as when running "make clean", which deletes the board-db subdirectory.

If the database is lost over the days, weeks or months while perft computation is taking place 
then it can be regenerated and used for perft count aggregation. The regenerated database 
of the same depth is guaranteed to be identical every time.

The final point about the position database depth is that it is not the same as the
perft computation count. Any of the database sizes 7, 8, or 9, can be used to
compute any perft value from "perft 1" to "perft 15". The database depth simply controls 
how deep the single core perft search for each workload needs to be. For example for "perft 10" with 
database of depth 7, the single core search is depth 3, while with position 
database of depth 8 the single core search is depth 2.

### Generate the workload files
The second stage generates one or more workload files which can be used to distribute the work of 
counting moves to multiple computers. In this stage is also when the perft depth is specified.
The workload file generation is done using the "./mcperft count-setup <depth> [split]" command. 
For example, to split the workload into 10 parts for "perft 13" use the command "./mcperft count-setup 13 10".

The workload files are named "workload_1" to "workload_10" and reside in the board-db/workload-files directory. If the number 
of workload files is omitted then only one file "workload_1" is generated. The workload file names must not be
modified. The mcperft relies on this file naming convention. 

The workload files are binary files, so are not user readable. The file contains a header that describes things 
such as search depth, the number of workloads, and which part of the overall workload this particular file covers.
After the file header, the workload file contains positions from the highest ply number for which the position 
database was created, so if in the first stage the position database was created using the "./mcperft create-db 8" then
the workload files contain 988,192,872 positions split approximately evenly between them.

This project doesn't include any tools to distribute the workload files to different computers. 
For testing distributed move counting, the workload_n files can be copied manually to other computers. 
The workload files must be placed in the board-db/workload-files directory on the move counting machines
in order for the ./mcperft application to use them.

In a special case when the requested perft depth is less than or equal to the position database ply depth then only one workload file
"workload_1" is generated. The workload cannot be split up for the small perft depths. In the above example 
depths 1 through 8 cannot be split up into multiple workload files. In this scenario the workload file contains some control 
information in the header and no positions.

Please be aware that the "./mcperft count-setup" command automatically deletes and re-creates the "board-db/workload-files
and "board-db/result-files" directories. The "result-files" is where the move counting results are kept. It is prudent to move result
files that you want to keep away from the working directory where the "./mcperft" is running.


### Perform move counting on each workload.
The third stage is to perform move counting for each position in all the workload files. The move counting is done using
the "./mcperft count [workload-file]" command. If the workload-file is omitted then the "workload_1" file is used.
The mcperft expects to find the workload file in the board-db/workload-files directory and expects to put result files
into board-db/result-files directory. Therefore when starting the ./mcperft application on different computers make sure 
to create the board-db/result-files and board-db/workload-files directory and compy the workload_n files into the 
workload-files directory. 

While the move counting is in progress the mcperft prints a message once per minute indicating how many workloads have been 
processed and some performance information. This information can be used to estimate when the move counting will finish.
While counting moves, the mcperft updates the board-db/result-files/temp_result file every few minutes with the counted positions.
If the counting is interruped with CTRL-C or due to power loss then the "./mcperft count" automatically restarts from 
the last checkpointed position. For counts that expect to take days, the user can set up scripts to automatically restart 
the count if the computer reboots.

When the count is finished the mcperft generates a board-db/result-files/result_n file, where the 'n' corresponds to the 
workload file number.

The computers that count the position in the workload files don't need access to the position database generated in the 
first stage, and only need about 1GB of DRAM. 


### Aggregate Results
The fourth stage is to aggregate all results in the broad-db/result-files into the final perft result.
The mcperft reads all "result_n" files in the result-files directory, and in conjunction with the position database 
generated in stage one, computes the final perft count for the perft depth set up in stage two.

This project doesn't provide any automation for gathering result_n files from different computers that performed the move counting, 
so this needs to be done manually for testing. The mcperft does perform checks to make sure that all the result files 
are present, but it is largely up to the sure to make sure that the database and result files match and all result 
files are present in the board-db/result-files directory.

Although the mcperft needs acces to the position database for the final perft computation, a computer with 128GB can 
perform this operation on the "ply 9" position database that required 1TB of DRAM to generate. 


## Other tools included in the project



