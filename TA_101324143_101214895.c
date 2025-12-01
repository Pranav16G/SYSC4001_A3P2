#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <semaphore.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <ctype.h>

#define NUM_QUESTIONS 5
#define RUBRIC_LINE_LEN 32
#define SHM_NAME "/ta_marker_shm"
#define MAX_EXAMS 100

typedef struct {
    char rubric[NUM_QUESTIONS][RUBRIC_LINE_LEN];
    char current_student_num[5];    
    int question_state[NUM_QUESTIONS];  
    int exam_index;
    int total_exams;
    int all_done;
    
    //semaphores
    sem_t rubric_rw_lock;
    sem_t rubric_readers_mutex;
    int rubric_read_count;
    sem_t exam_mutex;
    sem_t print_mutex;
} shared_t;

void random_delay(int min_us, int max_us) {
    int delay = min_us + rand() % (max_us - min_us + 1);
    usleep(delay * 1000);
}

void save_rubric_to_file(shared_t *sh) {
    FILE *f = fopen("rubric.txt", "w");
    if (f) {
        for (int i = 0; i < NUM_QUESTIONS; i++) {
            fprintf(f, "%d, %s\n", i+1, sh->rubric[i]);
        }
        fclose(f);
    }
}

int load_student_num(const char *filename, char *student_str) {
    char fullpath[256];
    snprintf(fullpath, sizeof(fullpath), "exams/%s", filename);
    FILE *f = fopen(fullpath, "r");
    if (!f) {
        printf("ERROR: Cannot open %s\n", fullpath);
        strcpy(student_str, "0000");
        return -1;
    }
    if (fgets(student_str, 5, f)) {  // Exactly 4 digits + null
        student_str[strcspn(student_str, "\n")] = 0;
        fclose(f);
        return 0;
    }
    fclose(f);
    strcpy(student_str, "0000");
    return -1;
}

void load_exam(const char *filename, shared_t *sh) {
    load_student_num(filename, sh->current_student_num);
}

void load_rubric(shared_t *sh) {
    FILE *f = fopen("rubric.txt", "r");
    if (f) {
        for (int i = 0; i < NUM_QUESTIONS; i++) {
            fgets(sh->rubric[i], RUBRIC_LINE_LEN, f);
            sh->rubric[i][strcspn(sh->rubric[i], "\n")] = 0;
        }
        fclose(f);
    }
}

char** scan_exams_dir(int *total) {
    DIR *dir = opendir("./exams");
    if (!dir) {
        perror("exams dir");
        exit(1);
    }
    
    char **files = malloc(MAX_EXAMS * sizeof(char*));
    char *names[MAX_EXAMS];
    int count = 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < MAX_EXAMS) {
        if (entry->d_name[0] != '.') {
            names[count] = strdup(entry->d_name);
            files[count] = names[count];
            count++;
        }
    }
    closedir(dir);
    *total = count;
    return files;
}

void rubric_reader_enter(shared_t *sh) {
    sem_wait(&sh->rubric_readers_mutex);
    sh->rubric_read_count++;
    if (sh->rubric_read_count == 1)
        sem_wait(&sh->rubric_rw_lock);
    sem_post(&sh->rubric_readers_mutex);
}

void rubric_reader_exit(shared_t *sh) {
    sem_wait(&sh->rubric_readers_mutex);
    sh->rubric_read_count--;
    if (sh->rubric_read_count == 0)
        sem_post(&sh->rubric_rw_lock);
    sem_post(&sh->rubric_readers_mutex);
}

void rubric_writer_enter(shared_t *sh) {
    sem_wait(&sh->rubric_rw_lock);
}

void rubric_writer_exit(shared_t *sh) {
    sem_post(&sh->rubric_rw_lock);
}

void safe_print(shared_t *sh, const char *fmt, ...) {
    sem_wait(&sh->print_mutex);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    sem_post(&sh->print_mutex);
}

