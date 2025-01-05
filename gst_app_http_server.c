#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <iostream>
#include <boost/circular_buffer.hpp>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <opencv2/opencv.hpp>
using namespace std;
using namespace cv;
#include "type.h"

#define CBSIZE 2048

typedef struct cbuf {
    char buf[CBSIZE];
    int fd;
    unsigned int rpos, wpos;
} cbuf_t;
unsigned int copy_until(char* dest, const char *src, char c, int len);
int read_line(cbuf_t *cbuf, char *dst, unsigned int size)
{
    unsigned int i = 0;
    ssize_t n;
    bool found = false;
    while (i < size) {
        if (cbuf->rpos == cbuf->wpos) {
            size_t wpos = cbuf->wpos % CBSIZE;
            //if ((n = read(cbuf->fd, cbuf->buf + wpos, (CBSIZE - wpos))) < 0) {
            if((n = recv(cbuf->fd, cbuf->buf + wpos, (CBSIZE - wpos), 0)) < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            } else if (n == 0)
                return 0;
            cbuf->wpos += n;
        }
        dst[i++] = cbuf->buf[cbuf->rpos++ % CBSIZE];
        if (dst[i - 1] == '\n')
        {
            found = true;
            break;
        }
    }
    if(!found) {
         fprintf(stderr, "line too large: %d %d\n", i, size);
         return -1;
    }
    dst[i] = 0;
    return i;
}

extern struct cam_source *all_source;
extern int num_all_source;

void plot_init();
char* plot_get(unsigned int *size);
void plot_end();
void plot_write(int hour, int min, int sec, float val);
void plot_start(unsigned int num_point);
void free_plot(char* buf);

static unsigned int pngsize;
static char* png;

extern time_t today_sec;

