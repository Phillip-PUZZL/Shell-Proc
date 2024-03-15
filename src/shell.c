#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/limits.h>

// Maximum input size for user input
#define INPUT_BUFFER_SIZE 1024

// Structure with job information like the command used to start it, process pid
// and pgid, the job number (used for fg and bg commands), whether the process
// is currently in the foreground, and when the process started
struct jobsMonitor {
    char *command;
    pid_t pid;
    pid_t pgid;
    int job_num;
    bool foreground;
    time_t started;
};

void sig_handler(int);

// Structure for directing signals to sig_handler
struct sigaction sigact;
// Used to tell the signal thread (main thread) which signal was thrown and to
// handle it. Needed because of restricted operations in the signal handler
volatile sig_atomic_t eflag = 0;
// pthread meant to handle user input
pthread_t input_thread;
// String meant to hold a trimmed string (removing excess spacing)
char *trimmed_input = NULL;
// String array to hold the tokens from user input
char **tokenized_input = NULL;
// String meant to hold the current working directory
char current_directory[PATH_MAX];
// String meant to hold the last directory in the CWD for displaying on the
// shell
char last_dir[PATH_MAX];
// Integer for the number of tokens made from the user input
int token_count = 0;
// Integer containing the current number of jobs running
int job_count = 0;
// Integer indicating the shell variable for stdout
int shell_terminal = STDOUT_FILENO;
// The base process pid and pgid
pid_t parent_pid;
pid_t parent_pgid;
// Current foreground pid
pid_t fg_pid;
// Whether the shell was printed (used in some instances when output occurs
// while waiting for user input so that the shell can still be printed)
bool shell_printed = false;
// Array of the job struct which contains information on currently running jobs
struct jobsMonitor *running_jobs = NULL;
// Mutex locks to ensure different aspects of the program cannot run at the same
// time
pthread_mutex_t job_lock, thread_lock, shell_lock;

// Change the current foreground pid
void change_fg_pid(pid_t pid) { fg_pid = pid; }

// Function which takes string input and trims excess spacing
char *trim(char trim_string[INPUT_BUFFER_SIZE]) {
    // Return string
    char *save_string;
    // Index to trim at
    int start_index = 0;
    // End index to stop trimming
    int end_index = (int) strlen(trim_string) - 1;

    // Find the beginning of the actual content
    while (start_index < (int) strlen(trim_string) &&
           isspace(trim_string[start_index])) {
        start_index++;
    }

    // Find the end of the actual content
    while (end_index >= 0 && isspace(trim_string[end_index])) {
        end_index--;
    }

    // Verification that any content exists at all
    if (start_index < (int) strlen(trim_string) && end_index >= start_index) {
        // Memory allocation
        save_string = (char *) malloc(sizeof(char) * (end_index - start_index + 2));
        memset(save_string, 0, end_index - start_index + 2);
        // Copy in the trimmed data
        strncpy(save_string, trim_string + start_index,
                end_index - start_index + 1);
        // Terminate the trimmed string
        save_string[(end_index - start_index + 1)] = '\0';

        // Return the trimmed value
        return save_string;
    }
    // If end or start indexes indicate something bad, return NULL
    return NULL;
}

// Function to concatenate all Strings in a String array
char *make_string_from_double_char(char **input) {
    // Return variable
    char *full_command;
    // Counting total letters
    int total_count = 0;
    // Counting total string entries
    int input_count = 0;

    // Calculate final string length and total number of entries
    for (int i = 0; input[i] != NULL; i++) {
        total_count += (int) strlen(input[i]);
        input_count++;
    }

    // Allocate memory (input_count used here to indicate spaces and termination
    // byte)
    full_command = (char *) malloc(sizeof(char) * (total_count + input_count));
    memset(full_command, 0, total_count + input_count);

    // Concatenate all content into the final string
    for (int i = 0; i < input_count; i++) {
        strcat(full_command, input[i]);
        // If the next entry is NULL, do not add the space so break
        if (input[i + 1] == NULL) {
            break;
        }
        // Adding the space between words
        strcat(full_command, " ");
    }
    // Termination byte added
    full_command[total_count + input_count - 1] = '\0';

    // Return result
    return full_command;
}

// Function which accepts an array of strings and splits it into a 2D array of
// strings which can be used for launching pipe or boolean expression jobs which
// require these 2D string arrays
// Example of performing this for pipes would be
// split_double_char_on_string(input_to_split, "|")
char ***split_double_char_on_string(char **input, char *split_string) {
    // Final 2D string array
    char ***final;
    // Total number of splits which need to be made, initialized at 1 since worst
    // case is no entries which will produce a 1 X y 2D string array where y is
    // just the length of the "input" parameter
    int num_splits = 1;

    // Get total number of splits
    for (int i = 0; input[i] != NULL; i++) {
        if (strcmp(input[i], split_string) == 0) {
            num_splits++;
        }
    }

    // Allocate top memory to the total number of split entities
    final = (char ***) malloc(sizeof(char **) * (num_splits + 1));
    memset(final, 0, num_splits + 1);

    // Total count is the number of characters in a string
    int total_count = 0;
    // Input count is the number of entries in final[x]
    int input_count = 0;
    // Number of splits reset to 0
    num_splits = 0;

    // Go through the input parameter until finds NULL termination and calculate
    // the number of entries between each instance of the "split string" before
    // allocating that much memory to them and continuing to the next split
    for (int i = 0; input[i] != NULL; i++) {
        if (strcmp(input[i], split_string) == 0) {
            final[num_splits] = (char **) malloc(sizeof(char *) * (input_count + 1));
            memset(final[num_splits], 0, input_count + 1);
            input_count = 0;
            num_splits++;
            continue;
        }
        input_count++;
    }

    // Final entry not allocated in that last loop so allocate here
    if (input_count > 0) {
        final[num_splits] = (char **) malloc(sizeof(char *) * (input_count + 1));
        memset(final[num_splits], 0, input_count + 1);
    }

    // Reset used variables
    num_splits = 0;
    input_count = 0;

    // Find the total number of characters in each string within the domain of
    // their split, then allocate that much memory to respective entries the in
    // the final array
    for (int i = 0; input[i] != NULL; i++) {
        if (strcmp(input[i], split_string) == 0) {
            input_count = 0;
            total_count = 0;
            num_splits++;
            continue;
        }
        for (int j = 0; input[i][j] != '\0'; j++) {
            total_count++;
        }
        final[num_splits][input_count] =
                (char *) malloc(sizeof(char) * (total_count + 1));
        memset(final[num_splits][input_count], 0, total_count + 1);
        total_count = 0;
        input_count++;
    }

    // Reset used variables
    num_splits = 0;
    input_count = 0;

    // Finally, assign respective strings to their positions within the final
    // array, add termination byte, and NULL terminate them
    for (int i = 0; input[i] != NULL; i++) {
        if (strcmp(input[i], split_string) == 0) {
            final[num_splits][input_count] = NULL;
            num_splits++;
            input_count = 0;
            continue;
        }
        strcpy(final[num_splits][input_count], input[i]);
        final[num_splits][input_count][(int) strlen(input[i])] = '\0';
        input_count++;
    }

    // Final entry is NULL terminated
    final[num_splits + 1] = NULL;

    // Return the result
    return final;
}

