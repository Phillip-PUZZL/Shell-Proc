#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

// Structure containing the file name and the value to compare to for a process
// monitor
struct procStruct {
    char *file_name;
    char *compare_value;
};

// Structure containing the necessary information for continuous operation
struct continuousData {
    // Total number of readings to take
    int total_readings;
    // CPU core numbers, all time measurements are per core
    int *cpu_number;
    // Total amount of time in user mode
    long int *user_time;
    // Total amount of time in system mode
    long int *system_time;
    // Total amount of time in idle mode
    long int *idle_time;
    // Total amount of total CPU time
    long int *total_cpu_time;
    // Total amount of memory configured on machine
    long int total_memory;
    // Total amount of free memory available
    long int free_memory;
    // Drive numbers associated with a drive sda[0, 1, 2, 5], all drive
    // measurements are per drive
    int *drive_number;
    // Previous sectors read
    long int *prev_sectors_read;
    // Sectors read per second
    float *sectors_read_per_second;
    // Previous sectors written
    long int *prev_sectors_written;
    // Sectors written per second
    float *sectors_written_per_second;
    // Previous number of context switches
    long int prev_context_switches;
    // Context switches per second
    float context_switches_per_second;
    // Previous number of process creations
    long int prev_process_creations;
    // Process creatinos per second
    float process_creations_per_second;
};

// Signal handler information
struct sigaction sigact;
volatile sig_atomic_t eflag = 0;
// thread for printing the data and reading the data
pthread_t print_thread, read_thread;
// Lock both threads from accessing / changing data at same time
pthread_mutex_t lock;
// Structure for all continuous data
struct continuousData cont_data;
// Int for the print rate
int print_rate;
// Int for the read rate
int read_rate;
// Total number of drives on the machine
int drive_count;
// Total number of CPU cores on the machine
int cpu_count;

static void sig_handler(int);

// Allocate the memory for the cont_data struct and zero it out
void init_memory() {
    cont_data.drive_number = (int *) malloc(sizeof(int) * drive_count);
    cont_data.sectors_read_per_second =
            (float *) malloc(sizeof(float) * drive_count);
    cont_data.sectors_written_per_second =
            (float *) malloc(sizeof(float) * drive_count);
    cont_data.prev_sectors_read =
            (long int *) malloc(sizeof(long int) * drive_count);
    cont_data.prev_sectors_written =
            (long int *) malloc(sizeof(long int) * drive_count);
    cont_data.cpu_number = (int *) malloc(sizeof(int) * cpu_count);
    cont_data.user_time = (long int *) malloc(sizeof(long int) * cpu_count);
    cont_data.system_time = (long int *) malloc(sizeof(long int) * cpu_count);
    cont_data.idle_time = (long int *) malloc(sizeof(long int) * cpu_count);
    cont_data.total_cpu_time = (long int *) malloc(sizeof(long int) * cpu_count);

    memset(cont_data.drive_number, 0, sizeof(int) * drive_count);
    memset(cont_data.sectors_read_per_second, 0, sizeof(float) * drive_count);
    memset(cont_data.sectors_written_per_second, 0,
           sizeof(float) * drive_count);
    memset(cont_data.prev_sectors_read, 0, sizeof(long int) * drive_count);
    memset(cont_data.prev_sectors_written, 0, sizeof(long int) * drive_count);
    memset(cont_data.cpu_number, 0, sizeof(int) * cpu_count);
    memset(cont_data.user_time, 0, sizeof(long int) * cpu_count);
    memset(cont_data.system_time, 0, sizeof(long int) * cpu_count);
    memset(cont_data.idle_time, 0, sizeof(long int) * cpu_count);
    memset(cont_data.total_cpu_time, 0, sizeof(long int) * cpu_count);
}

// Free the cont_data memory
void free_memory() {
    free(cont_data.drive_number);
    free(cont_data.sectors_read_per_second);
    free(cont_data.sectors_written_per_second);
    free(cont_data.prev_sectors_read);
    free(cont_data.prev_sectors_written);
    free(cont_data.cpu_number);
    free(cont_data.user_time);
    free(cont_data.system_time);
    free(cont_data.idle_time);
    free(cont_data.total_cpu_time);
}

// Kill the read and print threads
void kill_threads() {
    pthread_cancel(read_thread);
    pthread_cancel(print_thread);
}

