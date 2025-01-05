#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#define PIPE_READ 0
#define PIPE_WRITE 1

#define BUF_ALLOC 1024
static int fd;
static FILE *fp;
static pthread_mutex_t plot_mutex;

struct read_file_state
{
    uint32_t pos;
    uint32_t size;
    uint32_t alloc;
    char* buf;
    int32_t fd;
};

bool fd_skip(struct read_file_state* state, uint32_t num_byte)
{
    while (state->size - state->pos < num_byte)
    {
        if (state->size == state->alloc)
        {
            if (!state->buf)
                state->buf = (char*)malloc(BUF_ALLOC);
            else
                state->buf = (char*)realloc(state->buf, state->size + BUF_ALLOC);
            state->alloc += BUF_ALLOC;
        }
        //printf("alloc %d pos %d size %d\n", state->alloc, state->pos, state->size);
        int ret = read(state->fd, &state->buf[state->size], state->alloc - state->size);
        if (ret < 0) return false;
        state->size += ret;
    }
    state->pos += num_byte;
    return true;
}
bool fd_read(struct read_file_state* state, char** buf, uint32_t num_byte)
{
    bool status = fd_skip(state, num_byte);
    if (status)
        *buf = &state->buf[state->pos-num_byte];
    return status;
}
uint32_t swap_uint32( uint32_t val )
{
    val = ((val << 8) & 0xFF00FF00 ) | ((val >> 8) & 0xFF00FF ); 
    return (val << 16) | (val >> 16);
}
char* readpng(int fd, uint32_t *size)
{
    struct read_file_state state = {0};
    state.fd = fd;
    bool ret;
    char* temp;
    ret = fd_skip(&state, 8);
    uint32_t type = 0;
    while(type != 0x444e4549)
    {
        ret = fd_read(&state, &temp, 4);
        uint32_t len = swap_uint32(*(uint32_t*)temp);
        //printf("clen %u\n", len);
        ret = fd_read(&state, &temp, 4);
        type = *(uint32_t*)temp;
        //printf("type %x\n", type);
        ret = fd_skip(&state, len + 4);
        //printf("skip %d\n", ret);
    }
    *size = state.size;
    return state.buf;
    //printf("alloc %d pos %d size %d\n", state.alloc, state.pos, state.size);
}
void plot_start(unsigned int num_point)
{
    pthread_mutex_lock(&plot_mutex);
    unsigned int num_pix = num_point <= 3600 ? 1000 : num_point / 3600.0 * 1000;
    unsigned int incr = num_point < 3000 ? num_point / 10 : 300;
    fprintf(fp, "set terminal png size %u, 500\nset xtics %d\nset mxtics 5\n$DATA << EOD\n", num_pix, incr);
    fflush(fp);
}
void plot_write(int hour, int min, int sec, float val)
{
    fprintf(fp, "%d:%d:%d %f\n", hour, min, sec, val);
    fflush(fp);
}
void plot_end()
{
    fprintf(fp, "EOD\n");
    fflush(fp);
}
char* plot_get(unsigned int *size)
{
    fprintf(fp, "plot $DATA using 1:2 w lines\n");
    fflush(fp);
    char* ret = readpng(fd, size);
    return ret;
}
void free_plot(char* buf)
{
    free(buf);
    pthread_mutex_unlock(&plot_mutex);
}
void plot_init()
{
    int tfd[2];
    pthread_mutex_init(&plot_mutex, NULL);
    pipe(tfd);
    fp = popen("gnuplot", "w");
    close(tfd[1]);
    fprintf(fp, "set terminal png background rgb 'white'\nset timefmt '%%H:%%M:%%S'\nset xdata time\nset xtics rotate\nset yrange [:*<50]\nset output \"/dev/fd/%d\"\n", tfd[1]);
    fflush(fp);
    fd = tfd[0];
}