// This function is essentially the same as the last one, but instead of
// splitting on a single delimiter, with this one you can split on multiple. For
// example, you can pass a char ** of ["||\0", "&&\0", NULL] in order to split
// on all of the boolean expression operators supported by this shell.
// Essentially a copy of the last function, but using loops to check the
// delimiter instead of having a single one
char ***split_double_char_on_string_list(char **input, char **split_string) {
    char ***final;
    int num_splits = 1;

    for (int i = 0; input[i] != NULL; i++) {
        for (int j = 0; split_string[j] != NULL; j++) {
            if (strcmp(input[i], split_string[j]) == 0) {
                num_splits++;
                break;
            }
        }
    }

    final = (char ***) malloc(sizeof(char **) * (num_splits + 1));
    memset(final, 0, num_splits + 1);

    int total_count = 0;
    int input_count = 0;
    num_splits = 0;
    bool broke = false;

    for (int i = 0; input[i] != NULL; i++) {
        for (int j = 0; split_string[j] != NULL; j++) {
            if (strcmp(input[i], split_string[j]) == 0) {
                final[num_splits] = (char **) malloc(sizeof(char *) * (input_count + 1));
                memset(final[num_splits], 0, input_count + 1);
                input_count = 0;
                num_splits++;
                broke = true;
                break;
            }
        }
        if (broke) {
            broke = false;
            continue;
        }
        input_count++;
    }

    if (input_count > 0) {
        final[num_splits] = (char **) malloc(sizeof(char *) * (input_count + 1));
        memset(final[num_splits], 0, input_count + 1);
    }

    num_splits = 0;
    input_count = 0;
    broke = false;

    for (int i = 0; input[i] != NULL; i++) {
        for (int j = 0; split_string[j] != NULL; j++) {
            if (strcmp(input[i], split_string[j]) == 0) {
                input_count = 0;
                total_count = 0;
                num_splits++;
                broke = true;
                break;
            }
        }
        if (broke) {
            broke = false;
            continue;
        }
        for (int j = 0; input[i][j] != '\0'; j++) {
            total_count++;
        }
        final[num_splits][input_count] =
                (char *) malloc(sizeof(char) * (total_count + 1));
        memset(final[num_splits][input_count], 0, total_count + 1);
        total_count = 0;
        input_count++;
    }

    num_splits = 0;
    input_count = 0;
    broke = false;

    for (int i = 0; input[i] != NULL; i++) {
        for (int j = 0; split_string[j] != NULL; j++) {
            if (strcmp(input[i], split_string[j]) == 0) {
                final[num_splits][input_count] = NULL;
                num_splits++;
                input_count = 0;
                broke = true;
                break;
            }
        }
        if (broke) {
            broke = false;
            continue;
        }
        strcpy(final[num_splits][input_count], input[i]);
        final[num_splits][input_count][(int) strlen(input[i])] = '\0';
        input_count++;
    }

    final[num_splits + 1] = NULL;

    return final;
}

// Function for removing a job from the running_jobs pointer which contains all
// currently running jobs
void remove_job(pid_t pid, int exit_code, bool text, bool from_sig) {
    // Lock the mutex so other thread cannot make changes to jobs while performing
    pthread_mutex_lock(&job_lock);

    // Find which job is being removed from its pid
    int job_index = -1;
    for (int i = 0; i < job_count; i++) {
        if (running_jobs[i].pid == pid) {
            job_index = i;
            break;
        }
    }

    // If the job is not in the list, simply return
    if (job_index == -1) {
        return;
    }

    // Informing the shell about the jobs termination
    if (text && !running_jobs[job_index].foreground) {
        if (exit_code == 1) {
            printf("\n[%d] + terminated  %s\n", running_jobs[job_index].job_num,
                   running_jobs[job_index].command);
        } else if (exit_code == 0) {
            printf("\n[%d] + done  %s\n", running_jobs[job_index].job_num,
                   running_jobs[job_index].command);
        } else {
            printf("\n[%d] + terminated with code %d  %s\n",
                   running_jobs[job_index].job_num, exit_code,
                   running_jobs[job_index].command);
        }
        if (from_sig) {
            // One of the instances where printing the shell again is necessary since
            // job completion can be reported at any time
            pthread_mutex_lock(&shell_lock);
            printf("COSC-6325/701Shell %s $ ", last_dir);
            fflush(stdout);
            shell_printed = true;
            pthread_mutex_unlock(&shell_lock);
        }
    }

    // Free the memory from the command portion
    free(running_jobs[job_index].command);

    if (job_index == job_count - 1) {
        // If the terminated job is the last in the list, do not need to change
        // others and can simply reallocate memory
        running_jobs = (struct jobsMonitor *) realloc(
                running_jobs, sizeof(struct jobsMonitor) * (job_count - 1));
    } else {
        // If the terminated job is in between others, need to move them down the
        // list and adjust job numbers to reflect current state, then can reallocate
        // memory
        for (int j = job_index; j < job_count - 1; j++) {
            running_jobs[j] = running_jobs[j + 1];
            running_jobs[j].job_num = j;
        }
        running_jobs = (struct jobsMonitor *) realloc(
                running_jobs, sizeof(struct jobsMonitor) * (job_count - 1));
    }

    // Reduce job count by 1
    job_count--;
    pthread_mutex_unlock(&job_lock);
}