// Calculate a percantage
float get_percent(long int numerator, long int denominator) {
    return ((float) numerator / (float) denominator) * 100.0;
}

// Function to get the information from the /proc filesystem during the
// continuous version of operation
void get_proc_info_continuous() {
    // Necessary files for continuous version
    static const char *files[3] = {"stat", "meminfo", "diskstats"};
    // Reading 1024 bytes at a time from the files
    char read_buffer[1024];

    // Add a reading to the total
    cont_data.total_readings += 1;

    for (int i = 0; i < 3; i++) {
        // Get the file name for the /proc file and open it
        char file_name[100] = "/proc/";
        strcat(file_name, files[i]);
        FILE *info_file = fopen(file_name, "r");
        if (info_file == NULL) {
            fprintf(stderr, "Failed to open file %s\n", file_name);
            continue;
        }

        if (i == 0) {
            // From /proc/stat
            // variables for user, nice, system, idle, iowait, irq, and softirq cpu
            // time Also for context switches and process count
            long int user, nice, system, idle, iowait, irq, softirq, ctxt, processes,
                    index = 0;
            int cpu_number;

            while (fgets(read_buffer, sizeof(read_buffer), info_file)) {
                // Read the data from the file with these parameters and get cpu time
                // data
                if (sscanf(read_buffer, "cpu%*1[ ] %ld %ld %ld %ld %ld %ld %ld", &user,
                           &nice, &system, &idle, &iowait, &irq, &softirq) ||
                    sscanf(read_buffer, "cpu%d %ld %ld %ld %ld %ld %ld %ld",
                           &cpu_number, &user, &nice, &system, &idle, &iowait, &irq,
                           &softirq)) {

                    // If index is 0 then it is overall CPU stats, in this case cpu_number
                    // shouldn't exist so set it to -1
                    if (cont_data.total_cpu_time[index] == 0) {
                        if (index == 0) {
                            cont_data.cpu_number[index] = -1;
                        }
                        cont_data.cpu_number[index] = cpu_number;
                    }

                    // Set the data for this cpu core
                    cont_data.total_cpu_time[index] +=
                            (user + nice + system + idle + iowait + irq + softirq);
                    cont_data.user_time[index] += user;
                    cont_data.system_time[index] += system;
                    cont_data.idle_time[index] += idle;
                    index++;
                } else if (sscanf(read_buffer, "ctxt %ld", &ctxt)) {
                    // Context switch data on this line
                    if (cont_data.prev_context_switches == 0) {
                        // No prev ctxt data, set baseline
                        cont_data.prev_context_switches = ctxt;
                    } else {
                        // Prev ctxt data exists, measure difference and divide by the read
                        // rate. Set prev ctxt data to this one
                        cont_data.context_switches_per_second +=
                                ((float) (ctxt - cont_data.prev_context_switches) /
                                 (float) read_rate);
                        cont_data.prev_context_switches = ctxt;
                    }
                } else if (sscanf(read_buffer, "processes %ld", &processes)) {
                    // Process data on this line
                    if (cont_data.prev_process_creations == 0) {
                        // No previous process data, set baseline
                        cont_data.prev_process_creations = processes;
                    } else {
                        // Previous process data exists, measure difference and divide by
                        // read rate. Then set the new baseline
                        cont_data.process_creations_per_second +=
                                ((float) (processes - cont_data.prev_process_creations) /
                                 (float) read_rate);
                        cont_data.prev_process_creations = processes;
                    }
                }
            }
        } else if (i == 1) {
            // /proc/meminfo
            long int free_memory, total_memory;

            while (fgets(read_buffer, sizeof(read_buffer), info_file)) {
                if (strncmp(read_buffer, "MemTotal", (int) strlen("MemTotal")) == 0) {
                    // Memory total on this line, grab it
                    sscanf(read_buffer, "MemTotal: %ld %*s", &total_memory);
                } else if (strncmp(read_buffer, "MemAvailable", (int) strlen("MemAvailable")) ==
                           0) {
                    // Free memory on this line, grab it
                    sscanf(read_buffer, "MemAvailable: %ld %*s", &free_memory);
                }
            }

            if (cont_data.total_memory == 0) {
                // Total memory wasn't recorded before, do so not
                cont_data.total_memory = total_memory;
            }
            // Add total free memory together to divide by total readings at print
            // time
            cont_data.free_memory += free_memory;
        } else if (i == 2) {
            // /proc/diskstats
            long int sectors_read, sectors_written, index = 0;
            int drive_number;

            while (fgets(read_buffer, sizeof(read_buffer), info_file)) {
                if (sscanf(read_buffer,
                           "%*d %*d sda%*1[ ] %*ld %*ld %ld %*ld %*ld %*ld %ld",
                           &sectors_read, &sectors_written) ||
                    sscanf(read_buffer,
                           "%*d %*d sda%d %*ld %*ld %ld %*ld %*ld %*ld %ld",
                           &drive_number, &sectors_read, &sectors_written)) {
                    // Getting read and write data for sectors on a drive number
                    // Checking if ther is previous data, if not set the baselines
                    // sda with no drive number exists and is the total for a whole drive,
                    // doesn't have a number so assign it -1
                    if (cont_data.prev_sectors_read[index] == 0 ||
                        cont_data.prev_sectors_written[index] == 0) {
                        if (index == 0) {
                            cont_data.drive_number[index] = -1;
                        }
                        cont_data.drive_number[index] = drive_number;
                        cont_data.prev_sectors_read[index] = sectors_read;
                        cont_data.prev_sectors_written[index] = sectors_written;
                    }

                    // Measure difference between prev read / written and current then
                    // divide by the read rate then set the new baseline
                    cont_data.sectors_read_per_second[index] +=
                            ((float) (sectors_read -
                                      cont_data.prev_sectors_read[index]) /
                             (float) read_rate);
                    cont_data.sectors_written_per_second[index] +=
                            ((float) (sectors_written -
                                      cont_data.prev_sectors_written[index]) /
                             (float) read_rate);

                    cont_data.prev_sectors_read[index] = sectors_read;
                    cont_data.prev_sectors_written[index] = sectors_written;
                    index++;
                }
            }
        }
        fclose(info_file);
    }
}

