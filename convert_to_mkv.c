/*
g++ convert_to_mkv.c -o convert_to_mkv -g `pkg-config --cflags --libs gstreamer-1.0 gtk+-3.0 gstreamer-app-1.0`
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

bool audio_done = false;
bool video_done = false;

GstClockTime first_pts;

static gboolean message_cb (GstBus * bus, GstMessage * message, gpointer user_data)
{
        GError *err = NULL;
       gchar *dbg_info = NULL;
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_EOS:
      g_print ("Got EOS\n");
      if (audio_done && video_done) exit(0);
      break;
#if 0
    case GST_MESSAGE_ERROR:

       gst_message_parse_error (message, &err, &dbg_info);
       g_printerr ("ERROR from element %s: %s\n",
           GST_OBJECT_NAME (message->src), err->message);
       g_printerr ("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
#endif
     default:
#if 0
        const GstStructure *gs = gst_message_get_structure (message);
        if (gs != NULL)
            printf("%s\n",   gst_structure_to_string(gs));
        else
            printf("NULL gs\n");
#endif
        break;
  }

  return TRUE;
}


FILE* audiofileread, *videofileread;
guint64 videoframesize;


void videoNeedData (GstAppSrc *src, guint length, gpointer user_data)
{
    static GstClockTime prev_pts = 0;
    guint64 max = gst_app_src_get_max_bytes(src);
    guint64 sum = 0;
    if (videoframesize > max)
        printf("max video app src %lu videoframesize %lu\n", max, videoframesize);
    while(sum + videoframesize <= max)
    {
        //printf("videframesize %lu\n", videoframesize);
        gpointer buf = g_malloc(videoframesize);
        if (fread(buf, videoframesize, 1, videofileread) != 1)
            goto eos;
        GstBuffer *gbuf = gst_buffer_new_wrapped(buf, videoframesize);
        GstBufferFlags flag;
        if (fread(&flag, sizeof(GstBufferFlags), 1, videofileread) != 1)
            goto eos;
        gst_buffer_set_flags(gbuf, flag);
        GstClockTime pts;
        if (fread(&pts, sizeof(GstClockTime), 1, videofileread) != 1)
            goto eos;
        if (pts < prev_pts)
        {
            printf("video pts < prev_pts\n");
            exit(1);
        }
        prev_pts = pts;
        GST_BUFFER_PTS(gbuf) = pts - first_pts;
        //printf("pts %u\n", GST_BUFFER_PTS(gbuf));   
        gst_app_src_push_buffer(src, gbuf);
        sum += videoframesize;
        if (fread(&videoframesize, 4,1,videofileread) != 1) 
            goto eos;
    }
    //printf("videoNeedData end\n");
    return;
eos:
    printf("video eos\n");
    gst_app_src_end_of_stream(src);
    video_done = true;
//    if (audio_done) exit(0);
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
guint64 audioframesize = 0;

void audioneedData (GstAppSrc *src, guint length, gpointer user_data)
{
    static GstClockTime prev_pts = 0;
    guint64 max = gst_app_src_get_max_bytes(src);
    guint64 sum = 0;
    if (audioframesize > max)
        printf("max audio app src %lu audioframesize %lu\n", max, audioframesize);
    //printf("aud %u %u\n", audioframesize, max);
    while(sum + audioframesize <= max)
    {
        gpointer buf = g_malloc(audioframesize);
        //printf("audioframesize %lu\n", audioframesize);
        if (fread(buf, audioframesize, 1, audiofileread) != 1)
            goto eos;
        GstBuffer *gbuf = gst_buffer_new_wrapped(buf, audioframesize);
        GstBufferFlags flag;
        if (fread(&flag, sizeof(GstBufferFlags), 1, audiofileread) != 1)
            goto eos;
        gst_buffer_set_flags(gbuf, flag);
        GstClockTime pts;
        if (fread(&pts, sizeof(GstClockTime), 1, audiofileread) != 1)
            goto eos;
        if (pts < prev_pts)
        {
            printf("audio pts < prev_pts\n");
            exit(1);
        }
        prev_pts = pts;
        GST_BUFFER_PTS(gbuf) = pts - first_pts;
        //printf("pts %u\n", GST_BUFFER_PTS(gbuf));    
        gst_app_src_push_buffer(src, gbuf);
        sum += audioframesize;
        if (fread(&audioframesize, 4,1,audiofileread) != 1) 
            goto eos;
    }
    //printf("audioneedData end\n");
    return;
eos:
    printf("audio eos\n");
    gst_app_src_end_of_stream(src);
    audio_done = true;
    //if (video_done) exit(0);
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

int main(int argc, char **argv)
{
    GError *err = NULL;
    gst_init (&argc, &argv);
    char launch_cmd[1024];
    videofileread = fopen(argv[1], "r");
    audiofileread = fopen(argv[2], "r");
    //printf("%s %s\n", argv[1], argv[2]);
    fread(&audioframesize, 4,1,audiofileread);
    fread(&videoframesize, 4,1,videofileread);
    GstClockTime audiofirstpts, videofirstpts;
    
    fseek(audiofileread, audioframesize + sizeof(GstBufferFlags), SEEK_CUR);
    fseek(videofileread, videoframesize + sizeof(GstBufferFlags), SEEK_CUR);
    fread(&audiofirstpts, sizeof(GstClockTime), 1, audiofileread);
    fread(&videofirstpts, sizeof(GstClockTime), 1, videofileread);
    
    first_pts = audiofirstpts < videofirstpts ? audiofirstpts : videofirstpts;
    
    fclose(videofileread);
    fclose(audiofileread);
    
    videofileread = fopen(argv[1], "r");
    audiofileread = fopen(argv[2], "r");
    //printf("%s %s\n", argv[1], argv[2]);
    fread(&audioframesize, 4,1,audiofileread);
    fread(&videoframesize, 4,1,videofileread);
    
    printf("start videoframesize %lu\n", videoframesize);
    printf("start audioframesize %lu\n", audioframesize);
    printf("start video pts %lu\n", videofirstpts);
    printf("start audio pts %lu\n", audiofirstpts);
    
    sprintf(launch_cmd, "appsrc name=videosrc ! video/x-vp8,width=640,height=480 ! matroskamux name=mux ! filesink location=%s appsrc name=audiosrc ! audio/x-opus,channels=1,rate=24000 ! mux.", argv[3]);
    GstElement *pipeline = gst_parse_launch (launch_cmd, &err);
    GstElement* videosrc = gst_bin_get_by_name(GST_BIN (pipeline), "videosrc");
    gst_app_src_set_max_bytes((GstAppSrc*)videosrc, videoframesize * 100);
    GstElement* audiosrc = gst_bin_get_by_name(GST_BIN (pipeline), "audiosrc");
    gst_app_src_set_max_bytes((GstAppSrc*)audiosrc, audioframesize * 100);
    GstCaps *caps = gst_caps_new_simple ("video/x-vp8", NULL);
    gst_app_src_set_caps((GstAppSrc*)videosrc, caps);
    gst_caps_unref (caps);
    g_object_set((GstAppSrc*)videosrc, "format", GST_FORMAT_TIME, NULL);

    caps = gst_caps_new_simple ("audio/x-opus", 
                              "channel-mapping-family", G_TYPE_INT, 0,
                              NULL);
    gst_app_src_set_caps((GstAppSrc*)audiosrc, caps);
    gst_caps_unref (caps);
    g_object_set((GstAppSrc*)audiosrc, "format", GST_FORMAT_TIME, NULL);
    GstAppSrcCallbacks appsrccb = 
    {
        videoNeedData,
        videoEnoughData,
        videoSeekData,
    };
    gst_app_src_set_callbacks((GstAppSrc*)videosrc, &appsrccb, NULL, (GDestroyNotify) g_free);
    
    GstAppSrcCallbacks audioappsrccb = 
    {
        audioneedData,
        audioenoughData,
        audioseekData,
    };
    gst_app_src_set_callbacks((GstAppSrc*)audiosrc, &audioappsrccb, NULL, (GDestroyNotify) g_free);

    GstBus *bus;
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    gst_bus_add_signal_watch (bus);
    g_signal_connect (G_OBJECT (bus), "message", G_CALLBACK (message_cb), NULL);
    gst_object_unref (GST_OBJECT (bus));   
            
    gst_element_set_state (pipeline, GST_STATE_PLAYING);
    GMainLoop *loop = g_main_loop_new (NULL, FALSE);
    g_main_loop_run (loop);
    return 0;
}