// Easy function to free all allocated global memory and kill all child
// processes
void free_memory_kill_children() {
    // Free the trimmed input
    if (trimmed_input != NULL) {
        free(trimmed_input);
    }

    // Free all strings inside tokenized_input then free the array itself
    if (tokenized_input != NULL) {
        for (int i = 0; i < token_count; i++) {
            if (tokenized_input[i] != NULL) {
                free(tokenized_input[i]);
            }
        }
    }
    if (tokenized_input != NULL) {
        free(tokenized_input);
    }

    // Loop through all jobs, free the command variable associated, if running
    // then sent SIGKILL to terminate them, then free array itself
    if (running_jobs != NULL) {
        for (int i = 0; i < job_count; i++) {
            if (running_jobs[i].pid == parent_pid) {
                free(running_jobs[i].command);
                continue;
            }
            int status;
            while (waitpid(running_jobs[i].pid, &status, WNOHANG) == -1) {
                if (errno != EINTR) {
                    break;
                }
            }
            if (WIFEXITED(status)) {
                killpg(running_jobs[i].pid, SIGKILL);
            }
            free(running_jobs[i].command);
        }
        free(running_jobs);
    }

    // Stop the user input thread
    pthread_cancel(input_thread);
}

// Returning the signal handler to its default state
void sig_cleanup() { sigemptyset(&sigact.sa_mask); }

// Create the signal handler to take SIGINT and SIGCHLD and associate them with
// sig_handler function
void sig_init() {
    sigact.sa_handler = sig_handler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, (struct sigaction *) NULL);
    sigaction(SIGCHLD, &sigact, (struct sigaction *) NULL);
}

// Signal handler function which changes the current flag for the main function
// to handle
void sig_handler(int sig) {
    if (sig == SIGINT) {
        eflag = SIGINT;
    } else if (sig == SIGCHLD) {
        eflag = SIGCHLD;
    }
}

// Listing all currently running jobs
void list_jobs() {
    pthread_mutex_lock(&job_lock);
    // Loop through all jobs, excluding the parent job
    for (int i = 0; i < job_count; i++) {
        if (running_jobs[i].pid == parent_pid) {
            continue;
        }
        // Find the status of the job, if no longer running then remove it from the
        // list
        int status;
        while (waitpid(running_jobs[i].pid, &status, WNOHANG) == -1) {
            if (errno != EINTR) {
                break;
            }
        }

        if (WIFEXITED(status)) {
            remove_job(running_jobs[i].pid, WEXITSTATUS(status), true, false);
        }
    }

    // Print information about the jobs including their PID, job number, when they
    // started, and with which command
    printf("%6s%12s%30s\t%s\n", "PID", "JOB NUMBER", "STARTED", "COMMAND");
    for (int i = 0; i < job_count; i++) {
        printf("%6d%12d%30s\t%s\n", running_jobs[i].pid, running_jobs[i].job_num,
               trim(asctime(localtime(&running_jobs[i].started))),
               running_jobs[i].command);
    }
    pthread_mutex_unlock(&job_lock);
}

// Adding a new job to the list
void add_job_to_list(char **params, pid_t pid, bool text, bool foreground) {
    pthread_mutex_lock(&job_lock);
    // Get the current time in order to add it to the job information
    time_t started;
    time(&started);

    // Increase job count
    job_count++;

    if (running_jobs == NULL) {
        // If this is the first job added, allocate just one entry
        running_jobs = (struct jobsMonitor *) malloc(sizeof(struct jobsMonitor));
    } else {
        // Subsequent entries reallocate memory to adjust
        running_jobs = (struct jobsMonitor *) realloc(
                running_jobs, sizeof(struct jobsMonitor) * job_count);
    }

    // Set all job variables
    running_jobs[job_count - 1].command = make_string_from_double_char(params);
    running_jobs[job_count - 1].pid = pid;
    running_jobs[job_count - 1].pgid = getpgid(pid);
    running_jobs[job_count - 1].started = started;
    running_jobs[job_count - 1].job_num = job_count - 1;
    running_jobs[job_count - 1].foreground = foreground;

    // If it is a background job, print the job number and its PID
    if (text) {
        printf("[%d] %d\n", job_count - 1, pid);
    }
    pthread_mutex_unlock(&job_lock);
}

// Function to launch a job using execvp() from a child process
void launch_job(char *program, char **arguments, bool background) {
    execvp(program, arguments);
    // execvp() failure forces us to print information about failed task and exit
    char *full_command = make_string_from_double_char(arguments);
    if (background) {
        printf("\nCOSC-6325/701Shell: bash: Error running %s\n", full_command);
    } else {
        printf("COSC-6325/701Shell: bash: Error running %s\n", full_command);
    }
    free(full_command);
    exit(2);
}

