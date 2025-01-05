#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <gst/gst.h>
#include <gio/gio.h>
#include <stdbool.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/video/video-info.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <iostream>
#include <boost/circular_buffer.hpp>
//#include <opencv2/core.hpp>
#include <opencv2/opencv.hpp>
using namespace std;
using namespace cv;
#include "type.h"

extern struct cam_source *all_source;
extern int num_all_source;
extern uint64_t get_nsec();

int recv_all(int sock, uint8_t* buf, unsigned int size)
{
    unsigned int left = size;
    while (1)
    {
        int ret = recv(sock, &buf[size-left], left, 0);
        if (ret <= 0)
            return ret;
        else
        {
            left -= ret;
            if (!left) return size;
        }
    }
}
extern void* save_p[50];
extern unsigned int num_save_p;

#define RECV_LEN (40*1024)
void* esp32_cam_thread(void* param)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    if (setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
        printf("error setsockopt\n");
    struct sockaddr_in servaddr, client_addr;
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(4444);
    if (bind(sock, (struct sockaddr*)&servaddr, sizeof(servaddr)) != 0)
    {
        printf("socket bind failed...\n");
        return NULL;
    }
    struct sockaddr_in from;
    socklen_t fromlen;
    uint8_t data[RECV_LEN];
    ssize_t ret;
    struct cam_source* source = NULL;
    uint32_t i;
    while(1)
    {
        fromlen = sizeof(from);
        ret = recvfrom(sock, data, RECV_LEN, 0, (struct sockaddr*) &from, &fromlen);
        if (ret > 0)
        {
            source = NULL;
            if (ret < 3072)
            {
                for (i = 0; i < num_all_source; i++)
                {
                    if (ret != strlen(all_source[i].name)) continue;
                    if (!memcmp(all_source[i].name, (char*)data, ret))
                    {
                        source = &all_source[i];
                        break;
                    }
                }
                if (source)
                {
                    if (!source->cam_src_ip)
                    {
                        source->cam_src_ip = from.sin_addr.s_addr;
                        source->cam_src_port = from.sin_port;
                        source->last_time_with_udp_data = time(NULL);
                        if (source->last_time_connect == 0 && source->last_time_connect == 0)
                            printf("%s connected\n", source->name);
                        else
                            printf("%s connected after %d\n", source->name, source->last_time_with_udp_data - source->last_time_connect);
                        source->last_time_connect = source->last_time_with_udp_data;
                    }
                }
            }
            else if (ret > 3072)
            {
                for (i = 0; i < num_all_source; i++)
                    if (all_source[i].cam_src_ip == from.sin_addr.s_addr)
                    {
                        source = &all_source[i];
                        break;
                    }
                if (source)
                {
                    //printf("jpg %s\n", source->name);
                    gpointer pic_buf = g_malloc(ret);
                //    if (num_save_p < 50) 
                //    	save_p[num_save_p++] = pic_buf;
                    memcpy(pic_buf, data, ret);
                    GstBuffer *gbuf = gst_buffer_new_wrapped(pic_buf, ret);
                    GstClockTime pts = get_nsec();
                    if (!source->first_video_frame)
                    {
                        source->first_video_frame = true;
                        source->first_video_frame_time = pts;
                    }
                    GST_BUFFER_PTS(gbuf) = pts - source->first_video_frame_time;
                    gst_app_src_push_buffer((GstAppSrc*)source->jpgsrc, gbuf);
                    source->last_time_with_udp_data = time(NULL);
                }
            }
            else if (ret == 3072) /* audio buf */
            {
                for (i = 0; i < num_all_source; i++)
                    if (all_source[i].cam_src_ip == from.sin_addr.s_addr)
                    {
                        source = &all_source[i];
                        break;
                    }
                if (source)
                {
                    //printf("aud %s\n", source->name);
                    gpointer aud_buf = g_malloc(ret);
                  //  if (num_save_p < 50) 
                  //  	save_p[num_save_p++] = aud_buf;
                    memcpy(aud_buf, data, ret);
                    GstBuffer *gbuf = gst_buffer_new_wrapped(aud_buf, ret);
                    GstClockTime pts = get_nsec();
                    if (!source->first_audio_frame)
                    {
                        source->first_audio_frame = true;
                        source->first_audio_frame_time = pts;
                    }
                    GST_BUFFER_PTS(gbuf) = pts - source->first_audio_frame_time;
                    gst_app_src_push_buffer((GstAppSrc*)source->jpg_audio_src, gbuf);
                    source->last_time_with_udp_data = time(NULL);
                }
            }
        }
        time_t ts = time(NULL);;
        for (i = 0; i < num_all_source; i++)
            if (all_source[i].cam_src_ip != 0)
            {
                source = &all_source[i];
                if (ts - source->last_time_with_udp_data < 5u)
                {
                    if (ts - source->last_time_sent_udp_ack > 1u)
                    {
                        struct sockaddr_in toaddr;
                        toaddr.sin_addr.s_addr = source->cam_src_ip;
                        toaddr.sin_family = AF_INET;
                        toaddr.sin_port = source->cam_src_port;
                        char buf = 0xaa;
                        sendto(sock, &buf,1, 0, (const struct sockaddr*) &toaddr, sizeof(toaddr));
                        source->last_time_sent_udp_ack = ts;
                    }
                }
                else /* no data from cam */
                {
                    time_t ts = time(NULL);
                    source->cam_src_ip = 0;
                    printf("%s disconnected after %d\n", source->name, ts - source->last_time_connect);
                    source->last_time_connect = ts;
                }
            }
    }
    return NULL;
}