void ta_loop(shared_t *sh, int id, char **exam_files, int total_exams, int use_semaphores) {
    srand(getpid() + time(NULL));
    
    while (1) {
        sem_wait(&sh->exam_mutex);
        if (sh->all_done) {
            sem_post(&sh->exam_mutex);
            break;
        }
        char student[5];
        strncpy(student, sh->current_student_num, 5);
        sem_post(&sh->exam_mutex);
        
        if (strcmp(student, "9999") == 0) break;
        
        // Review rubric
        safe_print(sh, "[TA %d] Reviewing rubric for student %s\n", id, student);
        
        if (use_semaphores) rubric_reader_enter(sh);
        for (int q = 0; q < NUM_QUESTIONS; q++) {
            random_delay(500, 1000);
            char *line = sh->rubric[q];
            char *comma = strchr(line, ',');
            if (comma) {
                char *rub_char = comma + 2;
                if (*rub_char && isalpha(*rub_char)) {
                    if (rand() % 3 == 0) {
                        if (use_semaphores) {
                            rubric_reader_exit(sh);
                            rubric_writer_enter(sh);
                        }
                        (*rub_char)++;
                        safe_print(sh, "[TA %d] Corrected rubric Q%d to '%c'\n", id, q+1, *rub_char);
                        save_rubric_to_file(sh);
                        if (use_semaphores) rubric_writer_exit(sh);
                        if (use_semaphores) rubric_reader_enter(sh);
                    }
                }
            }
        }
        if (use_semaphores) rubric_reader_exit(sh);
        
        while (1) 
        {
            int q_to_mark = -1;
            sem_wait(&sh->exam_mutex);
            for (int q = 0; q < NUM_QUESTIONS; q++) {
                if (sh->question_state[q] == 0) {
                    sh->question_state[q] = 1;
                    q_to_mark = q;
                    break;
                }
            }
            strncpy(student, sh->current_student_num, 5);
            sem_post(&sh->exam_mutex);
            
            if (q_to_mark == -1) break;
            
            safe_print(sh, "[TA %d] Marking Q%d for student %s\n", id, q_to_mark+1, student);
            random_delay(1000, 2000);
            
            sem_wait(&sh->exam_mutex);
            sh->question_state[q_to_mark] = 2;
            sem_post(&sh->exam_mutex);
            
            safe_print(sh, "[TA %d] Finished Q%d for student %s\n", id, q_to_mark+1, student);
        }
        
        // Check if exam complete, load next
        sem_wait(&sh->exam_mutex);
        int all_done_local = 1;
        for (int q = 0; q < NUM_QUESTIONS; q++) {
            if (sh->question_state[q] != 2) {
                all_done_local = 0;
                break;
            }
        }
        if (all_done_local) {
            int next_idx = sh->exam_index + 1;
            if (next_idx >= sh->total_exams) {
                strcpy(sh->current_student_num, "9999");
                sh->all_done = 1;
                safe_print(sh, "All exams done\n", id);
            } 
            else 
            {
                sh->exam_index = next_idx;
                load_exam(exam_files[next_idx], sh);
                if (strcmp(sh->current_student_num, "0000") != 0) {
                    for (int q = 0; q < NUM_QUESTIONS; q++)
                        sh->question_state[q] = 0;
                    safe_print(sh, "[TA %d] Loaded exam %d (student %s)\n", 
                              id, sh->exam_index, sh->current_student_num);
                } 
                else 
                {
                    sh->all_done = 1;
                    safe_print(sh, "[TA %d] Bad exam file, stopping\n", id);
                }
            }
        }
        sem_post(&sh->exam_mutex);
    }
    safe_print(sh, "[TA %d] Exiting\n", id);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <num_tas> <0|1>  (0=no semaphores/partA, 1=semaphores/partB)\n", argv[0]);
        return 1;
    }
    
    int num_tas = atoi(argv[1]);
    int use_semaphores = atoi(argv[2]);
    if (num_tas < 2) {
        printf("Need >= 2 TAs\n");
        return 1;
    }
    
    //Scan exams
    int total_exams;
    char **exam_files = scan_exams_dir(&total_exams);
    printf("Found %d exam files\n", total_exams);
    
    //shared memory
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }
    ftruncate(shm_fd, sizeof(shared_t));
    shared_t *sh = mmap(0, sizeof(shared_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    
    //initialize
    memset(sh, 0, sizeof(shared_t));
    sh->total_exams = total_exams;
    sh->exam_index = 0;
    sh->all_done = 0;
    
    load_rubric(sh);
    load_exam(exam_files[0], sh);
    printf("Starting with student %s\n", sh->current_student_num);
    
    if (use_semaphores) {
        sem_init(&sh->rubric_rw_lock, 1, 1);
        sem_init(&sh->rubric_readers_mutex, 1, 1);
        sh->rubric_read_count = 0;
        sem_init(&sh->exam_mutex, 1, 1);
        sem_init(&sh->print_mutex, 1, 1);
    }
    
    //fork to run several TA process
    pid_t pids[num_tas];
    for (int i = 0; i < num_tas; i++) 
    {
        pids[i] = fork();
        if (pids[i] == 0) 
        {
            ta_loop(sh, i, exam_files, total_exams, use_semaphores);
            exit(0);
        }
    }
    
    //parent process  
    for (int i = 0; i < num_tas; i++) 
    {
        waitpid(pids[i], NULL, 0);
    }
    
    //destroy
    if (use_semaphores) 
    {
        sem_destroy(&sh->rubric_rw_lock);
        sem_destroy(&sh->rubric_readers_mutex);
        sem_destroy(&sh->exam_mutex);
        sem_destroy(&sh->print_mutex);
    }
    munmap(sh, sizeof(shared_t));
    close(shm_fd);
    shm_unlink(SHM_NAME);
    
    for (int i = 0; i < total_exams; i++)
        free(exam_files[i]);
    free(exam_files);
    
    return 0;
}
