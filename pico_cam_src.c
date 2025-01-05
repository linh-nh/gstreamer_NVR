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
#include <opencv2/opencv.hpp>
#include <unistd.h>
#include <net/if.h>
#include <sys/timerfd.h>
#include <pthread.h>
#include <sys/ioctl.h>

using namespace std;
using namespace cv;
#include "type.h"

#include <time.h>
#include <pcap.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>

extern struct cam_source *all_source;
extern int num_all_source;


extern void* save_p[50];
extern unsigned int num_save_p;

#define RECV_LEN (40*1024)

struct pico_source_info * pico_source;
uint32_t num_pico_source;

struct if_group
{
    char* if_name;
    uint32_t* source_id_group;
    uint32_t num_source_group;
};

#define JPG_BUF_SIZE (1024*1024*5)
struct pico_source_state
{
    int timerfd;
    int last_id;
    time_t last_time_with_data;
    uint8_t jpg_buf[JPG_BUF_SIZE];
    uint32_t jpg_buf_len;
    bool found_soi;
    bool found_eoi;
    bool find_soi_again;
    
};
struct pico_source_state * pico_state;

struct if_group *if_group;
uint32_t num_group;

extern uint64_t get_nsec();

void print_mac(uint8_t *mac)
{
    for (int i = 0; i < 6; i++)
        printf("%.2X ", mac[i]);
}

int rfind(uint8_t* data, uint32_t data_len, uint8_t* search, uint32_t search_len)
{
    assert(search_len <= data_len);
    uint32_t i = data_len - search_len;
    while (1)
    {
        bool found = true;
        for (int j = 0; j < search_len; j++)
        {
            if (data[i + j] != search[j])
            {
                found = false;
                break;
            }
        }
        if (found)
            return i;
        if (i == 0) break;
        i--;

    }
    return -1;
}
#define LOOKUP_LEN 2000