// Launching a pipe job, this is a recursive function
int launch_pipe_job(char ***commands, int inputfd, bool background) {
    // Child process PID
    pid_t pid;
    // Status of child process
    int status;
    // Command used to run child process
    char *full_command;

    // If this is the final recurse, commands[1] will be NULL so no need to
    // continue recursing
    if (commands[1] == NULL) {
        // Fork the process
        pid = fork();

        if (pid == 0) {
            // If child process check the inputfd, which is the input file descriptor
            // for this child. If it exists (not initial -1 which is used to start the
            // recursion) then duplicate the descriptor and close it
            if (inputfd != -1) {
                dup2(inputfd, STDIN_FILENO);
                close(inputfd);
            }
            // Launch the job
            launch_job(commands[0][0], commands[0], background);
        } else if (pid < 0) {
            // Failure to fork forces us to inform the user, close the file descriptor
            // if it exists, and exit the child process which will report this issue
            // up the chain
            full_command = make_string_from_double_char(commands[0]);
            if (background) {
                printf("\nCOSC-6325/701Shell: Error forking for \"%s\"\n",
                       full_command);
            } else {
                printf("COSC-6325/701Shell: Error forking for \"%s\"\n", full_command);
            }
            free(full_command);
            if (inputfd != -1) {
                close(inputfd);
            }
            exit(2);
        }
        // If it is the parent process then it will execute this
        // Close the fd
        if (inputfd != -1) {
            close(inputfd);
        }

        // Wait for its child process to finish and return the result of running
        while (waitpid(pid, &status, WUNTRACED) == -1) {
            if (errno != EINTR) {
                break;
            }
        }
        return WEXITSTATUS(status);
    } else {
        // This is not the final command to run in the pipeline, so this all runs
        // Declare the file descriptors used in the pipeline
        int fds[2];
        // Pipe the fds
        if (pipe(fds) != 0) {
            // If there are issues piping then report it and exit the process
            full_command = make_string_from_double_char(commands[0]);
            if (background) {
                printf("\nCOSC-6325/701Shell: Error piping on %s\n", full_command);
            } else {
                printf("COSC-6325/701Shell: Error piping on %s\n", full_command);
            }
            free(full_command);
            exit(2);
        }
        // Fork a new child process
        pid = fork();

        if (pid == 0) {
            // This is the child process
            // If this is not the first child process run in the pipeline then it will
            // duplicate the file descriptor.
            if (inputfd != -1) {
                dup2(inputfd, STDIN_FILENO);
                close(inputfd);
            }
            // Duplicate the output file descriptor for piping data to the next
            // process
            dup2(fds[1], STDOUT_FILENO);
            close(fds[1]);
            // Launch the job
            launch_job(commands[0][0], commands[0], background);
        } else if (pid < 0) {
            // Failure to fork forces us to inform the user, close the file descriptor
            // if it exists, and exit the child process which will report this issue
            // up the chain
            full_command = make_string_from_double_char(commands[0]);
            if (background) {
                printf("\nCOSC-6325/701Shell: Error forking for \"%s\"\n",
                       full_command);
            } else {
                printf("COSC-6325/701Shell: Error forking for \"%s\"\n", full_command);
            }
            free(full_command);
            close(fds[1]);
            if (inputfd != -1) {
                close(inputfd);
            }
            exit(2);
        }
        // Parent process closes descriptors
        close(fds[1]);
        if (inputfd != -1) {
            close(inputfd);
        }

        // Parent process launches next pipe in the pipeline
        status = launch_pipe_job(++commands, fds[0], background);

        // Parent closes the file descriptor
        close(fds[0]);

        // Parent process waits for the child process to exit then returns the
        // result
        while (waitpid(pid, NULL, WUNTRACED) == -1) {
            if (errno != EINTR) {
                break;
            }
        }
        return status;
    }
}

// Launching a job that includes a boolean expression of '&&' or '||'
int launch_op_job(char ***commands, char **ops, int prev_value,
                  bool background) {
    // Child PID
    pid_t pid;
    // Status of next recurse
    int status;
    // Child status
    int wait_status;
    // Is this current command piped?
    bool piped = false;
    // Current value (-1 for initial or error, 0 for false, 1 for true) to pass on
    // to the next entry
    int curr_val = -1;
    // The current operator either '||\0' or '&&\0'
    // The full command used within this entry
    char *full_command;

    // Checks whether this entry is piped
    for (int i = 0; commands[0][i] != NULL; i++) {
        if (strcmp(commands[0][i], "|") == 0) {
            piped = true;
            break;
        }
    }

    // If commands[1] is NULL then this is the final entry and can return without
    // recursing
    if (commands[1] == NULL) {

        // Checks to see if the previous entry returned true when it is an OR
        // operator, no need to check, it is true regardless
        if (strcmp(ops[0], "||\0") == 0 && prev_value == 1) {
            return 0;
            // Checks to see if the previous entry returned false when it is an AND
            // operator, no need to check, it is false regardless
        } else if (strcmp(ops[0], "&&\0") == 0 && prev_value == 0) {
            return 1;
        }

        // Fork the process
        pid = fork();

        if (pid == 0) {
            // Child process
            // Checks if it is piped
            if (!piped) {
                // If not piped, launch a regular job
                launch_job(commands[0][0], commands[0], background);
            } else {
                // If it is piped, create a new 2D string array containing the pipe
                // entries
                char ***piped_entry = split_double_char_on_string(commands[0], "|");
                bool bad_pipes = false;
                // Verify that the pipe entries are good
                for (int i = 0; piped_entry[i] != NULL; i++) {
                    if (piped_entry[i][0] == NULL) {
                        printf("COSC-6325/701Shell: Invalid operator: Must be an "
                               "expression on "
                               "either side of '|'\n");
                        bad_pipes = true;
                        break;
                    }
                }
                if (bad_pipes) {
                    // Free pipes memory
                    for (int i = 0; piped_entry[i] != NULL; i++) {
                        for (int j = 0; piped_entry[i][j] != NULL; j++) {
                            free(piped_entry[i][j]);
                        }
                        free(piped_entry[i]);
                    }
                    free(piped_entry);
                    exit(1);
                }
                // Launch a piped job and exit with the status returned
                int job_status = launch_pipe_job(piped_entry, -1, background);
                // Free pipes memory
                for (int i = 0; piped_entry[i] != NULL; i++) {
                    for (int j = 0; piped_entry[i][j] != NULL; j++) {
                        free(piped_entry[i][j]);
                    }
                    free(piped_entry[i]);
                }
                free(piped_entry);
                exit(job_status);
            }
        } else if (pid < 0) {
            // Failure to fork forces us to inform the user, close the file descriptor
            // if it exists, and exit the child process which will report this issue
            // up the chain
            full_command = make_string_from_double_char(commands[0]);
            if (background) {
                printf("\nCOSC-6325/701Shell: Error forking for \"%s\"\n",
                       full_command);
            } else {
                printf("COSC-6325/701Shell: Error forking for \"%s\"\n", full_command);
            }
            free(full_command);
            exit(1);
        } else {
            // Parent process waits for the child to finish then returns its result
            while (waitpid(pid, &wait_status, WUNTRACED) == -1) {
                if (errno != EINTR) {
                    break;
                }
            }
            return WEXITSTATUS(wait_status);
        }
    } else {
        // This is not the final entry, so more recursion necessary
        // Checks to see if the previous entry returned true when it is an OR
        // operator, no need to check, it is true regardless
        if (strcmp(ops[0], "||\0") == 0 && prev_value == 1) {
            if (ops[1] != NULL) {
                return launch_op_job(++commands, ++ops, 1, background);
            }
            // Checks to see if the previous entry returned false when it is an AND
            // operator, no need to check, it is false regardless
        } else if (strcmp(ops[0], "&&\0") == 0 && prev_value == 0) {
            if (ops[1] != NULL) {
                return launch_op_job(++commands, ++ops, 0, background);
            }
        }

        // For the process
        pid = fork();

        if (pid == 0) {
            // Child process
            // Checks if it is piped
            if (!piped) {
                // If not piped, launch a regular job
                launch_job(commands[0][0], commands[0], background);
            } else {
                // If it is piped create a new 2D string containing the pipes
                char ***piped_entry = split_double_char_on_string(commands[0], "|");
                bool bad_pipes = false;
                // Verify that the pipes are good
                for (int i = 0; piped_entry[i] != NULL; i++) {
                    if (piped_entry[i][0] == NULL) {
                        printf("COSC-6325/701Shell: Invalid operator: Must be an "
                               "expression on "
                               "either side of '|'\n");
                        bad_pipes = true;
                        break;
                    }
                }
                if (bad_pipes) {
                    // Free pipes memory
                    for (int i = 0; piped_entry[i] != NULL; i++) {
                        for (int j = 0; piped_entry[i][j] != NULL; j++) {
                            free(piped_entry[i][j]);
                        }
                        free(piped_entry[i]);
                    }
                    free(piped_entry);
                    exit(1);
                }
                // Launch a pipe job and exit with the result
                int job_status = launch_pipe_job(piped_entry, -1, background);
                // Free pipes memory
                for (int i = 0; piped_entry[i] != NULL; i++) {
                    for (int j = 0; piped_entry[i][j] != NULL; j++) {
                        free(piped_entry[i][j]);
                    }
                    free(piped_entry[i]);
                }
                free(piped_entry);
                exit(job_status);
            }
        } else if (pid < 0) {
            // Failure to fork forces us to inform the user, close the file descriptor
            // if it exists, and exit the child process which will report this issue
            // up the chain
            full_command = make_string_from_double_char(commands[0]);
            if (background) {
                printf("\nCOSC-6325/701Shell: Error forking for \"%s\"\n",
                       full_command);
            } else {
                printf("COSC-6325/701Shell: Error forking for \"%s\"\n", full_command);
            }
            free(full_command);
            exit(1);
        } else {

            // Parent process waits for child to return
            // Had some issues with this one receiving a signal which interrupted its
            // system calls so had to add the loop to check
            while (waitpid(pid, &wait_status, WUNTRACED) == -1) {
                if (errno != EINTR) {
                    break;
                }
            }

            // Checks its exit status and get a true or false result from whether the
            // command was successfull (0 return status) or not so much (any other
            // return status)
            if (WIFEXITED(wait_status)) {
                if (WEXITSTATUS(wait_status) == 0) {
                    curr_val = 1;
                } else {
                    curr_val = 0;
                }
            } else {
                printf("COSC-6325/701Shell: Error with boolean expression\n");
                return -1;
            }

            // If this current recursion is the first, need to keep the operator the
            // same. If it is not the first, all others will immediately be
            // incremented since they will already have left hand results
            if (prev_value == -1) {
                status = launch_op_job(++commands, ops, curr_val, background);
            } else {
                status = launch_op_job(++commands, ++ops, curr_val, background);
            }

            // Return the final status
            return status;
        }
        return -1;
    }
    return -1;
}