// Reads data at the specified read rate
_Noreturn void *run_reader() {
    while (true) {
        sleep(read_rate);
        pthread_mutex_lock(&lock);
        get_proc_info_continuous();
        pthread_mutex_unlock(&lock);
    }
}

// Prints the data for continuous running
_Noreturn void *run_printer() {
    while (true) {
        // Wait for print time
        sleep(print_rate);
        pthread_mutex_lock(&lock);
        // If there are no total readings, likelihood is good that read / write
        // rates are same, need to get readings before printing if none already
        // exist
        while (cont_data.total_readings == 0) {
            pthread_mutex_unlock(&lock);
            usleep(500);
            pthread_mutex_lock(&lock);
        }

        // Printing the data in a nice pretty grid.
        // Not going to comment all of this printing, it is pretty straightforward
        printf("Total readings taken over %d seconds: %d\n",
               (cont_data.total_readings * read_rate), cont_data.total_readings);

        printf("+-----------------------CPU Stats----------------------+\n");
        printf("| %-10s | %11s | %11s | %11s |\n", "Core", "User Mode",
               "System Mode", "Idle Mode");
        printf("+------------+-------------+-------------+-------------+\n");
        for (int i = 0; i < cpu_count; i++) {
            if (i == 0) {
                printf(
                        "| %-10s | %10.2f%% | %10.2f%% | %10.2f%% |\n", "All cores",
                        get_percent(cont_data.user_time[i], cont_data.total_cpu_time[i]),
                        get_percent(cont_data.system_time[i], cont_data.total_cpu_time[i]),
                        get_percent(cont_data.idle_time[i], cont_data.total_cpu_time[i]));
            } else {
                printf(
                        "| CPU%-7d | %10.2f%% | %10.2f%% | %10.2f%% |\n",
                        cont_data.cpu_number[i],
                        get_percent(cont_data.user_time[i], cont_data.total_cpu_time[i]),
                        get_percent(cont_data.system_time[i], cont_data.total_cpu_time[i]),
                        get_percent(cont_data.idle_time[i], cont_data.total_cpu_time[i]));
            }
        }
        printf("+---------------------End CPU Stats--------------------+\n");
        printf("+----------------------Memory Stats--------------------+\n");
        printf("| %-10s | %11s | %11s | %11s |\n", "Memory", "Total kBs",
               "Free kBs", "% Free");
        printf("+------------+-------------+-------------+-------------+\n");
        printf("| %-10s | %11ld | %11ld | %10.2f%% |\n", "Data",
               cont_data.total_memory,
               cont_data.free_memory / cont_data.total_readings,
               get_percent(cont_data.free_memory / cont_data.total_readings,
                           cont_data.total_memory));
        printf("+--------------------End Memory Stats------------------+\n");
        printf("+-----------------------Disk Stats---------------------+\n");
        printf("| %-10s | %18s | %18s |\n", "Drive", "Reads in Sect/s",
               "Writes in Sect/s");
        printf("+------------+--------------------+--------------------+\n");
        for (int i = 0; i < drive_count; i++) {
            if (i == 0) {
                printf("| %-10s | %18.2f | %18.2f |\n", "SDA",
                       (cont_data.sectors_read_per_second[i] /
                        (float) cont_data.total_readings),
                       (cont_data.sectors_written_per_second[i] /
                        (float) cont_data.total_readings));
            } else {
                printf("| %s%-7d | %18.2f | %18.2f |\n", "SDA",
                       cont_data.drive_number[i],
                       (cont_data.sectors_read_per_second[i] /
                        (float) cont_data.total_readings),
                       (cont_data.sectors_written_per_second[i] /
                        (float) cont_data.total_readings));
            }
        }
        printf("+---------------------End Disk Stats-------------------+\n");
        printf("+----------------------Context Stats-------------------+\n");
        printf("| %-10s | %39s |\n", "Context", "Switches/s");
        printf("+------------+-----------------------------------------+\n");
        printf("| %-10s | %39.2f |\n", "Data",
               (cont_data.context_switches_per_second /
                (float) cont_data.total_readings));
        printf("+--------------------End Context Stats-----------------+\n");
        printf("+----------------------Process Stats-------------------+\n");
        printf("| %-10s | %39s |\n", "Process", "Creations/s");
        printf("+------------+-----------------------------------------+\n");
        printf("| %-10s | %39.2f |\n", "Data",
               (cont_data.process_creations_per_second /
                (float) cont_data.total_readings));
        printf("+--------------------End Process Stats-----------------+\n\n\n");

        // After printing, free all of the memory
        free_memory();

        // Setup the new struct
        struct continuousData temp = {0};
        cont_data = temp;

        // Allocate and zero out the struct
        init_memory();

        pthread_mutex_unlock(&lock);
    }
}

