#ifndef TYPE_H
#define TYPE_H

struct rtp_client
{
    GstElement *rtpvideosrc;
    GstElement *rtpaudiosrc;
    FILE *audiofileread;
    FILE *videofileread;
    GstClockTime firstsamplesendtime;
    GMutex firstsamplesendtime_mutex;
    struct rtp_client* next;
    struct rtp_client* prev;
    struct cam_source* source;
    GstClockTime need_data_first_pts;
    bool is_live;
};

struct plot
{
    boost::circular_buffer<guint32> time;
    boost::circular_buffer<float> val;
};
struct video_plot
{
    struct plot plot_data;
    Mat prev_frame;
    GstClockTime prev_frame_time;
};

struct rtspsrc_save_link
{
    uint8_t num_peer;
    GstElement** elem;
};

struct cam_source
{
    char* name;
//    char* pipeline_cmd;
    GstElement *pipeline;
    GstElement *videoappsink;
    GstElement *audioappsink;
    GstElement *videoprocappsink;
    GstElement *audioprocappsink;
    GstElement *jpgsrc;
    GstElement *jpg_audio_src;
    char* videofilename;
    char* audiofilename;
    char* plotfilename;
    FILE *audiofile;
    FILE *videofile;
    
    GMutex client_mutex;
    struct rtp_client* client;

    uint32_t max_video_frame_bytes;
    uint32_t max_audio_frame_bytes;

    boost::circular_buffer<GstClockTime> video_time_lookup;
    boost::circular_buffer<guint64> video_pos_lookup;
    boost::circular_buffer<GstClockTime> audio_time_lookup;
    boost::circular_buffer<guint64> audio_pos_lookup;
    struct video_plot vplot;
    time_t rtsp_enable_time;
    time_t http_seek_time;
    bool rtsp_live;
    int video_lookup_index;
    int audio_lookup_index;
    void* have_client;
    guint64 audioframesize;
    guint64 videoframesize;
    char* http_ip;
    GstRTSPMediaFactory *factory;
    GstCaps* videocaps;
    char* videopaytext;
    char* audiopaytext;
    GstCaps* audiocaps;
    GMutex factory_set_launch_mutex;
    uint32_t cam_src_ip;
    uint16_t cam_src_port;
    time_t last_time_with_udp_data;
    time_t last_time_sent_udp_ack;
    time_t last_time_connect;
    
    char** param;
    uint8_t num_param;
    
    uint8_t retry_state;
    
    uint64_t first_video_frame_time;
    bool first_video_frame;
    uint64_t first_audio_frame_time;
    bool first_audio_frame;
    
    bool have_rtspsrc;
    bool have_error;
    bool rtspsrc_saved;
    uint8_t rtspsrc_num;
    struct rtspsrc_save_link*  rtspsrc_save_link;
};

struct pico_source_info
{
    char* recv_if;
    char* cam_ip;
    uint8_t* cam_mac;
    GstElement *video_src;
    GstElement *audio_src;
    struct cam_source* source;
};

#endif /* TYPE_H */