// bg command function
void switch_background(char **input) {
    // Index for which job it is trying to run in the background
    int job_num;
    // If no input is supplied other than "bg", uses the most recently started job
    // (the last)
    if (input[1] == NULL) {
        job_num = job_count - 1;

        // If most recent job is the parent process, report that none exist
        if (job_num == 0) {
            printf("COSC-6325/701Shell: fg: No background processes running\n");
            return;
        }
    } else {
        // If a job number is supplied "bg 1" or something alike, verify that the
        // submitted parameter is a valid number
        for (int i = 0; i < (int) strlen(input[1]); i++) {
            if (!isdigit(input[1][i])) {
                printf("COSC-6325/701Shell: bg: Job specifier must be a number\n");
                return;
            }
        }

        // Convert the user inputted job entry to a number and verify that it exists
        job_num = atoi(input[1]);
        if (job_num < 1 || job_num >= job_count) {
            printf("COSC-6325/701Shell: bg: Invalid job number\n");
            return;
        }
    }

    // Get the pgid of the job
    pid_t pgid = running_jobs[job_num].pgid;

    // Send the continue signal to the job
    killpg(pgid, SIGCONT);

    // Report to the user that the job has continued
    printf("[%d] + continued  %s\n", running_jobs[job_num].job_num,
           running_jobs[job_num].command);
}

// fg command function
void switch_foreground(char **input, bool text) {
    // Index for the job in running_jobs struct pointer
    int job_num = 0;
    // If just "fg" as the command, selects most recent job
    if (input[1] == NULL) {
        job_num = job_count - 1;

        // If most recent job is the parent process, report that none exist
        if (job_num == 0) {
            printf("COSC-6325/701Shell: fg: No background processes running\n");
            return;
        }
    } else {
        // If a job number is supplied "fg 1" or something similar, ensure that the
        // inputted data is a number
        for (int i = 0; i < (int) strlen(input[1]); i++) {
            if (!isdigit(input[1][i])) {
                printf("COSC-6325/701Shell: fg: Invalid job number %s\n", input[1]);
                return;
            }
        }

        // Verify that the number is an existing job
        job_num = atoi(input[1]);
        if (job_num < 1 || job_num >= job_count) {
            printf("COSC-6325/701Shell: fg: Invalid job number %s\n", input[1]);
            return;
        }
    }

    // Get the pid and pgid of the job
    pid_t pid = running_jobs[job_num].pid;
    pid_t pgid = running_jobs[job_num].pgid;

    // Put the job into the foreground.
    tcsetpgrp(shell_terminal, pid);

    // If this is continuing from the background, report that the shell is
    // continuing the job
    if (text) {
        printf("[%d] + continued  %s\n", running_jobs[job_num].job_num,
               running_jobs[job_num].command);
    }

    // Change the current foreground pid number
    change_fg_pid(pid);

    // Send the job a continue signal
    killpg(pgid, SIGCONT);

    // Wait for the job to report.
    int status;
    while (waitpid(pid, &status, WUNTRACED) == -1) {
        if (errno != EINTR) {
            break;
        }
    }

    // Put the shell back in the foreground.
    tcsetpgrp(shell_terminal, parent_pid);

    // Sometimes output doesn't appear immediately after tcsetpgrp() because of
    // reasons. usleep for 100 nanoseconds seems to solve the issue
    usleep(100);

    // Change the foreground pid number back to the parent
    change_fg_pid(parent_pid);

    // The job may have stopped rather than completed, if so, don't remove it.
    if (!WIFEXITED(status)) {
        running_jobs[job_num].foreground = false;
        printf("[%d] + suspended  %s\n", running_jobs[job_num].job_num,
               running_jobs[job_num].command);
    }
}