void* pcap_receive(void* param)
{
    char *device;
    char error_buffer[PCAP_ERRBUF_SIZE];
    pcap_t *handle;
    const u_char *packet;
    struct pcap_pkthdr packet_header;
    int packet_count_limit = 1;
    int i;
    
    pcap_if_t *interfaces,*temp;
    struct if_group* gr = &if_group[(uint32_t)(size_t)param];

    handle = pcap_create(gr->if_name, error_buffer);
    if (!handle)
    {
        printf("%s\n", error_buffer);
        return NULL;
    }
    printf("set immediate %d\n", pcap_set_immediate_mode(handle, 1));
    printf ("activate %d\n", pcap_activate(handle));
    struct bpf_program pgm;
    pcap_compile(handle, &pgm, "ether proto (0x88b5 or 0x88b6)", 1, 0xffffff);
    
    pcap_setfilter(handle, &pgm);
    struct itimerspec next_time = 
    {
        .it_interval = 
        {
            .tv_sec = 0,
            .tv_nsec = 0,
        },
        .it_value =
        {
            .tv_sec = 3,
            .tv_nsec = 0,
        },
    };
    for (i = 0; i < gr->num_source_group; i++)
    {
        timerfd_settime(pico_state[gr->source_id_group[i]].timerfd, 0, &next_time, NULL);
    }
    
    while (1)
    {
        packet = pcap_next(handle, &packet_header);

        if (packet == NULL) {
            printf("No packet found %s\n", gr->if_name);
            return NULL;
        }
        bool found_source;
        struct pico_source_info* cur_source;
        for (i = 0; i < gr->num_source_group; i++)
        {
            found_source = true;
            cur_source = &pico_source[gr->source_id_group[i]];
            for (int j = 0; j < 6; j++)
            {
                if (cur_source->cam_mac[j] != packet[6 + j])
                {
                    found_source = false;
                    break;
                }
            }
            if (found_source)
                break;
        }
        if (!found_source) continue;

        struct pico_source_state* state = &pico_state[gr->source_id_group[i]];
        if (packet[13] == 0xb5)
        {
            if (state->jpg_buf_len + packet_header.len - 15 > JPG_BUF_SIZE)
            {
                state->jpg_buf_len = 0;
                continue;
            }
            memcpy(&state->jpg_buf[state->jpg_buf_len], &packet[15], packet_header.len - 15);
            state->jpg_buf_len += packet_header.len - 15;
            
            if (packet[14] < state->last_id)
            {
                if (state->found_soi)
                {
                    uint8_t* packet_lookup;
                    uint32_t packet_lookup_len;
                    if (state->jpg_buf_len > LOOKUP_LEN)
                    {
                        packet_lookup = &state->jpg_buf[state->jpg_buf_len - LOOKUP_LEN];
                        packet_lookup_len = LOOKUP_LEN;
                    }
                    else
                    {
                        packet_lookup = state->jpg_buf;
                        packet_lookup_len = state->jpg_buf_len;
                    }
                    int eoi_pos = rfind(packet_lookup, packet_lookup_len, (uint8_t*)"\xff\xd9", 2);
                    if (eoi_pos >= 0)
                    {
                        uint32_t jpg_len = packet_lookup - state->jpg_buf + eoi_pos + 2;
                        gpointer pic_buf = g_malloc(jpg_len);
                        memcpy(pic_buf, state->jpg_buf, jpg_len);

                        GstBuffer *gbuf = gst_buffer_new_wrapped(pic_buf, jpg_len);
                        time_t ts = time(NULL);
                        GstClockTime pts = get_nsec();
                        if (!cur_source->source->first_video_frame)
                        {
                            cur_source->source->first_video_frame = true;
                            cur_source->source->first_video_frame_time = pts;
                        }
                        GST_BUFFER_PTS(gbuf) = pts - cur_source->source->first_video_frame_time;
                        
                        gst_app_src_push_buffer((GstAppSrc*)cur_source->video_src, gbuf);
                        timerfd_settime(state->timerfd, 0, &next_time, NULL);
                        
                        int sec_diff = ts - state->last_time_with_data;
                        if (sec_diff > 3)
                        {
                            print_mac(cur_source->cam_mac);
                            printf("connected after %d sec\n", sec_diff);
                        }
                        state->last_time_with_data = ts;
                        
                        uint32_t soi_search_len = state->jpg_buf_len - jpg_len;
                        uint8_t* soi_p = (uint8_t*)memmem(&state->jpg_buf[jpg_len], soi_search_len, "\xff\xd8", 2);
                        if (soi_p)
                        {
                            uint32_t new_len = state->jpg_buf_len - (soi_p - state->jpg_buf);
                            memmove(state->jpg_buf, soi_p, new_len);
                            state->jpg_buf_len = new_len;
                            state->found_soi = true;
                            state->find_soi_again = false;
                        }
                        else
                        {
                            memmove(state->jpg_buf, &state->jpg_buf[jpg_len], soi_search_len);
                            state->jpg_buf_len = soi_search_len;
                            state->found_soi = false;
                            state->find_soi_again = true;
                        }
                    }
                }
                else
                {
                    uint8_t* packet_lookup;
                    uint32_t packet_lookup_len;
                    if (state->jpg_buf_len > LOOKUP_LEN)
                    {
                        packet_lookup = &state->jpg_buf[state->jpg_buf_len - LOOKUP_LEN];
                        packet_lookup_len = LOOKUP_LEN;
                    }
                    else
                    {
                        packet_lookup = state->jpg_buf;
                        packet_lookup_len = state->jpg_buf_len;
                    }
                    int soi_pos = rfind(packet_lookup, packet_lookup_len, (uint8_t*) "\xff\xd8", 2);
                    if (soi_pos >= 0)
                    {
                        uint32_t new_len = packet_lookup_len - soi_pos;
                        memmove(state->jpg_buf, &packet_lookup[soi_pos], new_len);
                        state->jpg_buf_len = new_len;
                        state->found_soi = true;
                        state->find_soi_again = false;
                    }
                }
            }
            else if (packet[14] > state->last_id)
            {
                if (state->find_soi_again)
                {
                    uint8_t* soi_p = (uint8_t*)memmem(state->jpg_buf, state->jpg_buf_len, (uint8_t*)"\xff\xd8", 2);
                    if (soi_p)
                    {
                        uint32_t new_len = state->jpg_buf_len - (soi_p - state->jpg_buf);
                        memmove(state->jpg_buf, soi_p, new_len);
                        state->jpg_buf_len = new_len;
                        state->found_soi = true;
                        state->find_soi_again = false;
                    }
                    else
                    {
                        state->found_soi = false;
                        state->find_soi_again = false;
                    }
                }
            }
            state->last_id = packet[14];
        }
        else if (packet[13] == 0xb6)
        {
            if (cur_source->audio_src)
            {
                uint32_t data_len = packet_header.len - 14;
                gpointer aud_buf = g_malloc(data_len);

                memcpy(aud_buf, &packet[14], data_len);
                GstBuffer *gbuf = gst_buffer_new_wrapped(aud_buf, data_len);
                time_t ts = time(NULL);
                GstClockTime pts = get_nsec();
                if (!cur_source->source->first_audio_frame)
                {
                    cur_source->source->first_audio_frame = true;
                    cur_source->source->first_audio_frame_time = pts;
                }
                GST_BUFFER_PTS(gbuf) = pts - cur_source->source->first_audio_frame_time;
                gst_app_src_push_buffer((GstAppSrc*)cur_source->audio_src, gbuf);

                int sec_diff = ts - state->last_time_with_data;
                if (sec_diff > 3)
                {
                    print_mac(cur_source->cam_mac);
                    printf("connected after %d sec\n", sec_diff);
                }
                state->last_time_with_data = ts;
            }
            
        }
    }
}
void get_mac_from_if_name(char* name, uint8_t* mac)
{
    int s;
    struct ifreq buffer;

    s = socket(PF_INET, SOCK_DGRAM, 0);

    memset(&buffer, 0x00, sizeof(buffer));

    strcpy(buffer.ifr_name, name);

    ioctl(s, SIOCGIFHWADDR, &buffer);
    close(s);
    for(int i = 0; i < 6; i++)
    {
        mac[i] = buffer.ifr_hwaddr.sa_data[i];
    }
}

