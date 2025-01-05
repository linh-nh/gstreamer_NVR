/* gcc gst_app.c -o gst_app -g `pkg-config --cflags --libs gstreamer-1.0 gtk+-3.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0` */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <gst/gst.h>
#include <gio/gio.h>
#include <stdbool.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/video/video-info.h>
#include <iostream>
#include <boost/circular_buffer.hpp>
#include <sys/resource.h>
#include <opencv2/opencv.hpp>
using namespace std;
using namespace cv;
#include "type.h"

#define VIDEO_PLOT_NUM_POINT 86400
#define VIDEO_PLOT_INTERVAL 1

struct cam_source *all_source;
int num_all_source;

GstRTSPServer *server;
GstRTSPMountPoints *mounts;
GstBus *bus;

time_t today_sec;
uint64_t get_nsec()
{
    struct timespec tspec;
    clock_gettime(CLOCK_REALTIME, &tspec);
    
    return (tspec.tv_sec - today_sec) * 1000000000 + tspec.tv_nsec;
}

static void add_to_list(struct rtp_client** source_list, struct rtp_client* elem, GMutex* lock)
{
    g_mutex_lock(lock);
    struct rtp_client * list = *source_list;
    *source_list = elem;
    if (!list)
    {   
        elem->next = elem->prev = NULL;
        goto ret;
    }
    list->prev = elem;
    elem->next = list;
    elem->prev = NULL;
ret:
    g_mutex_unlock(lock);
}

static void remove_from_list(struct rtp_client** source_list, struct rtp_client* elem, GMutex *lock)
{
    g_mutex_lock(lock);
    struct rtp_client * list = *source_list;
    if (!(elem->next || elem->prev))
    {
        *source_list = NULL;
        goto ret;
    }
    if (!elem->next)
    {
        elem->prev->next = NULL;
        goto ret;
    }
    if (!elem->prev)
    {
        elem->next->prev = NULL;
        *source_list = elem->next;
        goto ret;
    }
    elem->next->prev = elem->prev;
    elem->prev->next = elem->next;
ret:
    g_mutex_unlock(lock);
}

gboolean retryFunc(gpointer data)
{
    struct cam_source *source = (struct cam_source *)data;
    GstElement* pipeline = source->pipeline;
    GstState state, state_pend;
    GstStateChangeReturn scr = gst_element_get_state(pipeline, &state, &state_pend, 0); 
    printf("Current state:%d\nPending State: %d\nGetState return: %d\nretry...\n",state, state_pend, scr);
    if (source->retry_state == 1)
    {
        if (source->have_error)
        {
            /* wait until stable and no more error */
            source->have_error = false;
            return TRUE;
        }
        gst_element_set_state (pipeline, GST_STATE_NULL);
        source->retry_state = 2;
        return TRUE;
    }
    else if (source->retry_state == 2)
    {
        gst_element_set_state (pipeline, GST_STATE_PLAYING);
        if (source->have_error)
        {
            source->retry_state = 1;
            source->have_error = 0;
            return TRUE;
        }
        else
        {
            source->retry_state = 0;
            return FALSE;
        }
    }
    return TRUE;
}

#define ALLOC(var,type,num) do { \
                              if (!(var)) \
                                  (var) = (type*)malloc(sizeof(type)); \
                              else \
                                  (var) = (type*)realloc((var), sizeof(type)*(num)); \
                            } while (0)

static void add_pad_cb (GstElement *src, GstPad *pad, gpointer data)
{
    struct rtspsrc_save_link* save = (struct rtspsrc_save_link*) data;
    GstPad *peer_pad = gst_pad_get_peer(pad);
    if (peer_pad)
        return;
    else
    {
        for (int i = 0; i < save->num_peer; i++)
        {
            gst_element_link(src, save->elem[i]);
        }
    }
    
}

void save_rtsp_elem_link(struct cam_source* source, GstElement* elem)
{
    GstIterator* pad_iter = gst_element_iterate_src_pads(elem);
    GValue iter_elem = G_VALUE_INIT;
    bool iter_done = false;
    struct rtspsrc_save_link* save = NULL;

    while (!iter_done)
    {
        switch (gst_iterator_next (pad_iter, &iter_elem)) 
        {
            case GST_ITERATOR_OK:
            {
                GstPad *pad = GST_PAD(g_value_get_object (&iter_elem));
                GstPad *peer_pad = gst_pad_get_peer(pad);
                if (peer_pad)
                {
                    if (!save)
                    {
                        ALLOC(source->rtspsrc_save_link, struct rtspsrc_save_link, source->rtspsrc_num+1);
                        save = &source->rtspsrc_save_link[source->rtspsrc_num];
                        save->num_peer = 0;
                        save->elem = NULL;
                        source->rtspsrc_num++;
                        source->have_rtspsrc = true;
                        g_signal_connect(elem, "pad-added", G_CALLBACK(add_pad_cb), save);
                    }
                    ALLOC(save->elem, GstElement*, save->num_peer+1);
                    GstElement* peer_elem = (GstElement*)gst_pad_get_parent(peer_pad);
                    save->elem[save->num_peer++] = peer_elem;
                    gst_object_unref(peer_elem);
                }
                break;
            }
            case GST_ITERATOR_RESYNC:
            case GST_ITERATOR_ERROR:
            case GST_ITERATOR_DONE:
                iter_done = TRUE;
                break;
        }
    }
    if (save)
        printf("%s: rtspsrc num saved peer elem:%d\n", source->name, save->num_peer);
    g_value_unset (&iter_elem);
    gst_iterator_free (pad_iter);
}

void save_rtsp_src_link(struct cam_source* source)
{
    GstIterator * pipeline_iter = gst_bin_iterate_recurse(GST_BIN (source->pipeline));
    GValue iter_elem = G_VALUE_INIT;
    bool iter_done = false;
    while (!iter_done)
    {
        switch (gst_iterator_next (pipeline_iter, &iter_elem)) 
        {
            case GST_ITERATOR_OK:
            {
                GstElement *elem = GST_ELEMENT(g_value_get_object (&iter_elem));
                const char* long_name = gst_element_get_metadata (elem,GST_ELEMENT_METADATA_LONGNAME);
                if (!strcmp(long_name, "RTSP packet receiver"))
                {
                    save_rtsp_elem_link(source, elem);
                }
                g_value_reset (&iter_elem);
                break;
            }
            case GST_ITERATOR_RESYNC:
            case GST_ITERATOR_ERROR:
            case GST_ITERATOR_DONE:
                iter_done = TRUE;
                break;
        }
    }
    g_value_unset (&iter_elem);
    gst_iterator_free (pipeline_iter);
}

void trigger_rtsp_retry(struct cam_source* source)
{
    source->have_error = true;
    if (source->have_rtspsrc && source->retry_state == 0)
    {
        source->retry_state = 1;
        source->have_error = false;
        g_timeout_add_seconds(3, retryFunc, source);
    }
}