// Function for launching a job not containing a boolean expression
int launch_job_no_op(char **command, int param_count, bool piped,
                     bool background) {
    // What program is it running
    char *program = (char *) malloc(sizeof(char) * ((int) strlen(command[0]) + 1));
    memset(program, 0, (int) strlen(command[0] + 1));
    // Program parameters
    char **params = (char **) malloc(sizeof(char *) * (param_count + 1));
    memset(params, 0, param_count + 1);
    // 2D string array in case it is piped
    char ***split_params = NULL;

    // Copy and terminate the program variable
    strcpy(program, command[0]);
    program[(int) strlen(command[0])] = '\0';

    // Get all of the parameters into the params String array and NULL terminate
    // it
    for (int i = 0; i < param_count; i++) {
        params[i] = (char *) malloc(sizeof(char) * ((int) strlen(command[i]) + 1));
        memset(params[i], 0, (int) strlen(command[i] + 1));
        strcpy(params[i], command[i]);
        params[i][(int) strlen(command[i])] = '\0';
    }
    printf("\n");
    params[param_count] = NULL;

    if (piped) {
        // If the program is piped, create the 2D string array with all of the pipes
        // and verify that it is valid
        split_params = split_double_char_on_string(params, "|");
        for (int i = 0; split_params[i] != NULL; i++) {
            if (split_params[i][0] == NULL) {
                printf("COSC-6325/701Shell: Invalid operator: Must be an expression on "
                       "either side of '|'\n");
                return 1;
            }
        }
    }

    // Fork the process
    pid_t pid;
    pid = fork();

    if (pid < 0) {
        // Error forking, notify user
        printf("COSC-6325/701Shell: Could not fork process\n");
    } else if (pid == 0) {
        // Child process
        // Need to set pgid in case running in the background so it can be returned
        // to the foreground
        setpgid(0, 0);
        if (!piped) {
            // Launch a regular non-piped process
            launch_job(program, params, false);
        } else {
            // Launch a piped process and verify that it ended up running properly and
            // exit
            int status = launch_pipe_job(split_params, -1, false);

            for (int i = 0; i < param_count; i++) {
                if (params[i] != NULL) {
                    free(params[i]);
                }
            }
            free(params);

            for (int i = 0; split_params[i] != NULL; i++) {
                for (int j = 0; split_params[i][j] != NULL; j++) {
                    free(split_params[i][j]);
                }
                free(split_params[i]);
            }
            free(split_params);

            if (status != 0) {
                printf("COSC-6325/701Shell: Result errored with status %d\n", status);
            }
            exit(status);
        }
    } else {
        // Parent process
        // Set pgid of child process so it can be moved to the foreground from the
        // background later
        setpgid(pid, pid);
        if (!background) {
            // If it isn't a background task, add it to the jobs list then switch it
            // to the foreground
            add_job_to_list(params, pid, false, true);
            char *temp[2] = {"fg", NULL};
            switch_foreground(temp, false);
        } else {
            // If it is a background task, simply add it to the jobs list
            add_job_to_list(params, pid, true, false);
        }
    }

    // Free the parameter memory
    for (int i = 0; i < param_count; i++) {
        if (params[i] != NULL) {
            free(params[i]);
        }
    }
    free(params);

    // Free split parameters memory
    if (split_params != NULL) {
        for (int i = 0; split_params[i] != NULL; i++) {
            for (int j = 0; split_params[i][j] != NULL; j++) {
                free(split_params[i][j]);
            }
            free(split_params[i]);
        }
        free(split_params);
    }
    return 0;
}

// Function for launching a job which contains boolean expressions
int launch_job_op(char **command, int command_count, bool background) {
    // Creating the supported operations array
    char *ops[3] = {"&&\0", "||\0", NULL};
    // Splitting the command on the supported operations array
    char ***split_on_and_or = split_double_char_on_string_list(command, ops);

    // Initializing the list of operators in the operations "pipeline"
    int op_count = 2;
    char **param_ops = (char **) malloc(sizeof(char *) * op_count);
    memset(param_ops, 0, op_count);
    param_ops[0] = (char *) malloc(sizeof(char) * 3);
    memset(param_ops[0], 0, 3);
    param_ops[1] = NULL;

    for (int i = 0; i < command_count; i++) {
        // If it finds the OR operator
        if (strcmp(command[i], "||") == 0) {
            if (op_count == 2) {
                // First entry is OR, just need to add it to the first entry and
                // increment op_count
                strcpy(param_ops[0], "||\0");
                op_count++;
            } else {
                // Not the first entry in the param_ops list, need to reallocate memory
                // of the array, allocate space for the operator plus termination byte,
                // then NULL terminate the array again
                param_ops = (char **) realloc(param_ops, sizeof(char *) * (op_count));
                param_ops[op_count - 2] = (char *) malloc(sizeof(char) * 3);
                memset(param_ops[op_count - 2], 0, 3);
                strcpy(param_ops[op_count - 2], "||\0");
                param_ops[op_count - 1] = NULL;
                op_count++;
            }
        } else if (strcmp(command[i], "&&") == 0) {
            // If it finds the AND operator
            if (op_count == 2) {
                // First entry is AND, just need to add it to the first entry and
                // increment op_count
                strcpy(param_ops[0], "&&\0");
                op_count++;
            } else {
                // Not the first entry in the param_ops list, need to reallocate memory
                // of the array, allocate space for the operator plus termination byte,
                // then NULL terminate the array again
                param_ops = (char **) realloc(param_ops, sizeof(char *) * (op_count));
                param_ops[op_count - 2] = (char *) malloc(sizeof(char) * 3);
                memset(param_ops[op_count - 2], 0, 3);
                strcpy(param_ops[op_count - 2], "&&\0");
                param_ops[op_count - 1] = NULL;
                op_count++;
            }
        }
    }

    // Verify that the list of split entries is good
    for (int i = 0; split_on_and_or[i] != NULL; i++) {
        if (split_on_and_or[i][0] == NULL) {
            printf("COSC-6325/701Shell: Invalid operator: Must be an expression on "
                   "either side of '&&' or '||'\n");
            return 1;
        }
    }

    // Fork the process
    pid_t pid;
    pid = fork();

    if (pid < 0) {
        // Error forking, notify the user
        printf("COSC-6325/701Shell: Could not fork process\n");
    } else if (pid == 0) {
        // Child process, set pgid so it can be moved from bg to fg
        setpgid(0, 0);
        // Launch boolean operations jobs and exit with the status
        int status = launch_op_job(split_on_and_or, param_ops, -1, background);

        for (int i = 0; i < op_count - 1; i++) {
            if (param_ops[i] != NULL) {
                free(param_ops[i]);
            }
        }
        free(param_ops);

        for (int i = 0; split_on_and_or[i] != NULL; i++) {
            for (int j = 0; split_on_and_or[i][j] != NULL; j++) {
                free(split_on_and_or[i][j]);
            }
            free(split_on_and_or[i]);
        }
        free(split_on_and_or);

        exit(status);
    } else {
        // Parent process, set pgid of child process for moving from bg to fg later
        setpgid(pid, pid);
        if (!background) {
            // Not a background process, can add job to list and immediately switch to
            // fg
            add_job_to_list(command, pid, false, true);
            char *temp[2] = {"fg", NULL};
            switch_foreground(temp, false);
        } else {
            // Background process, add to job list and leave this function
            add_job_to_list(command, pid, true, false);
        }
    }

    // Freeing the list of operators
    for (int i = 0; i < op_count - 1; i++) {
        if (param_ops[i] != NULL) {
            free(param_ops[i]);
        }
    }
    free(param_ops);

    // Free split parameters memory
    for (int i = 0; split_on_and_or[i] != NULL; i++) {
        for (int j = 0; split_on_and_or[i][j] != NULL; j++) {
            free(split_on_and_or[i][j]);
        }
        free(split_on_and_or[i]);
    }
    free(split_on_and_or);

    return 0;
}