char proc_cmd(char* str, unsigned char* source_index)
{
    char namestr[50];
    char namelen = copy_until(namestr, str, '/', 50);
    if (!namelen) return 0;
    bool found = false;
    int i;
    for (i = 0; i < num_all_source; i++)
        if (!strcmp(namestr, all_source[i].name))
        {
            found = true;
            break;
        }
    if (!found) return 0;
    *source_index = i;
    char* s = &str[namelen+1];
    int l = strlen(s);
    char temp[5];
    if (l == 6)
    {
        temp[0] = s[0];
        temp[1] = s[1];
        temp[2] = 0;
        int hour = atoi(temp);
        if (hour > 23)
            return false;
        temp[0] = s[2];
        temp[1] = s[3];
        temp[2] = 0;
        int min = atoi(temp);
        if (min > 59)
            return false;
        temp[0] = s[4];
        temp[1] = s[5];
        temp[2] = 0;
        int sec = atoi(temp);
        if (sec > 59)
            return 0;
        time_t timenow = time(NULL);
        struct tm *ptm = localtime(&timenow);
        ptm->tm_sec = sec;
        ptm->tm_min = min;
        ptm->tm_hour = hour;
        all_source[i].http_seek_time = mktime(ptm);
        bool out_of_range = false;
        guint64 time_diff = (all_source[i].http_seek_time - today_sec)*1000000000u;
        auto video_end = all_source[i].video_time_lookup.end();
        auto video_begin = all_source[i].video_time_lookup.begin();
        auto video_lookup_elm = upper_bound(video_begin, video_end, time_diff);
        if (video_end == video_lookup_elm)
            if (time_diff > all_source[i].video_time_lookup[video_end-video_begin]) 
                out_of_range = true;
        if (all_source[i].audioappsink)
        {
            auto audio_end = all_source[i].audio_time_lookup.end();
            auto audio_begin = all_source[i].audio_time_lookup.begin();
            auto audio_lookup_elm = upper_bound(audio_begin, audio_end, time_diff);
            if (audio_end == audio_lookup_elm)
                if (time_diff > all_source[i].audio_time_lookup[audio_end-audio_begin]) 
                    out_of_range = true;
            if (!out_of_range)
                all_source[i].audio_lookup_index = audio_lookup_elm - audio_begin - 1;
        }
        if (out_of_range)
        {
            printf("OOO\n");
            all_source[i].rtsp_enable_time = 0;
            return 0;
        }
        all_source[i].video_lookup_index = video_lookup_elm - video_begin - 1;
        all_source[i].rtsp_enable_time = timenow;
        return 1;
    }
    else if (l == 4)
    {
        if (!strcmp(s,"live"))
        {
            all_source[i].http_seek_time = 0;
            time_t timenow = time(NULL);
            all_source[i].rtsp_enable_time = timenow;
            return 1;
        }
        else
            return 0;
    }
    else if (l == 3)
    {
        if (!strcmp(s, "all"))
        {
            struct plot *plot = &all_source[i].vplot.plot_data;
            plot_start(plot->val.size());
            
            for (int j = 0; j < plot->val.size(); j++)
            {
                time_t temp_time = plot->time[j] + today_sec;
                struct tm *ptm = localtime(&temp_time);
                plot_write(ptm->tm_hour, ptm->tm_min, ptm->tm_sec, plot->val[j]);
                //printf("%d %d %d %f\n", ptm->tm_hour, ptm->tm_min, ptm->tm_sec, plot[0]->val[i]);
            }
            plot_end();
            png = plot_get(&pngsize);
            return 2;
        }
        else
            return 0;
    }
    return 0;
}
unsigned int copy_until(char* dest, const char *src, char c, int len)
{
    int i = 0;
    while (i < len - 1 && src[i] != 0)
    {
        if (src[i] == c)
        {
            dest[i] = 0;
            return i;
        }
        dest[i] = src[i];
        i++;
    }
    return 0;        
}
bool starts_with(const char *string, const char *prefix)
{
    while(*prefix && *string)
    {
        if(*prefix++ != *string++)
            return 0;
    }

    return 1;
}
char okreply[] = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nA";
char nokreply[] = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nB";
void* http_server_thread(void* param)
{
    plot_init();
    cbuf_t *cbuf = (cbuf_t *)calloc(1, sizeof(*cbuf));
    int server_fd, new_socket, valread;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buf[512];
    
    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
       
    // Forcefully attaching socket to the port 8080
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                                                  &opt, sizeof(opt)))
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons( 8089 );
       
    // Forcefully attaching socket to the port 8080
    if (bind(server_fd, (struct sockaddr *)&address, 
                                 sizeof(address))<0)
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    while(1)
    {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, 
                           (socklen_t*)&addrlen))<0)
        {
            perror("accept");
            exit(EXIT_FAILURE);
        }
        struct sockaddr_in *addr_in = (struct sockaddr_in *)&address;
        char* ip = inet_ntoa(addr_in->sin_addr);
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*) &tv, sizeof(tv));
        cbuf->fd = new_socket;
        int ret = read_line(cbuf, buf, sizeof(buf));
        if (starts_with(buf, "GET /"))
        {
            char temp[50];
            printf("%s ", ip);
            printf("%s", buf);
            if (copy_until(temp, &buf[5], ' ', 50))
            {
                unsigned char source_index;
                char ret = proc_cmd(temp, &source_index);
                if(ret == 0)
                    send(new_socket, nokreply, sizeof(nokreply)-1, 0);
                else if (ret == 1)
                {
                    send(new_socket, okreply, sizeof(okreply)-1, 0);
                    all_source[source_index].http_ip = ip;
                }
                else if (ret == 2)
                {
                    sprintf(buf, "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %d\r\n\r\n", pngsize);
                    send(new_socket, buf, strlen(buf), 0);
                    send(new_socket, png, pngsize, 0);
                    free_plot(png);
                }
            }
        }
        close(cbuf->fd);
        cbuf->wpos = 0;
        cbuf->rpos = 0;
    }
    free(cbuf);
}
