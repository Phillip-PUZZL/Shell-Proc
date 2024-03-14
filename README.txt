Compiling:
The 'Makefile' has 4 options when running
1) 'make all' or 'make' will compile both shell.c and proc_parse.c and will place their binaries into a
   folder called 'bin' on the same level as the 'src' folder.
2) 'make shell' will compile only the shell.c file and place it into the bin folder.
3) 'make proc' will compile only the proc_parse.c file and place it into the bin folder.
4) 'make clean' will delete the bin folder alongside all compile binaries and subdirectories.


Running:
The process parser can be run as follows
 - The compiled binary from above will be named 'proc_parse' so running it is as simple as './bin/proc_parse'
   from inside of the root folder for this project for the first "non-continuous" version
 - To run the process parser using the "continuous" version, you can use './bin/proc_parse <read_rate> <print_rate>'
   e.g. './bin/proc_parse 1 2' to have it read the /proc filesystem every second and print averages over 2 second
   intervals.
 - Usage information can be seen by using './bin/proc_parse -h'

The shell can be run as follows
 - The shell has no command line arguments and the compiled binary will be named 'shell' so running it would be
   './bin/shell'


Shell features:
1) This shell supports the 'cd' command
   a) Must provide an argument 'cd <argument>' e.g. 'cd bin'
   b) To cd to directories with spaces in them, you must use '\ ' instead of regular spaces. e.g. 'cd test\ folder'
   c) The cd command will not cause the program to exit if any arguments are incorrect

2) This shell supports the 'jobs' command
   a) To view currently running jobs you can simply type 'jobs'

3) This shell supports foreground and background processes through 'fg' and 'bg' respectively
   a) To start a job as a background process, simply add '&' to the end of it. e.g. 'ls -la &'
   b) You can also start a job in the foreground with something like 'man chdir' then by hitting CTRL-Z to suspend it
   c) A suspended job can be restarted in the background by using 'bg <job ID>' (job ID is viewable using the 'jobs' command).
      You can also type 'bg' without a job ID and it will use the most recently started job.
   d) You can move a job to the foreground using 'fg <job ID>' (job ID is viewable using the 'jobs' command).
      You can also type 'fg' without a job ID and it will place the most recently started job into the foreground.

4) This shell supports piping
   a) To pipe a command using this shell, simply create a pipeline. e.g. 'cat Makefile | grep bin'
   b) You can run a pipeline in the background with the '&' symbol. e.g. 'cat Makefile | grep bin &'
   c) Testing for defective piping is performed and will not exit the shell.
      e.g. '| grep bin', ' | grep bin', 'cat Makefile | | grep bin', 'cat Makefile |' all will
      simply prvide an invalid operator error.
   d) All file descriptors are closed.

5) This shell supports boolean operations
   a) To use boolean operations in this shell, you can simply use '||' or '&&' symbols. e.g. 'cat Makefile && ls -la'
      will run both 'cat Makefile' and 'ls -la' but 'yeet && ls -la' will simply show an error for the 'yeet' command.
      This works on both '&&' and '||' operators.
   b) 'A && B' -> B will only run if A completed successfully. 'A || B' -> B will only run if A did not complete successfully
   c) Testing for defective operators is perfomed and will not exit the shell.
      e.g. '&& ls -la', 'cat Makefile && && ls -la', 'cat Makefile ||' will all simply provide an error.
   d) You can operate pipes with the boolean operations and they will work in the background
      e.g. 'cat Makefile | grep bin && ls -la &'

6) This shell uses execvp() to run programs which it does not know
   a) If the shell determines that the command being ran is not implicit to the shell
      it will use execvp() to attempt to run it. If it fails, it will simply return an
      error and not exit the shell.


Exiting both programs:
1) To exit the shell, simply type 'exit' or you can use 'CTRL-C' to SIGINT.
With both programs, on exit or SIGINT, it will free all allocated memory and kill
any threads and child processes which are running.
