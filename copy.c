#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <stdbool.h>
#include <limits.h>
#include <stdint.h>

#define QUEUE_EMPTY INT_MIN
#define TEN_MILLIS_IN_NANOS 10000000

struct timespec ts;

/* thread management */
pthread_mutex_t lock;
pthread_rwlock_t rwlock;

/* file management */
FILE *src_file;
FILE *cpy_file;
FILE *log_file;

/* circular buffer -> start */
typedef struct
{
    char data;
    off_t offset;
} BUFFER_ITEM;

typedef struct
{
    BUFFER_ITEM *buffer;
    int head;
    int tail;
    int maxlen;
} CIRCULAR_BUFFER;

CIRCULAR_BUFFER circular_buffer;

void cbufferInit(CIRCULAR_BUFFER *cb, int size)
{
    cb->maxlen = size + 1;
    cb->buffer = malloc(sizeof(BUFFER_ITEM) * cb->maxlen);
    cb->head = 0;
    cb->tail = 0;
}

int cbufferPush(CIRCULAR_BUFFER *cb, BUFFER_ITEM value)
{
    int rc = pthread_mutex_lock(&lock);
    if (rc)
    { /* an error has occurred */
        pthread_exit(NULL);
    }

    /* writing advances head */
    int next;
    next = cb->head + 1;
    if (next >= cb->maxlen - 1)
        next = 0; // next is where the head will point after write

    // if (next == cb->tail)
    //     return -1; // circular buffer is full

    cb->buffer[cb->head] = value; // load data and move
    cb->head = next;              // head to the next data offset

    pthread_mutex_unlock(&lock);
    return next;
}

int cbufferPop(CIRCULAR_BUFFER *cb, BUFFER_ITEM *value)
{
    int rc = pthread_mutex_lock(&lock);
    if (rc)
    { /* an error has occurred */
        pthread_exit(NULL);
    }

    /* reading advances tail */
    int next;
    // if (cb->head == cb->tail)
    //     return -1; // no data

    next = cb->tail + 1; // next is where to read
    if (next >= cb->maxlen - 1)
        next = 0;

    *value = cb->buffer[cb->tail]; // read data and move
    cb->tail = next;               // tail to next offset

    pthread_mutex_unlock(&lock);
    return next;
}
/* circular buffer <- end */

void randomSleepNs()
{
    ts.tv_sec = 0;
    ts.tv_nsec = rand() % (TEN_MILLIS_IN_NANOS + 1);
    nanosleep(&ts, NULL);
    // printf("%lld.%.9ld nanosleep: %s thread %d\n", (long long)ts.tv_sec, ts.tv_nsec, ttype, index);
}

char *formatString(char *op, char *tt, int num, long int off, uint8_t b, int i)
{
    /*
    operation, thread_type, thread_number, offset, actual_byte, index if produce/consume but -1 for read/write byte
    */
    int len = 55;
    char *buffer = malloc(len + 1);

    snprintf(buffer, len, "%s %s%d O%ld B%u I%d\n", op, tt, num, off, b, i);
    return buffer;
}

/* thread routines */
void *inRoutine(void *arg)
{
    randomSleepNs();

    int t_num = *(int *)arg;
    static char ch;
    static long offst;

    fseek(src_file, 0, SEEK_SET);

    while (1)
    {

        pthread_rwlock_rdlock(&rwlock);
        /* read from src file */
        ch = fgetc(src_file);

        uint8_t byte = ch;
        offst = ftell(src_file);
        // printf("> inRoutine: buffer.data: %c, buffer.offset: %ld\n", ch, offst);

        /* write to log file */
        fseek(log_file, 0, SEEK_CUR);
        fputs(
            formatString("read_byte", "PT", t_num, offst, byte, -1),
            log_file);

        if (ch == EOF)
        {
            pthread_rwlock_unlock(&rwlock);
            pthread_exit(NULL);
            break;
        }

        /* create buffer data */
        BUFFER_ITEM item = {
            .data = ch,
            .offset = offst,
        };

        /* write data to buffer */
        int index = cbufferPush(&circular_buffer, item);

        /* write to log file */
        fseek(log_file, 0, SEEK_CUR);
        fputs(
            formatString("produce", "PT", t_num, offst, byte, index),
            log_file);

        randomSleepNs();
        pthread_rwlock_unlock(&rwlock);
    }

    free(arg);
    return NULL;
}