void* tcp_check(void* param)
{
    struct itimerspec next_time = 
    {
        .it_interval = 
        {
            .tv_sec = 0,
            .tv_nsec = 0,
        },
        .it_value =
        {
            .tv_sec = 3,
            .tv_nsec = 0,
        },
    };
    struct pico_source_info *source = &pico_source[(uint32_t)(size_t)param];
    struct pico_source_state *state = &pico_state[(uint32_t)(size_t)param];
    
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(4444);
    if (inet_pton(AF_INET, source->cam_ip, &serv_addr.sin_addr) <= 0) 
    {
        printf("Invalid address %s\n", source->cam_ip);
        return NULL;
    }
    uint8_t mac[6];
    get_mac_from_if_name(source->recv_if, mac);
    bool wait_timeout = true;
    bool first_noconnect = true;
    while(1)
    {
        if (wait_timeout)
        {
            uint64_t readval;
            read(state->timerfd, &readval, 8);
            print_mac(source->cam_mac);
            printf("disconnected\n");
        }
        wait_timeout = false;
        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        int status = connect(client_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        if (status < 0)
        {
            if (first_noconnect)
            {
                printf("cannot connect to %s\n", source->cam_ip);
                first_noconnect = false;
            }   
            close(client_fd);
            sleep(3);
            continue;
        }
        else
        {
            printf("connected to %s\n", source->cam_ip);
            first_noconnect = true;
        }
        send(client_fd, "\x06", 1, 0);
        uint32_t buf32;
        int ret = recv(client_fd, &buf32, 4, 0);
        if (ret != 4)
        {
            printf("recv ret %d\n", ret);
            close(client_fd); 
            continue;
        }
        if (buf32 == 0)
        {
            printf("Sending boot command to %s\n", source->cam_ip);
            uint32_t start_data[3] = 
            {
                0x1004b0f7, //pc
                0x20042000, //sp
                0x1004b000, // vtor
            };
            send(client_fd, "\x05", 1, 0);
            send(client_fd, start_data, 12, 0);
            close(client_fd);
        }
        else
        {
            printf("Sending dest mac to %s\n", source->cam_ip);
            send(client_fd, "\x07", 1, 0);
            send(client_fd, mac, 6, 0);
            close(client_fd);
            timerfd_settime(state->timerfd, 0, &next_time, NULL);
            wait_timeout = true;
        }
    }
}

void* pico_cam_thread(void* param)
{
    int i=0;

    for (i = 0; i < num_pico_source; i++)
    {
        bool in_group = false;
        for (int j = 0; j < num_group; j++)
        {
            if (!strcmp(pico_source[i].recv_if, if_group[j].if_name))
            {
                in_group = true;
                if_group[j].source_id_group = (uint32_t*) realloc(if_group[j].source_id_group, 4 * (if_group[j].num_source_group + 1));
                if_group[j].source_id_group[if_group[j].num_source_group] = i;
                if_group[j].num_source_group++;
                break;
            }
        }
        if (!in_group)
        {
            if (!if_group)
                if_group = (struct if_group*)malloc(sizeof(struct if_group));
            else
                if_group = (struct if_group*)realloc(if_group, sizeof(struct if_group) * (num_group + 1));
            if_group[num_group].if_name = pico_source[i].recv_if;
            if_group[num_group].source_id_group = (uint32_t*)malloc(4);
            if_group[num_group].num_source_group = 1;
            if_group[num_group].source_id_group[0] = i;
            num_group++;
        }
    }
    pico_state = (struct pico_source_state*) malloc(sizeof(struct pico_source_state) * num_pico_source);
    memset(pico_state, 0, sizeof(struct pico_source_state) * num_pico_source);
    for (i = 0; i < num_pico_source; i++)
    {
        pico_state[i].last_id = 255;
        pico_state[i].timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        pthread_t threadId;
        int thread_err = pthread_create(&threadId, NULL, &tcp_check, (void*)i);  
    }
    for (i = 0; i < num_group; i++)
    {
        pthread_t threadId;
        int thread_err = pthread_create(&threadId, NULL, &pcap_receive, (void*)i);             
    }
    return NULL;
}