// Function to get the total number of drives (including the entry which
// includes ALL drive data)
bool set_drive_count() {
    char *file_name = "/proc/diskstats";
    FILE *info_file = fopen(file_name, "r");
    char read_buffer[1024];

    if (info_file == NULL) {
        fprintf(stderr, "Failed to open file %s\n", file_name);
        fclose(info_file);
        return false;
    }

    while (fgets(read_buffer, sizeof(read_buffer), info_file)) {
        long int test;
        if (sscanf(read_buffer, "%*d %*d sda %ld", &test) ||
            sscanf(read_buffer, "%*d %*d sda%*d %ld", &test)) {
            // If it finds either of the above, add one to the drive count
            drive_count += 1;
        }
    }

    fclose(info_file);
    return true;
}

// Function to get the total number of CPU cores (including the entry which
// includes ALL CPU data)
bool set_cpu_count() {
    char *file_name = "/proc/stat";
    FILE *info_file = fopen(file_name, "r");
    char read_buffer[1024];

    if (info_file == NULL) {
        fprintf(stderr, "Failed to open file %s\n", file_name);
        fclose(info_file);
        return false;
    }

    while (fgets(read_buffer, sizeof(read_buffer), info_file)) {
        long int test = 0;
        if (sscanf(read_buffer, "cpu %ld %*[^\n]", &test) ||
            sscanf(read_buffer, "cpu%*d %ld %*[^\n]", &test)) {
            // If it finds cpu entries then add one to cpu_count
            cpu_count += 1;
        }
    }

    fclose(info_file);
    return true;
}