void *outRoutine(void *arg)
{
    randomSleepNs();

    int t_num = *(int *)arg;
    static char ch;
    static long offst;

    /* get source file last index */
    fseek(src_file, 0L, SEEK_END);
    long int last_index = ftell(src_file);
    rewind(src_file);

    static BUFFER_ITEM buffer_item;

    while (1)
    {
        /* read from buffer */
        int index = cbufferPop(&circular_buffer, &buffer_item);

        ch = buffer_item.data;
        uint8_t byte = ch;
        offst = buffer_item.offset;
        // printf("< outRoutine: buffer.data: %c, buffer.offset: %ld, index: %d\n", ch, offst, index);

        if (offst >= last_index)
        {
            // printf("thread%d exits\n", t_num);
            pthread_exit(NULL);
            break;
        }

        pthread_rwlock_rdlock(&rwlock);
        /* write to log file */
        fseek(log_file, 0, SEEK_CUR);
        fputs(
            formatString("consume", "CT", t_num, offst, byte, index),
            log_file);

        /* write to copy file */
        fseek(cpy_file, offst - 1, SEEK_SET);
        fputc(ch, cpy_file);

        /* write to log file */
        fseek(log_file, 0, SEEK_CUR);
        fputs(
            formatString("write_byte", "CT", t_num, offst, byte, -1),
            log_file);

        randomSleepNs();
        pthread_rwlock_unlock(&rwlock);
    }

    free(arg);
    return NULL;
}

int main(int argc, char *argv[])
{
    srand(time(0));

    /* process command line arguments */
    if (argc != 7)
    {
        (argc < 7) ? printf("invalid number of arguments supplied.") : printf("too many arguments supplied!");
        printf("\nexpected command: ./cpy <nIN> <nOUT> <file> <copy> <bufSize> <Log>\n");
        return -1;
    }

    /* initialize variables with command line arguments */
    int n_in = atoi(argv[1]);
    int n_out = atoi(argv[2]);
    char *source_pathname = argv[3];
    char *copy_filename = argv[4];
    int buffer_size = atoi(argv[5]);
    char *log_filename = argv[6];

    /* open files and initialize new circular buffer */
    src_file = fopen(source_pathname, "r+");
    cpy_file = fopen(copy_filename, "w+");
    log_file = fopen(log_filename, "w+");
    cbufferInit(&circular_buffer, buffer_size);

    pthread_rwlock_init(&rwlock, NULL);

    if (pthread_mutex_init(&lock, NULL) != 0)
    {
        printf("\n mutex init has failed\n");
        return -1;
    }

    int i;

    /* create IN threads */
    pthread_t IN[n_in];
    for (i = 0; i < n_in; i++)
    {
        int *a = malloc(sizeof(int));
        *a = i;
        pthread_create(&IN[i], NULL, &inRoutine, a);
    }

    /* create OUT threads */
    pthread_t OUT[n_out];
    for (i = 0; i < n_out; i++)
    {
        int *a = malloc(sizeof(int));
        *a = i;
        pthread_create(&OUT[i], NULL, &outRoutine, a);
    }

    /* join threads with main thread */
    for (i = 0; i < n_in; i++)
        pthread_join(IN[i], NULL);

    for (i = 0; i < n_out; i++)
        pthread_join(OUT[i], NULL);

    /* close files */
    fclose(src_file);
    fclose(cpy_file);
    fclose(log_file);

    pthread_mutex_destroy(&lock);
    pthread_rwlock_destroy(&rwlock);

    return 0;
}