static gboolean message_cb (GstBus * bus, GstMessage * message, gpointer user_data)
{
    switch (GST_MESSAGE_TYPE (message)) 
    {
        case GST_MESSAGE_ERROR:
        {
            GError *err = NULL;
            gchar *name, *debug = NULL;

            name = gst_object_get_path_string (message->src);
            gst_message_parse_error (message, &err, &debug);

            g_printerr ("ERROR MESSAGE: from element %s: %s\n", name, err->message);
            if (debug != NULL)
                g_printerr ("ERROR MESSAGE additional debug info:\n%s\n", debug);
            trigger_rtsp_retry((struct cam_source *)user_data);
            break;
        }
        case GST_MESSAGE_EOS:
        {
            struct cam_source * source = (struct cam_source *)user_data;
            g_print ("Got EOS message from %s\n", source->name);
            trigger_rtsp_retry((struct cam_source *)source);
            break;
        }

            break;
        case GST_MESSAGE_WARNING:
        {
           GError *err = NULL;
           gchar *name, *debug = NULL;

           name = gst_object_get_path_string (message->src);
           gst_message_parse_warning (message, &err, &debug);

           g_printerr ("WARNING MESSAGE: from element %s: %s\n", name, err->message);
           if (debug != NULL)
              g_printerr ("WARNING MESSAGE additional debug info:\n%s\n", debug);

           g_error_free (err);
           g_free (debug);
           g_free (name);
           break;
        }

        case GST_MESSAGE_STATE_CHANGED:
        {
            struct cam_source *source = (struct cam_source *)user_data;
            GstState old_state, new_state;

            gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
            if ((GstElement*)message->src == source->pipeline && new_state == GST_STATE_PLAYING)
            {
                if (!source->rtspsrc_saved)
                {
                    save_rtsp_src_link(source);
                    source->rtspsrc_saved = true;
                }
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}

static void handlecommand (gchar *sz)
{
    g_print ("You typed: %s\n", sz);
}


static gboolean
mycallback (GIOChannel *channel, GIOCondition cond, gpointer data)
{
    gchar *str_return;
    gsize length;
    gsize terminator_pos;
    GError *error = NULL;
    if (g_io_channel_read_line (channel, &str_return, &length, &terminator_pos, &error) == G_IO_STATUS_ERROR)
    {
        g_warning ("Something went wrong");
    }
    if (error != NULL)
    {
        g_print ("MYCALLBACK EERROR:%s\n",error->message);
        exit(1);
    }
    handlecommand (str_return);
    g_free( str_return );
    return TRUE;
}
void eos(GstAppSink *sink, gpointer user_data)
{
    printf("eos\n");
}


GstClockTime time_lookup_interval = 5000000000UL; /* 5 sec interval for time lookup series */

unsigned int lookup_size = 20000;

static void media_destroy_cb(void* p1)
{
    struct rtp_client *client = (struct rtp_client*) p1;
    printf("media destroy\n");

    if (client->videofileread) 
    {
        fclose(client->videofileread);
    }
    if (client->audiofileread) 
    {
        fclose(client->audiofileread);
    }
    remove_from_list(&client->source->client, client, &client->source->client_mutex);
    free(client);
}
char* plot_get(unsigned int *size);
void plot_end();
void plot_write(int hour, int min, int sec, float val);
void plot_start(unsigned int num_point);
void free_plot(char* buf);
void save_video_plot()
{
    char* png;
    unsigned int pngsize;
    for (int i = 0; i < num_all_source; i++)
    {
        struct plot *plot = &all_source[i].vplot.plot_data;
        plot_start(plot->val.size());
        
        for (int j = 0; j < plot->val.size(); j++)
        {
            time_t temp_time = plot->time[j];
            struct tm *ptm = localtime(&temp_time);
            plot_write(ptm->tm_hour, ptm->tm_min, ptm->tm_sec, plot->val[j]);
            //printf("%d %d %d %f\n", ptm->tm_hour, ptm->tm_min, ptm->tm_sec, plot[0]->val[i]);
        }
        plot_end();
        png = plot_get(&pngsize);
        FILE* f = fopen(all_source[i].plotfilename, "w");
        fwrite(png, pngsize, 1, f);
        fclose(f);
        free_plot(png);
    }    
}
int prev_time_need_new_file;
volatile bool stop = false;
void check_need_new_file()
{
    if (!stop)
    {
        time_t timenow = time(NULL);
        struct tm *ptm = localtime(&timenow);
        if (ptm->tm_mday != prev_time_need_new_file)
        {
            stop = true;
            save_video_plot();
            for (int i = 0; i < num_all_source; i++)
            {
                fsync(fileno(all_source[i].audiofile));
                fsync(fileno(all_source[i].videofile));
                fclose(all_source[i].audiofile);
                fclose(all_source[i].videofile);
                //gst_element_set_state (all_source[i].pipeline, GST_STATE_NULL);
                //gst_object_unref (all_source[i].pipeline);
                all_source[i].audiofile = NULL;
                all_source[i].videofile = NULL;
                char* cmdline = g_strdup_printf("./convert.sh %s %s %s.mkv", all_source[i].videofilename, all_source[i].audiofilename, all_source[i].videofilename);
                int pid = fork();
                if( pid < 0 )
                {
                    printf("error fork\n");
                }
                if (pid > 0)
                {
                    struct rlimit lim;
                    getrlimit(RLIMIT_NOFILE, &lim); /* rlim_cur 1024 */
                    for (i = 3; i <= lim.rlim_cur; ++i) close (i);
                    system(cmdline);
                }
            }
            printf("exit now...\n");
            exit(0);
        }
    }
}
void assign_file_name(struct cam_source *source)
{
    time_t timenow = time(NULL);
    struct tm *ptm = localtime(&timenow);
    unsigned int i;
    char* temp = g_strdup_printf("vid_%s_%.4u_%.2u_%.2u", source->name, ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday);
    i = 1;
    while(!access(temp, F_OK))
    {
        g_free(temp);
        temp = g_strdup_printf("vid_%s_%.4u_%.2u_%.2u_%.2u", source->name, ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday, i);
        i++;
    }
    source->videofilename = temp;
    
    temp = g_strdup_printf("plot_%s_%.4u_%.2u_%.2u", source->name, ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday);
    i = 1;
    while(!access(temp, F_OK))
    {
        g_free(temp);
        temp = g_strdup_printf("plot_%s_%.4u_%.2u_%.2u_%.2u", source->name, ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday, i);
        i++;
    }
    source->plotfilename = temp;
    
    
    if (source->audioappsink)
    {
        temp = g_strdup_printf("aud_%s_%.4u_%.2u_%.2u", source->name, ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday);
        i = 1;
        while(!access(temp, F_OK))
        {
            g_free(temp);
            temp = g_strdup_printf("aud_%s_%.4u_%.2u_%.2u_%.2u", source->name, ptm->tm_year+1900, ptm->tm_mon+1, ptm->tm_mday, i);
            i++;
        }
        source->audiofilename = temp;
    }
}
void set_factory_launch_str(struct cam_source *source)
{
    g_mutex_lock(&source->factory_set_launch_mutex);
    if (source->videopaytext && (!source->audioappsink || source->audiopaytext))
    {
        char* temp;
        if (source->audioappsink)
            temp = g_strdup_printf("( appsrc name=videosrc ! %s name=pay0 pt=96 appsrc name=audiosrc ! %s name=pay1 pt=97 )", source->videopaytext, source->audiopaytext);
        else
            temp = g_strdup_printf("( appsrc name=videosrc ! %s name=pay0 pt=96 )", source->videopaytext);
        gst_rtsp_media_factory_set_launch (source->factory, temp);
        g_free(temp);
        temp = g_strdup_printf("/%s/test", source->name);
        gst_rtsp_mount_points_add_factory (mounts, temp, source->factory);
        g_free(temp);
        if (source->audioappsink)
        {
            g_free(source->audiopaytext);
            source->audiopaytext = NULL;
        }
        g_free(source->videopaytext);
        source->videopaytext = NULL;
    }
    g_mutex_unlock(&source->factory_set_launch_mutex);
}
GstFlowReturn new_buffer(GstAppSink *sink, gpointer user_data)
{
//    printf("new_buffer\n");
    if (!stop)
    {
        struct cam_source *source = (struct cam_source*) user_data;
        GstSample* sample;
        while (1)
        {
            sample = gst_app_sink_try_pull_sample(sink, 0);
            if (!sample) break;
            if (!source->videocaps)
            {
                GstCaps* cap = gst_sample_get_caps(sample);
                gchar* capstr = gst_caps_to_string(cap);
                char* temp = strstr(capstr, "video/x-");
                char* temp2 = strchr(&temp[8], ',');
                char* temp3 = g_strndup(&temp[8], temp2-&temp[8]);
                source->videopaytext = g_strconcat("rtp", temp3, "pay", NULL);
                g_free(temp3);
                g_free(capstr);
                source->videocaps = gst_caps_copy(cap);
            }
            if (source->videopaytext && (!source->audioappsink || source->audiopaytext))
            {
                set_factory_launch_str(source);
            }
            GstBuffer* buf = gst_sample_get_buffer(sample);
            GST_BUFFER_PTS(buf) = GST_BUFFER_DTS(buf) = get_nsec();
            GstMapInfo mapinfo;
            if (!gst_buffer_map(buf, &mapinfo, GST_MAP_READ))
            {
                printf("cannot map buffer");
                return GST_FLOW_OK;
            }
            GstBufferFlags flags = gst_buffer_get_flags(buf);
            if (!(flags & GST_BUFFER_FLAG_DELTA_UNIT))
            {
                bool write_seek_point = false;
                if (!source->video_time_lookup.size())
                    write_seek_point = true;
                else if (GST_BUFFER_PTS(buf) - source->video_time_lookup[source->video_time_lookup.size()-1] >= time_lookup_interval)
                    write_seek_point = true;
                if (write_seek_point)
                {
                    source->video_time_lookup.push_back(GST_BUFFER_PTS(buf));
                    source->video_pos_lookup.push_back(ftell(source->videofile));
        /*            printf("video pts %lu end-begin %d size %d endval %lu sizeval %lu\n",
                           GST_BUFFER_PTS(buf), 
                           video_time_lookup.end()-video_time_lookup.begin(),
                           video_time_lookup.size(),
                           video_time_lookup[video_time_lookup.end()-video_time_lookup.begin()-1],
                           video_time_lookup[video_time_lookup.size()-1]);
        */
                    //printf("video pts %lu\n", GST_BUFFER_PTS(buf));
                }
            }
            fwrite(&mapinfo.size, 4, 1,source->videofile);
            if (mapinfo.size > source->max_video_frame_bytes)
                source->max_video_frame_bytes = mapinfo.size;
        //    printf("size %u\n", mapinfo.size);
            fwrite(mapinfo.data, mapinfo.size, 1, source->videofile);
            fwrite(&flags, sizeof(flags), 1, source->videofile);
        //    printf("pts %d\n", GST_BUFFER_PTS(buf));
            fwrite(&GST_BUFFER_PTS(buf), sizeof(GstClockTime), 1, source->videofile);
            struct rtp_client* client = source->client;
            
            while (client)
            {
                if (client->is_live)
                {
                    GstState state;
                    gst_element_get_state(client->rtpvideosrc, &state, NULL, 0);
                    if (state == GST_STATE_PAUSED || state == GST_STATE_PLAYING)
                    {
                        guint64 max = gst_app_src_get_max_bytes((GstAppSrc*)client->rtpvideosrc);
                        guint64 cur = gst_app_src_get_current_level_bytes((GstAppSrc*)client->rtpvideosrc);
                        if (cur + mapinfo.size <= max)
                        {
                            //gst_element_get_state(pay0, &state, NULL, 0);
                            //g_print("pay0 %d\n", state);
                        	GstBuffer *copybuffer = gst_buffer_copy(buf);
                        	GstClockTime pts;
                        	if (!client->firstsamplesendtime)
                        	{
                            	g_mutex_lock(&client->firstsamplesendtime_mutex);
                            	if (!client->firstsamplesendtime)
                            	{
                            		client->firstsamplesendtime = GST_BUFFER_PTS (copybuffer);
                            	}
                            	g_mutex_unlock(&client->firstsamplesendtime_mutex);
                            }
                        	GST_BUFFER_PTS (copybuffer) -= client->firstsamplesendtime;
                        	GST_BUFFER_DTS (copybuffer) = GST_BUFFER_PTS (copybuffer);
                            gst_app_src_push_buffer((GstAppSrc*)client->rtpvideosrc, copybuffer);
                        }
                    }
                }
                client = client->next;
            }
            gst_buffer_unmap(buf, &mapinfo);
            gst_sample_unref(sample);
        }
        return GST_FLOW_OK;
    }
    else
        return GST_FLOW_EOS;
}

GstFlowReturn new_preroll(GstAppSink *sink, gpointer user_data)
{
    if (!stop)
    {
        struct cam_source *source = (struct cam_source *)user_data;
        if (!source->videofile)
        {
            struct cam_source *source = (struct cam_source*) user_data;
            source->videofile = fopen(source->videofilename,"w");
        }
        return GST_FLOW_OK;
    }
    else
        return GST_FLOW_EOS; 
}

void audio_eos(GstAppSink *sink, gpointer user_data)
{
    printf("eos\n");
}

GstFlowReturn audio_new_buffer(GstAppSink *sink, gpointer user_data)
{
//    printf("audio_new_buffer\n");
    if (!stop)
    {
        GstSample* sample = gst_app_sink_pull_sample((GstAppSink*)sink);
        struct cam_source *source = (struct cam_source*) user_data;
        if (!source->audiocaps)
        {
            GstCaps* cap = gst_sample_get_caps(sample);
            gchar* capstr = gst_caps_to_string(cap);
            char* temp = strstr(capstr, "audio/x-");
            char* temp2 = strchr(&temp[8], ',');
            char* temp3 = g_strndup(&temp[8], temp2-&temp[8]);
            if (!strcmp(temp3,"opus")) /* support only opus for now */
            {
                source->audiopaytext = g_strconcat("rtp", temp3, "pay", NULL);
            }
            else
            {
                source->audiopaytext = g_strdup("rtpgstpay");
            }
            g_free(temp3);
            g_free(capstr);
            source->audiocaps = gst_caps_copy(cap);
        }
        if (source->videopaytext && source->audiopaytext)
        {
            set_factory_launch_str(source);
        }
        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstMapInfo mapinfo;
        if (!gst_buffer_map(buf, &mapinfo, GST_MAP_READ))
        {
            printf("cannot map buffer");
            return GST_FLOW_OK;
        }
        GST_BUFFER_PTS(buf) = GST_BUFFER_DTS(buf) = get_nsec();
        GstBufferFlags flags = gst_buffer_get_flags(buf);
        if (!(flags & GST_BUFFER_FLAG_DELTA_UNIT))
        {
            bool write_seek_point = false;
            if (!source->audio_time_lookup.size())
                write_seek_point = true;
            else if (GST_BUFFER_PTS(buf) - source->audio_time_lookup[source->audio_time_lookup.size()-1] >= time_lookup_interval)
                write_seek_point = true;
            if (write_seek_point)
            {
                source->audio_time_lookup.push_back(GST_BUFFER_PTS(buf));
                source->audio_pos_lookup.push_back(ftell(source->audiofile));
    /*            printf("audio pts %lu end-begin %d size %d endval %lu sizeval %lu\n",
                       GST_BUFFER_PTS(buf), 
                       audio_time_lookup.end()-audio_time_lookup.begin(),
                       audio_time_lookup.size(),
                       audio_time_lookup[audio_time_lookup.end()-audio_time_lookup.begin()-1],
                       audio_time_lookup[audio_time_lookup.size()-1]);
    */
                //printf("audio pts %lu\n", GST_BUFFER_PTS(buf));
            }
        }
        fwrite(&mapinfo.size, 4, 1, source->audiofile);
        if (mapinfo.size > source->max_audio_frame_bytes)
            source->max_audio_frame_bytes = mapinfo.size;
    //    printf("duration %d\n", GST_BUFFER_DURATION(buf));
        fwrite(mapinfo.data, mapinfo.size, 1, source->audiofile);
        fwrite(&flags, sizeof(flags), 1, source->audiofile);
    //    printf("pts %d\n", GST_BUFFER_PTS(buf));
        fwrite(&GST_BUFFER_PTS(buf), sizeof(GstClockTime), 1, source->audiofile);

        struct rtp_client* client = source->client;
        
        while (client)
        {
            if (client->is_live)
            {
            //printf("rtpaudiosrc\n");
                GstState state;
                gst_element_get_state(client->rtpaudiosrc, &state, NULL, 0);
                if (state == GST_STATE_PAUSED || state == GST_STATE_PLAYING)
                {
                    //printf("rtpaudiosrc state %d\n", state);
                    guint64 max = gst_app_src_get_max_bytes((GstAppSrc*)client->rtpaudiosrc);
                    guint64 cur = gst_app_src_get_current_level_bytes((GstAppSrc*)client->rtpaudiosrc);
                    if (cur + mapinfo.size <= max)
                    {
                    	GstBuffer *copybuffer = gst_buffer_copy(buf);
                    	GstClockTime pts;
                    	if (!client->firstsamplesendtime)
                    	{
                        	g_mutex_lock(&client->firstsamplesendtime_mutex);
                        	if (!client->firstsamplesendtime)
                        	{
                        		client->firstsamplesendtime = GST_BUFFER_PTS (copybuffer);
                        	}
                        	g_mutex_unlock(&client->firstsamplesendtime_mutex);
                        }
                    	GST_BUFFER_PTS (copybuffer) -= client->firstsamplesendtime;
                    	GST_BUFFER_DTS (copybuffer) = GST_BUFFER_PTS (copybuffer);
                        gst_app_src_push_buffer((GstAppSrc*)client->rtpaudiosrc, copybuffer);
                    }
                }
            }
            client = client->next;
        }
        gst_buffer_unmap(buf, &mapinfo);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    else
        return GST_FLOW_EOS;
}

GstFlowReturn audio_new_preroll(GstAppSink *sink, gpointer user_data)
{
    struct cam_source *source = (struct cam_source*) user_data;
    if (!stop)
    {
        if (!source->audiofile)
        {
            source->audiofile = fopen(source->audiofilename,"w");
        }
        return GST_FLOW_OK;
    }
    else
        return GST_FLOW_EOS;
}

void video_proc_eos(GstAppSink *sink, gpointer user_data)
{
    printf("video_proc eos\n");
}
GstFlowReturn video_proc_new_buffer(GstAppSink *sink, gpointer user_data)
{
    //printf("video_proc_new_buffer\n");
    if (!stop)
    {
        struct cam_source *source = (struct cam_source*) user_data;
        GstSample* sample;
        while (1)
        {
            sample = gst_app_sink_try_pull_sample(sink, 0);
            if (!sample) break;
            GstBuffer* buf = gst_sample_get_buffer(sample);
            GstClockTime cur_frame_time = (GstClockTime) get_nsec();
            GstVideoInfo vinfo;
            gst_video_info_from_caps (&vinfo, gst_sample_get_caps(sample));
            guint32 video_width = vinfo.width, video_height = vinfo.height;
        //    printf("%d %d\n", vinfo.width, vinfo.height);
        //        printf("%u\n", cur_frame_time - prev_frame_time);
            bool need_new_frame = false;
            if (!source->vplot.prev_frame_time || 
                cur_frame_time - source->vplot.prev_frame_time > 1000000000u) /*1 sec */
            {
                need_new_frame = true;
            }
            if (need_new_frame)
            {
                GstMapInfo mapinfo;
                if (!gst_buffer_map(buf, &mapinfo, GST_MAP_READ))
                {
                    printf("cannot map buffer");
                    return GST_FLOW_OK;
                }
                //printf("video mapinfosize %d\n", mapinfo.size);
                Mat frame(video_height * 3 /2 , video_width, CV_8UC1, (char *)mapinfo.data, mapinfo.size / (video_height * 3 / 2));
                Mat gray;
                cvtColor(frame, gray, COLOR_YUV2GRAY_YV12);
                Mat blur;
                GaussianBlur(gray, blur, Size(11,11), 0);

                if (!source->vplot.prev_frame_time)
                {
                    source->vplot.prev_frame = Mat(blur);
                    source->vplot.prev_frame_time = cur_frame_time;
                }
                else if (cur_frame_time - source->vplot.prev_frame_time > 1000000000u)
                {
                    Mat diff;
                    absdiff(blur, source->vplot.prev_frame, diff);
                    float fdiff = mean(diff)[0];
        //                printf("plot %d diff %f\n", i, fdiff);
                    source->vplot.plot_data.time.push_back(cur_frame_time/1000000000u);
                    source->vplot.plot_data.val.push_back(fdiff);
                    source->vplot.prev_frame = blur;
                    source->vplot.prev_frame_time = cur_frame_time;
                }
                    

                gst_buffer_unmap(buf, &mapinfo);
            }
            
            gst_sample_unref(sample);
        }
        return GST_FLOW_OK;
    }
    else
        return GST_FLOW_EOS;
}

void audio_proc_eos(GstAppSink *sink, gpointer user_data)
{
    printf("audio_proc eos\n");
}
GstFlowReturn audio_proc_new_buffer(GstAppSink *sink, gpointer user_data)
{
//    printf("audio_new_buffer\n");
    GstSample* sample = gst_app_sink_pull_sample(sink);
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo mapinfo;
    if (!gst_buffer_map(buf, &mapinfo, GST_MAP_READ))
    {
        printf("cannot map buffer");
        goto exit_function;
    }
    //printf("audio mapinfosize %d\n", mapinfo.size);
    gst_buffer_unmap(buf, &mapinfo);
    gst_sample_unref(sample);
exit_function:
    return GST_FLOW_OK;
}
/*
void* save_p[50] = {NULL};
unsigned int num_save_p;

static void mtrace_init(void)
{

}
void* (*real_free)(void*) = 0;
unsigned int last_save_p_free;
void free (void* p)
{
	if (last_save_p_free < 50 && p != 0)
		if (p == save_p[last_save_p_free])
			last_save_p_free++;
    if (!real_free)
    {
    	void* handle = dlopen("libc.so.6",RTLD_LAZY);
    	real_free = (void* (*)(void*)) dlsym(handle,"free");
    }
    real_free (p);
}
*/
void videoNeedData (GstAppSrc *src, guint length, gpointer user_data)
{
    if (!stop)
    {
        struct rtp_client *client = (struct rtp_client*) user_data;
        struct cam_source* source = client->source;
        guint64 max = gst_app_src_get_max_bytes(src);
        guint64 sum = 0;

        while(sum + source->videoframesize <= max)
        {
            gpointer buf = g_malloc(source->videoframesize);
            fread(buf, source->videoframesize, 1, client->videofileread);
            GstBuffer *gbuf = gst_buffer_new_wrapped(buf, source->videoframesize);
            GstBufferFlags flag;
            fread(&flag, sizeof(GstBufferFlags), 1, client->videofileread);
            gst_buffer_set_flags(gbuf, flag);
            GstClockTime pts;
            fread(&pts, sizeof(GstClockTime), 1, client->videofileread);
            GST_BUFFER_PTS(gbuf) = pts - client->need_data_first_pts;
    //        printf("pts %u\n", GST_BUFFER_PTS(gbuf));   
            gst_app_src_push_buffer(src, gbuf);
            sum += source->videoframesize;
            if (fread(&source->videoframesize, 4,1,client->videofileread) != 1) 
            {
                printf("eof\n");
                gst_app_src_end_of_stream(src);
                break;
            }
    //        fread(&size, sizeof(gsize),1,videofile);
        }
    }
}

void videoEnoughData(GstAppSrc *src, gpointer user_data)
{
    printf("enough data\n");
}
gboolean videoSeekData(GstAppSrc *src, guint64 offset, gpointer user_data)
{
    printf("seek data\n");
    return false;
}

void audioneedData (GstAppSrc *src, guint length, gpointer user_data)
{
    if (!stop)
    {
        struct rtp_client *client = (struct rtp_client*) user_data;
        struct cam_source* source = client->source;
        guint64 max = gst_app_src_get_max_bytes(src);
        guint64 sum = 0;
        while(sum + source->audioframesize <= max)
        {
            gpointer buf = g_malloc(source->audioframesize);
            fread(buf, source->audioframesize, 1, client->audiofileread);
            GstBuffer *gbuf = gst_buffer_new_wrapped(buf, source->audioframesize);
            GstBufferFlags flag;
            fread(&flag, sizeof(GstBufferFlags), 1, client->audiofileread);
            gst_buffer_set_flags(gbuf, flag);
            GstClockTime pts;
            fread(&pts, sizeof(GstClockTime), 1, client->audiofileread);
            GST_BUFFER_PTS(gbuf) = pts - client->need_data_first_pts;
            //printf("pts %u\n", GST_BUFFER_PTS(gbuf));    
            gst_app_src_push_buffer(src, gbuf);
            sum += source->audioframesize;
            if (fread(&source->audioframesize, 4,1,client->audiofileread) != 1) 
            {
                gst_app_src_end_of_stream(src);
                break;
            }
    //        fread(&size, sizeof(gsize),1,videofile);
        }
    }
}

void audioenoughData(GstAppSrc *src, gpointer user_data)
{
    printf("audio enough data\n");
}
gboolean audioseekData(GstAppSrc *src, guint64 offset, gpointer user_data)
{
    printf("audio seek data\n");
    return false;
}

static GstRTSPFilterResult media_filter (GstRTSPSession * sess, GstRTSPSessionMedia * sessmedia,
    gpointer user_data)
{
  bool* timedout = (bool*) user_data;
  GstRTSPMedia *media = gst_rtsp_session_media_get_media(sessmedia);
  int nstream = gst_rtsp_media_n_streams (media);
  for (int i = 0; i < nstream; i++)
  {
    GstRTSPStreamTransport *transport = gst_rtsp_session_media_get_transport(sessmedia, i);
    if (gst_rtsp_stream_transport_is_timed_out(transport))
    {
      *timedout = true;
    }
  }
  return GST_RTSP_FILTER_KEEP;
}

static GstRTSPFilterResult session_filter (GstRTSPClient * client, GstRTSPSession * session,
    gpointer user_data)
{
  gst_rtsp_session_filter(session, media_filter, user_data);
  return GST_RTSP_FILTER_KEEP;
}
static GstRTSPFilterResult client_filter (GstRTSPServer * server, GstRTSPClient * client,
    gpointer user_data)
{
    bool client_timedout = false;
    char* filter_num = (char*) user_data;
    gst_rtsp_client_session_filter (client, session_filter, &client_timedout);
    if (client_timedout)
    {
        printf("timed out\n");
        filter_num[0]++;
        return GST_RTSP_FILTER_REMOVE;
    }
    else
    {
        filter_num[1]++;
        return GST_RTSP_FILTER_KEEP;
    }
}
gboolean timerFunc(gpointer data)
{
    char filter_num[2] = {0};
    gst_rtsp_server_client_filter (server, client_filter, &filter_num[0]);
    if (filter_num[0])
    {
        GstRTSPSessionPool *pool = gst_rtsp_server_get_session_pool(server);
        gst_rtsp_session_pool_cleanup(pool);
        g_object_unref(pool);
    }
    check_need_new_file();
    return TRUE;
}
static void
media_configure (GstRTSPMediaFactory * factory, GstRTSPMedia * media,
    gpointer user_data)
{
    struct cam_source *source = (struct cam_source*) user_data;
    struct rtp_client* new_client = (struct rtp_client*)calloc(1, sizeof(struct rtp_client));
    new_client->source = source;
    
    printf("media configure\n");
    new_client->is_live = source->http_seek_time == 0;

    add_to_list(&source->client, new_client, &source->client_mutex);
    GstElement *element, *appsrc, *appsink, *rtpvideosrc, *rtpaudiosrc;
    GstCaps *caps;

    /* get the element (bin) used for providing the streams of the media */
    element = gst_rtsp_media_get_element (media);
    /* make sure the data is freed when the media is gone */

    g_object_set_data_full (G_OBJECT (media), "rtsp-extra-data", new_client,
      (GDestroyNotify) media_destroy_cb);
    /* Find the 2 app sources (video / audio), and configure them, connect to the
    * signals to request data */

    /* configure the caps of the video */

    new_client->rtpvideosrc = rtpvideosrc = gst_bin_get_by_name(GST_BIN (element), "videosrc");
    if (source->audioappsink)
        new_client->rtpaudiosrc = rtpaudiosrc = gst_bin_get_by_name(GST_BIN (element), "audiosrc");

    gst_app_src_set_caps((GstAppSrc*)rtpvideosrc, source->videocaps);
    gst_app_src_set_max_bytes((GstAppSrc*)rtpvideosrc, source->max_video_frame_bytes * 10);
    g_object_set((GstAppSrc*)rtpvideosrc, "format", GST_FORMAT_TIME, NULL);

    if (source->audioappsink)
    {
        gst_app_src_set_caps((GstAppSrc*)rtpaudiosrc, source->audiocaps);
        gst_app_src_set_max_bytes((GstAppSrc*)rtpaudiosrc, source->max_audio_frame_bytes * 10);
        g_object_set((GstAppSrc*)rtpaudiosrc, "format", GST_FORMAT_TIME, NULL);
    }    
    if (!new_client->is_live)
    {
        GstAppSrcCallbacks appsrccb = 
        {
            videoNeedData,
            videoEnoughData,
            videoSeekData,
        };
        gst_app_src_set_callbacks((GstAppSrc*)rtpvideosrc, &appsrccb, new_client, NULL);
        if (source->audioappsink)
        {
            GstAppSrcCallbacks audioappsrccb = 
            {
                audioneedData,
                audioenoughData,
                audioseekData,
            };
            gst_app_src_set_callbacks((GstAppSrc*)rtpaudiosrc, &audioappsrccb, new_client, NULL);
            new_client->audiofileread = fopen(source->audiofilename, "r");
            fseek(new_client->audiofileread, source->audio_pos_lookup[source->audio_lookup_index], SEEK_SET);
            fread(&source->audioframesize, 4,1,new_client->audiofileread);
        }
        new_client->videofileread = fopen(source->videofilename, "r");
        if (source->audioappsink)
            new_client->need_data_first_pts = source->video_time_lookup[source->video_lookup_index] < source->audio_time_lookup[source->audio_lookup_index] ? source->video_time_lookup[source->video_lookup_index] : source->audio_time_lookup[source->audio_lookup_index]; 
        else
            new_client->need_data_first_pts = source->video_time_lookup[source->video_lookup_index];
        fseek(new_client->videofileread, source->video_pos_lookup[source->video_lookup_index], SEEK_SET);
        fread(&source->videoframesize, 4,1,new_client->videofileread);
    }

    gst_object_unref (element);

}
void* http_server_thread(void* param);
void* esp32_cam_thread(void* param);
void* pico_cam_thread(void* param);

gboolean auth_check (GstRTSPAuth *auth, GstRTSPContext *ctx, const gchar *check)
{
    if (g_str_equal (check, GST_RTSP_AUTH_CHECK_CONNECT))
    {
        GstRTSPConnection *conn = ctx->conn;
        const char* ip = gst_rtsp_connection_get_ip(conn);
        printf("%s connected\n", ip);
        bool found = false;
        for (int i = 0; i < num_all_source; i++)
        {
            char* http_ip = all_source[i].http_ip;
            if (http_ip)
            {
                if (!strcmp(ip, http_ip))
                {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {printf("ret false\n");return false;}

    }
    else if (g_str_equal (check, GST_RTSP_AUTH_CHECK_MEDIA_FACTORY_ACCESS))
    {
        GstRTSPConnection *conn = ctx->conn;
        const char* ip = gst_rtsp_connection_get_ip(conn);
        GstRTSPMediaFactory *factory = ctx->factory;
        bool found = false;
        int i;
        for (i = 0; i < num_all_source; i++)
        {
            if (factory == all_source[i].factory)
            {
                found = true;
                break;
            }
        }
        if (!found) {printf("ret %d false\n",__LINE__);return false;}
        if (all_source[i].http_ip)
        {
            char* http_ip = all_source[i].http_ip;
            if (!strcmp(ip, http_ip))
            {
                time_t timenow = time(NULL);
                if (timenow - all_source[i].rtsp_enable_time > 3600)
                {
                    printf("over time\n");
                    {printf("ret %d false\n",__LINE__);return false;}
                }
      //          if (!all_source[i].have_client)
       //             all_source[i].have_client = ctx->client;
        //        else if (all_source[i].have_client != ctx->client)
         //           {printf("ret %d false\n",__LINE__);return false;}
            }
            else
                {printf("ret %d false\n",__LINE__);return false;}
//            g_object_set(ctx->client, "drop-backlog", FALSE, NULL);
        }
        else {printf("ret %d false\n",__LINE__);return false;}
    }
    return true;
}
char read_config(FILE* f, char* s, int len)
{
    static char state = 0;
    while(1)
    {
        char* ret = fgets(s, len, f);
        if (ret)
        {
            if (strlen(s) == 1)
            {
                state = 0;
                continue;
            }
            else
            {
                if (s[0] == '#')
                    continue;
                else
                {
                    s[strlen(s)-1] = 0;
                    if (state == 0)
                    {
                        state = 1;
                        return 1; /* new source */
                    }
                    else
                    {
                        return 0; /* extra data */
                    }
                }
            }
        }
        else
            return 3; /* end of file */
    }
}
uint8_t hex_to_byte(char hex)
{
    if (hex >= '0' && hex <= '9')
        return hex-'0';
    else if (hex >= 'A' && hex <= 'Z')
        return hex - 'A' + 10;
    else
        assert(false);
}

uint8_t* str_to_mac(char* mac_str)
{
    uint8_t* mac = (uint8_t*) malloc(6);
    for (int i = 0; i < 6; i++)
    {
        mac[i] = (hex_to_byte(mac_str[i*3]) << 4) | hex_to_byte(mac_str[i*3+1]);
    }
    return mac;
}

extern struct pico_source_info * pico_source;
extern uint32_t num_pico_source;

int set_source_param(char* source_name, char** param_list, int num_param)
{
    int ret = 0;
    uint32_t i = 0;
    while(i < num_param)
    {
        int param_len = strlen(param_list[i]);
        if (strchr(param_list[i], '!'))
            break;
        else if (!memcmp(param_list[i], "pico", 4))
        {
            assert(param_len < 14);
            char pico_name[11];
            pico_name[0] = 0;
            strcpy(pico_name, &param_list[i][4]);
            if (!strcmp(source_name, pico_name))
            {
                if (!pico_source)
                    pico_source = (struct pico_source_info*)malloc(sizeof(struct pico_source_info));
                else
                    pico_source = (struct pico_source_info*)realloc(pico_source, sizeof(struct pico_source_info)*(num_pico_source+1));
                memset(&pico_source[num_pico_source], 0, sizeof(struct pico_source_info));
                pico_source[num_pico_source].recv_if = param_list[i+1];
                pico_source[num_pico_source].cam_ip = param_list[i+2];
                pico_source[num_pico_source].cam_mac = str_to_mac(param_list[i+3]);
                
                ret = 1;
                break;
            }
            i += 4;
        }
    }
    return ret;            
}
int main(int argc, char **argv)
{
    time_t tnow = time(NULL);
    struct tm* tm = localtime(&tnow);
    tm->tm_sec = tm->tm_min = tm->tm_hour = 0;
    today_sec = mktime(tm);

    GError *err = NULL;
    gst_init (&argc, &argv);
    GMainLoop *loop = g_main_loop_new (NULL, FALSE);
    GIOChannel *channel = g_io_channel_unix_new (STDIN_FILENO);
    GError **perror = NULL;
    g_io_channel_set_encoding (channel, NULL, perror);
    g_io_add_watch (channel, G_IO_IN, mycallback, NULL);

    /* create a server instance */
    server = gst_rtsp_server_new ();
    gst_rtsp_server_set_address(server, "0.0.0.0");
    gst_rtsp_server_set_service(server, "8090");
    /* get the mount points for this server, every server has a default object
    * that be used to map uri mount points to media factories */
    mounts = gst_rtsp_server_get_mount_points (server);
    
    struct cam_source *source = NULL;
    int num_source = 0;

    char line[1024];
    
    FILE* fconfig = fopen("pipeline.txt", "r");
    int count_source = 0;
    char ret;
    while(1)
    {
        ret = read_config(fconfig, line, 1024); /* new source name */
        if (ret == 1)
        {
            if (!source)
                source = (struct cam_source*)malloc(sizeof(struct cam_source));
            else
                source = (struct cam_source*)realloc(source, sizeof(struct cam_source)*(count_source+1));
            memset(&source[count_source], 0, sizeof(struct cam_source));
            source[count_source].name = g_strdup(line);
            count_source++;
        }
        else if (ret == 0)
        {
            int cur_source = count_source - 1;
            if (!source[cur_source].param)
                source[cur_source].param = (char**)malloc(sizeof(char*));
            else
                source[cur_source].param = (char**)realloc(source[cur_source].param, sizeof(char*) * (source[cur_source].num_param + 1));
            source[cur_source].param[source[cur_source].num_param] = g_strdup(line);
            source[cur_source].num_param++;
        }
        else if (ret == 3)
            break;
    }
    fclose(fconfig);

    bool pico_source_present = false;
    bool esp_source_present = false;
    
    for (num_source = 0; num_source < count_source; num_source++)
    {
        char* name;
        name = source[num_source].name;

        GstElement* pipeline;
        GError *err = NULL;
        source[num_source].pipeline = pipeline = gst_parse_launch (source[num_source].param[source[num_source].num_param - 1], &err);
        if(pipeline == NULL || err != NULL)
        {
            GST_ERROR("create pipeline failed\n");
            return -1;
        }
        GstElement* appsink, *audioappsink, *audioprocappsink, *videoprocappsink, *jpgsrc,*jpg_audio_src;
        source[num_source].videoappsink = appsink = gst_bin_get_by_name(GST_BIN (pipeline), "videorec");
        GstAppSinkCallbacks appsinkcb = 
        {
            eos,
            new_preroll,
            new_buffer,
            NULL
        };
        gst_app_sink_set_callbacks((GstAppSink*)appsink, &appsinkcb, &source[num_source], NULL);
        source[num_source].audioappsink = audioappsink = gst_bin_get_by_name(GST_BIN (pipeline), "audiorec");
        if (audioappsink)
        {
            GstAppSinkCallbacks audioappsinkcb = 
            {
                audio_eos,
                audio_new_preroll,
                audio_new_buffer,
                NULL
            };
            gst_app_sink_set_callbacks((GstAppSink*)audioappsink, &audioappsinkcb, &source[num_source], NULL);

            source[num_source].audioprocappsink = audioprocappsink = gst_bin_get_by_name(GST_BIN (pipeline), "audioanalysis");
            GstAppSinkCallbacks audioprocappsinkcb = 
            {
                audio_proc_eos,
                NULL,
                audio_proc_new_buffer,
                NULL
            };
            gst_app_sink_set_callbacks((GstAppSink*)audioprocappsink, &audioprocappsinkcb, &source[num_source], NULL);
        }
        
        source[num_source].videoprocappsink = videoprocappsink = gst_bin_get_by_name(GST_BIN (pipeline), "videoanalysis");
        GstAppSinkCallbacks videoprocappsinkcb = 
        {
            video_proc_eos,
            NULL,
            video_proc_new_buffer,
            NULL
        };
        gst_app_sink_set_callbacks((GstAppSink*)videoprocappsink, &videoprocappsinkcb, &source[num_source], NULL);
        source[num_source].video_time_lookup = boost::circular_buffer<GstClockTime> (lookup_size);
        source[num_source].video_pos_lookup = boost::circular_buffer<guint64> (lookup_size);
        if (audioappsink)
        {
            source[num_source].audio_time_lookup = boost::circular_buffer<GstClockTime> (lookup_size);
            source[num_source].audio_pos_lookup = boost::circular_buffer<guint64> (lookup_size);
        }
        
        GstIterator * pipeline_iter = gst_bin_iterate_elements(GST_BIN (pipeline));
        GValue iter_elem = G_VALUE_INIT;
        bool iter_done = false;
        while (!iter_done)
        {
            switch (gst_iterator_next (pipeline_iter, &iter_elem)) 
            {
                case GST_ITERATOR_OK:
                {
                    GstElement *elem = GST_ELEMENT(g_value_get_object (&iter_elem));
                    char* elem_name = gst_element_get_name(elem);
                    if (elem_name)
                    {
                        int elem_name_len = strlen(elem_name);
                        if (elem_name_len >= 6)
                        {
                            if (!memcmp(elem_name, "jpgsrc", 6))
                            {
                                assert(elem_name_len < 16);
                                char source_name[11];
                                source_name[0] = 0;
                                strcpy(source_name, &elem_name[6]);
                                int source_type = set_source_param(source_name, source[num_source].param, source[num_source].num_param);
                                const char* strAudioRate;
                                const char* audioEndian;
                                if (source_type == 0)
                                {
                                    strAudioRate = "16000";
                                    audioEndian = "LE";
                                }
                                else if (source_type == 1)
                                {
                                    strAudioRate = "31250";
                                    audioEndian = "BE";
                                }
                                char tempStr[100];
                                tempStr[0] = 0;
                                sprintf(tempStr, "jpgsrc%s", source_name);
                                jpgsrc = gst_bin_get_by_name(GST_BIN (pipeline), tempStr);
                                if (jpgsrc)
                                {
                                    if (source_type == 0)
                                        source[num_source].jpgsrc = jpgsrc;
                                    else if (source_type == 1)
                                        pico_source[num_pico_source].video_src = jpgsrc;
                                        
                                    GstCaps* jpgcap = gst_caps_from_string ("image/jpeg");
                                    gst_app_src_set_caps((GstAppSrc*)jpgsrc, jpgcap);
                                    g_object_set(jpgsrc, "format", GST_FORMAT_TIME, NULL);
                                }
                                sprintf(tempStr, "jpgaudiosrc%s", source_name);
                                jpg_audio_src = gst_bin_get_by_name(GST_BIN (pipeline), tempStr);

                                if (jpg_audio_src)
                                {
                                    if (source_type == 0)
                                        source[num_source].jpg_audio_src = jpg_audio_src;
                                    else if (source_type == 1)
                                        pico_source[num_pico_source].audio_src = jpg_audio_src;
                                        
                                    tempStr[0] = 0;
                                    sprintf(tempStr, "audio/x-raw,format=S24%s,channels=1,rate=%s,layout=interleaved", audioEndian, strAudioRate);
                                    GstCaps* jpg_audio_src_cap = gst_caps_from_string (tempStr);
                                    gst_app_src_set_caps((GstAppSrc*)jpg_audio_src, jpg_audio_src_cap);
                                    g_object_set(jpg_audio_src, "format", GST_FORMAT_TIME, NULL);
                                }
                                
                                if (source_type == 1)
                                {
                                    pico_source[num_pico_source].source = &source[num_source];
                                    num_pico_source++;
                                    pico_source_present = true;
                                }
                                else
                                    esp_source_present = true;
                                    
                            }
                        }
                        g_free(elem_name);
                    }
                    
                    g_value_reset (&iter_elem);
                }
                    break;
                case GST_ITERATOR_RESYNC:
                case GST_ITERATOR_ERROR:
                case GST_ITERATOR_DONE:
                    iter_done = TRUE;
                    break;
            }
        }
        g_value_unset (&iter_elem);
        gst_iterator_free (pipeline_iter);

        bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
        gst_bus_add_signal_watch (bus);
        g_signal_connect (G_OBJECT (bus), "message", G_CALLBACK (message_cb), &source[num_source]);
        gst_object_unref (GST_OBJECT (bus));           
        source[num_source].vplot.plot_data.time = boost::circular_buffer<guint32> (VIDEO_PLOT_NUM_POINT);
        source[num_source].vplot.plot_data.val = boost::circular_buffer<float> (VIDEO_PLOT_NUM_POINT);
        
        GstRTSPMediaFactory *factory;
        /* make a media factory for a test stream. The default media factory can use
        * gst-launch syntax to create pipelines.
        * any launch line works as long as it contains elements named pay%d. Each
        * element with pay%d names will be a stream */
        source[num_source].factory = factory = gst_rtsp_media_factory_new ();

        /* notify when our media is ready, This is called whenever someone asks for
        * the media and a new pipeline with our appsrc is created */
        g_signal_connect (factory, "media-configure", (GCallback) media_configure, &source[num_source]);
        /* attach the test factory to the /test url */
        gst_rtsp_media_factory_set_do_retransmission(factory, true);

        assign_file_name(&source[num_source]);
        
        gst_element_set_state (source[num_source].pipeline, GST_STATE_PLAYING);
        
    }
    all_source = source;
    num_all_source = num_source;
    g_timeout_add_seconds(2, timerFunc, NULL);
    
    pthread_t threadId;
    int thread_err = pthread_create(&threadId, NULL, &http_server_thread, NULL);
    if (thread_err)
    {
        printf("error create thread\n");
        exit(thread_err);
    }
    
    if (esp_source_present)
    {
        thread_err = pthread_create(&threadId, NULL, &esp32_cam_thread, NULL);
        if (thread_err)
        {
            printf("error create thread\n");
            exit(thread_err);
        }
    }
    if (pico_source_present)
    {
        thread_err = pthread_create(&threadId, NULL, &pico_cam_thread, NULL);
        if (thread_err)
        {
            printf("error create thread\n");
            exit(thread_err);
        }
    }
    GstRTSPAuth * auth = gst_rtsp_auth_new ();
    GstRTSPAuthClass *klass = GST_RTSP_AUTH_GET_CLASS (auth);
    klass->authenticate = NULL;
    klass->generate_authenticate_header = NULL;
    klass->accept_certificate = NULL;
    klass->check = auth_check;
    gst_rtsp_server_set_auth (server, auth);
    g_object_unref (auth);
    /* attach the server to the default maincontext */
    gst_rtsp_server_attach (server, NULL);

    /* start serving */
    g_print ("stream ready\n");

    time_t temp_time = time(NULL);
    struct tm *ptm = localtime(&temp_time);
    prev_time_need_new_file = ptm->tm_mday;
    //prev_time_need_new_file = ptm->tm_min + 3 >= 60 ? ptm->tm_min + 3 - 60 : ptm->tm_min + 3;
    g_main_loop_run (loop);
    return 0;
}