// This is for the no terminal parameters on the program, collects one time data
bool get_proc_info_once(char **file_data) {
    static const struct procStruct files[4] = {{"cpuinfo", "model name"},
                                               {"version", NULL},
                                               {"meminfo", "MemTotal"},
                                               {"uptime",  NULL}};
    char read_buffer[1024];

    for (int i = 0; i < 4; i++) {
        // Attaches the proce file name to the /proc/ path and opens it
        char file_name[100] = "/proc/";
        strcat(file_name, files[i].file_name);
        FILE *info_file = fopen(file_name, "r");
        if (info_file == NULL) {
            fprintf(stderr, "Failed to open file %s\n", file_name);
            continue;
        }

        if (files[i].compare_value == NULL) {
            // No compare value, two options, either uptime data or
            if (strcmp(files[i].file_name, "uptime") != 0) {
                // Version data, just one line of kernel info
                if (fgets(read_buffer, sizeof(read_buffer), info_file) != NULL) {
                    file_data[i] = (char *) malloc(sizeof(char) * strlen(read_buffer));
                    strcpy(file_data[i], read_buffer);
                }
            } else {
                // Uptime data
                if (fgets(read_buffer, sizeof(read_buffer), info_file) != NULL) {
                    // Day in seconds
                    static const int DAY = 24 * 60 * 60;
                    // Hour in seconds
                    static const int HOUR = 60 * 60;
                    // Minute in seconds
                    static const int MINUTE = 60;
                    float total_uptime;

                    // Get uptime, first float in /proc/uptime file
                    sscanf(read_buffer, "%f", &total_uptime);
                    // Get days up and subtract that number of seconds from the total
                    int days_up = floor(total_uptime / (float) DAY);
                    total_uptime = total_uptime - (days_up * DAY);
                    // Get hours up and subtract that number of seconds from the total
                    int hours_up = floor(total_uptime / (float) HOUR);
                    total_uptime = total_uptime - (hours_up * HOUR);
                    // Get minutes up and subtract that number of seconds from the total
                    int minutes_up = floor(total_uptime / (float) MINUTE);
                    total_uptime = total_uptime - (minutes_up * MINUTE);
                    // Remainder is seconds up
                    int seconds_up = total_uptime;

                    // Put it into a nice string, allocate data to the function parameter,
                    // and copy it in
                    snprintf(read_buffer, sizeof(read_buffer),
                             "%d days %d hours %d minutes %d seconds", days_up, hours_up,
                             minutes_up, seconds_up);
                    file_data[i] = (char *) malloc(sizeof(char) * strlen(read_buffer));
                    strcpy(file_data[i], read_buffer);
                }
            }
        } else {
            // These have comparison values, easy to do in a loop
            while (fgets(read_buffer, sizeof(read_buffer), info_file) != NULL) {
                // Compare the current comparison value to the read buffer to see if it
                // matches
                if (strncmp(read_buffer, files[i].compare_value,
                            (int) strlen(files[i].compare_value)) == 0) {

                    // If it matches, get the index where the comparison value stops and
                    // the actual value starts
                    int index = (int) strlen(files[i].compare_value);
                    // Allocate memory to the change buffer
                    char *change_buffer =
                            (char *) malloc(sizeof(char) * strlen(read_buffer) + 1);
                    // Copy the read buffer into the change buffer
                    strcpy(change_buffer, read_buffer);

                    // Find the start value of the actual wanted data (trimming
                    // whitespace)
                    while (change_buffer[index] == ':' || change_buffer[index] == ' ' ||
                           change_buffer[index] == '\t') {
                        index++;
                    }

                    // Allocate data to the function parameter
                    file_data[i] =
                            (char *) malloc(sizeof(char) * (strlen(change_buffer) - index));
                    // Copy the data from the change buffer at the index until the end
                    strncpy(file_data[i], change_buffer + index,
                            strlen(change_buffer) - index);
                    // Free temporary memory
                    free(change_buffer);
                    break;
                }
            }
        }
        fclose(info_file);
    }
    return true;
}

// Return the signal handlers to no special action
void sig_cleanup() { sigemptyset(&sigact.sa_mask); }

// Interrupt signal capture
void sig_init() {
    sigact.sa_handler = sig_handler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, (struct sigaction *) NULL);
}

// Set eflag when captures SIGINT
static void sig_handler(int sig) {
    if (sig == SIGINT) {
        eflag = SIGINT;
    }
}