// The user input indicates a desire to run something under execvp()
void handle_external() {
    // Booleans which indicate whether this is a piped job, uses boolean
    // expressions, and whether it is run in the bg
    bool piped = false;
    bool uses_and_or_op = false;
    bool background = false;

    // Checks all tokens to see if it is piped or uses boolean expressions
    for (int i = 0; i < token_count; i++) {
        if (strcmp(tokenized_input[i], "|") == 0) {
            piped = true;
        } else if (strcmp(tokenized_input[i], "||") == 0 ||
                   strcmp(tokenized_input[i], "&&") == 0) {
            uses_and_or_op = true;
        }
    }

    if (piped) {
        // Piped and we need to verify that '&' is only located at the very end of
        // the total input, not at the end of a pipe in a pipeline or a pipeline
        // itself if it is also a boolean expression
        for (int i = 0; i < token_count; i++) {
            if (strcmp(tokenized_input[i], "&") == 0 && i != token_count - 1) {
                printf("COSC-6325/701Shell: This shell does not support background "
                       "processes within a pipe. Add '&' to the end to background the "
                       "whole pipeline\n");
                return;
            }
        }
    }

    // Verifies whether it is a background job and if so sets the last entry to
    // NULL and reduces token count so it will not continue with it attached
    if (strcmp(tokenized_input[token_count - 1], "&") == 0) {
        background = true;
        tokenized_input[token_count - 1] = NULL;
        token_count--;
    }

    // Verifies that all boolean expressions and pipes in the input are good
    if (strcmp(tokenized_input[token_count - 1], "&&") == 0 ||
        strcmp(tokenized_input[token_count - 1], "||") == 0) {
        printf("COSC-6325/701Shell: Invalid operator: '&&' or '||' found at end of "
               "expression, must be complete\n");
        return;
    } else if (strcmp(tokenized_input[token_count - 1], "|") == 0) {
        printf("COSC-6325/701Shell: Invalid operator: '|' found at end of "
               "expression, must be complete\n");
        return;
    }

    if (!uses_and_or_op) {
        // This input is not a boolean expression so launch without
        launch_job_no_op(tokenized_input, token_count, piped, background);
    } else {
        // This input is a boolean expression, so launch with boolean expression
        // launcher
        launch_job_op(tokenized_input, token_count, background);
    }
}