int main(int argc, char *argv[]) {

    // Check if there is one or three arguments (just the function vs the function
    // with two terminal parameters)
    if (argc != 1 && argc != 3) {
        // If there are 2 arguments and the second on is the -h flag, print usage
        // information
        if (argc == 2 && strcmp(argv[1], "-h") == 0) {
            printf("Usage information:\n\n");
            printf("proc_parse -> No command line arguments runs the application "
                   "once\nDisplays processor type, kernel version, memory "
                   "configuration, and uptime information\n\n");
            printf("proc_parse <read_rate> <printout_rate> -> <read_rate> and "
                   "<printout_rate> in integer seconds.\nDisplays averages for "
                   "processor mode, free memory, disk r/w rate, context switch rate, "
                   "process creation rate\n<read_rate> is the time between reads of "
                   "the /proc file system\n<prinout_rate> is the time between "
                   "printing statistics to the screen\nNOTE: <read_rate> <= "
                   "<printout_rate>\n");
            return 0;
        }
        // If there are too few or too many arguments, have the user check the -h
        // flag to see usage information
        printf("Improper command line usage. Use the -h flag to see usage "
               "instructions.\n");
        return 1;
    }

    // One time version, only has the binary argument
    if (argc == 1) {
        // Create and allocate the information String array
        char **system_info;
        system_info = (char **) malloc(sizeof(char *) * 4);

        // Get process information
        if (get_proc_info_once(system_info)) {
            // Process information retrieval went well, print results
            printf("Processor Type: %s\n", system_info[0]);
            printf("Kernel Version: %s\n", system_info[1]);
            printf("Memory Configured: %s\n", system_info[2]);
            printf("Uptime: %s\n", system_info[3]);
        }

        // Free the memory
        for (int i = 0; i < 4; i++) {
            free(system_info[i]);
        }
        free(system_info);
    } else if (argc == 3) {
        // Continuous version of the program
        // Verify that the read rate is actually a number and that it is greater
        // than 0
        for (int i = 0; i < (int) strlen(argv[1]); i++) {
            if (!isdigit(argv[1][i]) || (atoi(argv[1]) <= 0)) {
                printf("<read_rate> must be an integer value greater than 0\n");
                return 1;
            }
        }

        // Verify that the print rate is actually a number and that it is greater
        // than 0
        for (int i = 0; i < (int) strlen(argv[2]); i++) {
            if (!isdigit(argv[2][i]) || (atoi(argv[2]) <= 0)) {
                printf("<printout_rate> must be an integer value greater than 0\n");
                return 1;
            }
        }

        read_rate = atoi(argv[1]);
        print_rate = atoi(argv[2]);

        // Verify that the print rate is greater than or equal to the read rate
        if (read_rate > print_rate) {
            printf("<read_rate> must be less than or equal to <printout_rate>");
            return 1;
        }

        if (pthread_mutex_init(&lock, NULL) != 0) {
            printf("Could not lock threads\n");
            return 1;
        }

        // Try to get CPU and drive counts
        if (!(set_cpu_count() && set_drive_count())) {
            printf("Could not detect number of CPU cores or number of drives\n");
            return 1;
        }

        system("clear");
        printf("Starting /proc file system analyzer\nFirst results in %d "
               "seconds\n",
               print_rate);

        struct continuousData temp = {0};
        cont_data = temp;

        // Allocate and zero out cont_data
        init_memory();

        // Set the signal handler and block signals for the print and read threads
        sigset_t old_mask;
        sigemptyset(&old_mask);
        sigaddset(&old_mask, SIGINT);
        pthread_sigmask(SIG_BLOCK, &old_mask, NULL);

        pthread_create(&print_thread, NULL, run_printer, NULL);
        pthread_create(&read_thread, NULL, run_reader, NULL);

        // Initialize signal handler for main thread
        sig_init();

        // Unblock signals for main thread
        sigset_t new_mask;
        sigemptyset(&new_mask);
        sigaddset(&new_mask, SIGINT);
        pthread_sigmask(SIG_UNBLOCK, &new_mask, NULL);

        // Monitor for SIGINT
        while (true) {
            if (eflag != 0) {
                if (eflag == SIGINT) {
                    break;
                }
            }
        }

        // SIGINT occurs, free all memory and kill all threads then reset signal
        // handler and exit
        pthread_mutex_lock(&lock);
        free_memory();
        kill_threads();
        pthread_mutex_unlock(&lock);
        sig_cleanup();
        pthread_exit(NULL);
    }

    return 0;
}