// Funcion which runs the user input capture
void *input_thread_handler() {
    if (getcwd(current_directory, sizeof(current_directory)) == NULL) {
        printf("COSC-6325/701Shell: Error getting current working directory");
        return NULL;
    }

    // Initializes the previous command string to nothing
    char prev_command[INPUT_BUFFER_SIZE] = {0};

    // Runs infinitely until ended with SIGINT, exit command, or SIGKILL
    while (true) {
        // String for capturing user input
        char user_input[INPUT_BUFFER_SIZE] = {0};
        // Sets token count to 0 for new input
        token_count = 0;
        // Sets the last directory to nothing
        memset(last_dir, 0, PATH_MAX);
        // Sets the CWD to nothing
        char path_copy[PATH_MAX] = {0};
        // Sets the CWD to global current dir
        strcpy(path_copy, current_directory);

        // Gets the last directory in the path for displaying, needs special help
        // for the '/' directory
        char *token;
        if (strcmp(path_copy, "/") != 0) {
            token = strtok(path_copy, "/");
            while (token != NULL) {
                strcpy(last_dir, token);
                token = strtok(NULL, "/");
            }
        } else {
            last_dir[0] = '/';
        }

        // Lock and print the shell unless already printed by a different function
        pthread_mutex_lock(&shell_lock);
        if (!shell_printed) {
            printf("COSC-6325/701Shell %s $ ", last_dir);
            fflush(stdout);
            shell_printed = true;
        } else {
            shell_printed = false;
        }
        pthread_mutex_unlock(&shell_lock);

        // Gets the user input
        fgets(user_input, INPUT_BUFFER_SIZE, stdin);
        fflush(stdin);

        // Marks the shell as having not been printed since we are waiting on it to
        // print again
        shell_printed = false;

        pthread_mutex_lock(&thread_lock);

        // Trim the user input (didn't know strtok_r already stripped whitespace
        // before writing the trim function :/)
        trimmed_input = trim(user_input);

        // If there is nothing but whitespace then report it and restart the loop
        if (trimmed_input == NULL) {
            printf("COSC-6325/701Shell: Could not find discernable input\n");
            pthread_mutex_unlock(&thread_lock);
            continue;
        }

        // Counts all of the tokens for looping over
        bool counted_this = false;
        for (int i = 0; i < (int) strlen(trimmed_input); i++) {
            if (!isspace(trimmed_input[i]) && !counted_this) {
                counted_this = true;
                token_count++;
            } else if (isspace(trimmed_input[i])) {
                counted_this = false;
            }
        }

        // Allocates memory to the String array holding user input tokens
        tokenized_input = (char **) malloc(sizeof(char *) * (token_count + 1));
        memset(tokenized_input, 0, token_count + 1);

        char *save_ptr = NULL;

        // Begin tokenizing
        token = strtok_r(trimmed_input, " \t", &save_ptr);

        // Loop through tokens
        int curr_index = 0;
        while (token != NULL) {
            // Allocate memory for each token
            tokenized_input[curr_index] =
                    (char *) malloc(sizeof(char) * ((int) strlen(token) + 1));
            memset(tokenized_input[curr_index], 0, (int) strlen(token) + 1);
            // Copy token into the allocated space
            strncpy(tokenized_input[curr_index], token, (int) strlen(token));
            // Termination byte at the end
            tokenized_input[curr_index][(int) strlen(token)] = '\0';

            // Get next token
            token = strtok_r(NULL, " \t", &save_ptr);
            curr_index++;
        }

        // NULL terminate token entries
        tokenized_input[token_count] = NULL;

        // Copy the first entry in the tokenized input to the prev_command variable
        strcpy(prev_command, tokenized_input[0]);

        // Free the trimmed input
        free(trimmed_input);
        trimmed_input = NULL;

        // Checking keywords
        if (strcmp(tokenized_input[0], "exit") == 0) {
            // "exit" exits the shell
            eflag = SIGINT;
        } else if (strcmp(tokenized_input[0], "cd") == 0) {
            // "cd" changes directory. Need to use "\ " to include spaces in path
            if (tokenized_input[1] == NULL) {
                printf("COSC-6325/701Shell: cd: Need path argument\n");
            } else {
                // Checking for '\' key char in the path argument to see about
                // concatenating spaces
                char change_path[PATH_MAX] = {'\0'};
                if (token_count > 2) {
                    for (int i = 1; i < token_count; i++) {
                        if (tokenized_input[i][(int) strlen(tokenized_input[i]) - 1] ==
                            '\\') {
                            tokenized_input[i][(int) strlen(tokenized_input[i]) - 1] = '\0';
                            strcat(change_path, tokenized_input[i]);
                            strcat(change_path, " ");
                        } else {
                            strcat(change_path, tokenized_input[i]);
                            break;
                        }
                    }
                } else {
                    strcat(change_path, tokenized_input[1]);
                }

                // Actually changes the directory
                int dir_check = chdir(change_path);
                // Verifies that it was changed
                if (dir_check != 0) {
                    printf("COSC-6325/701Shell: cd: Cannot find directory %s\n",
                           change_path);
                } else {
                    // Checks with errors getting the CWD
                    if (getcwd(current_directory, sizeof(current_directory)) == NULL) {
                        printf("COSC-6325/701Shell: cd: Error getting directory\n");
                    }
                }
            }
        } else if (strcmp(tokenized_input[0], "jobs") == 0) {
            // "jobs" will list currently active jobs
            list_jobs();
        } else if (strcmp(tokenized_input[0], "fg") == 0) {
            // "fg [job_id]" will change the foreground to the job indicated
            // "fg" will change the foreground to the last job started
            switch_foreground(tokenized_input, true);
        } else if (strcmp(tokenized_input[0], "bg") == 0) {
            // "bg [job_id]" will continue the job indicated in the background
            // "bg" will continue the last job started in the background
            switch_background(tokenized_input);
        } else {
            // If it is none of these key words then run the command using the
            // execvp() system call
            handle_external();
        }

        // Free the tokenized input
        for (int i = 0; i < token_count + 1; i++) {
            free(tokenized_input[i]);
        }
        free(tokenized_input);
        token_count = 0;

        pthread_mutex_unlock(&thread_lock);
    }
}

int main(int argc, char *argv[]) {

    // Initially getting the parent pid and pgid and setting the foreground pid to
    // the parent
    parent_pid = getpid();
    parent_pgid = getpgid(0);
    fg_pid = parent_pid;

    // May use arguments later as an expansion so wanted to include them here
    // without getting a warning. So adds the parent as a job which cannot be
    // switched to fg or bg from within the shell
    if (argc > 0) {
        add_job_to_list(argv, parent_pid, false, true);
    }

    // Setup signal handler and ignore SIGTTOU signal which causes issues with
    // child processes
    sig_init();
    signal(SIGTTOU, SIG_IGN);

    // Create the user input shell
    pthread_create(&input_thread, NULL, input_thread_handler, NULL);

    // Infinite loop which runs until SIGINT, exit command, or SIGKILL for
    // handling signals
    while (true) {
        // Check if a signal happened
        if (eflag != 0) {
            if (eflag == SIGINT) {
                // SIGINT thrown, exits the loop which then runs the
                // free_memory_kill_children() function
                pthread_mutex_lock(&thread_lock);
                if (fg_pid == parent_pid) {
                    // Verify that the current fg process is the parent before freeing
                    // memory and killing children
                    pthread_mutex_unlock(&thread_lock);
                    break;
                }
                pthread_mutex_unlock(&thread_lock);
            } else if (eflag == SIGCHLD) {
                // SIGCHLD thrown, got a signal from a child process (stopping or
                // exiting)
                pthread_mutex_lock(&thread_lock);
                // Reset flag
                eflag = 0;
                // Checking which process could have thrown the signal
                for (int i = 0; i < job_count; i++) {
                    if (running_jobs[i].pid == parent_pid) {
                        continue;
                    }
                    int status;
                    // Check children status'
                    while (waitpid(running_jobs[i].pid, &status, WNOHANG) == -1) {
                        if (errno != EINTR) {
                            break;
                        }
                    }
                    // If the child exited, remove the job
                    if (WIFEXITED(status)) {
                        if (!running_jobs[i].foreground) {
                            shell_printed = !shell_printed;
                        }
                        remove_job(running_jobs[i].pid, WEXITSTATUS(status), true, true);
                    }
                }
                pthread_mutex_unlock(&thread_lock);
            }
        }
    }

    // Free all of the allocated memory and kill the children
    free_memory_kill_children();
    return 0;
}
