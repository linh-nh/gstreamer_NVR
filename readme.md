# Gstreamer Network Video Recorder (NVR)
A simple NVR based on gstreamer that may suit your need.

## Features
- Record network video, rtsp source is the most useful for people. Other supported sources are diy by myself: esp32, pico(rp2040).
- Auto retry if rtsp source fail
- Graph to record change in video image, between each second
- Restream: live or replay at a specific time
- Simple (almost none) http interface
- Export recorded stream to mkv

## Prequisite
- Linux
- Gstreamer
- OpenCV
- libpcap
- Your ability to construct gstreamer pipeline

## Compile

Compile NVR:
```
g++ gst_app.c gst_app_http_server.c plot.c esp_cam_src.c pico_cam_src.c -o gst_app -g `pkg-config --cflags --libs gstreamer-1.0 gtk+-3.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0 opencv4 gstreamer-rtsp-1.0 gstreamer-video-1.0` -lpcap
```
Compile converter, to convert recorded stream to mkv:
```
g++ convert_to_mkv.c -o convert_to_mkv -g `pkg-config --cflags --libs gstreamer-1.0 gtk+-3.0 gstreamer-app-1.0`
```
## Usage

### NVR
Run ```./gst_app```

NVR program saves stream in its own raw format, in current directory. It also need pipeline.txt in current directory. Example of this file is included. In pipeline.txt, there is a pipeline text for each source you want to use. A pipeline need to have some required elements. For example, this one:
```
uridecodebin uri=rtsp://username:password@192.168.1.2/ch0_0.h264 name=src ! tee name=teevideo ! clockoverlay ! queue ! vaapipostproc ! vaapivp8enc ! appsink name=videorec teevideo. ! queue ! appsink name=videoanalysis src. ! tee name=teeaudio ! queue ! audioconvert ! opusenc ! appsink name=audiorec teeaudio. ! queue ! appsink name=audioanalysis
```
Required elements are: tee, queue, appsink name=videorec, appsink name=videoanalysis, appsink name=audiorec, appsink name=audioanalysis


If your source is video only, you can omit: appsink name=audiorec, appsink name=audioanalysis


Other elements you can change to suit your need: clockoverlay, vaapipostproc, vaapivp8enc, audioconvert, opusenc


uridecodebin is simpler to use but you may consider to manually use rtspsrc, like this:
```
rtspsrc location=rtsp://username:password@192.168.1.2/ch0_0.h264 is-live=true latency=100 name=src ! rtph264depay ! avdec_h264 ! tee name=teevideo ! clockoverlay ! queue ! vaapipostproc ! vaapivp8enc ! appsink name=videorec teevideo. ! queue ! appsink name=videoanalysis src. ! rtpL16depay ! tee name=teeaudio ! queue ! audioconvert ! opusenc ! appsink name=audiorec teeaudio. ! queue ! appsink name=audioanalysis
```
With this you can have lower latency. Also I have met a wifi camera that needed force-non-compliant-url=true in rtspsrc to work. The disadvantage is that you have to figure out the correct rtpXXXdepay elements.

### Web interface
Before viewing, a source need to be "requested" first from web interface. For example if your source is named "livingroom", open web browser and go to:

```http://127.0.0.1:8089/livingroom/live```

This will allow access to live source named "livingroom"


To replay at a specific time (for example 13:01:30)

```http://127.0.0.1:8089/livingroom/130130```

To view the change graph:

```http://127.0.0.1:8089/livingroom/all```

Using 127.0.0.1 is ok if you are using server PC to view the restream. Otherwise it is IP address of the server.

If all is good, a letter "A" is returned. If something's wrong, for example the requested replay time is outside of recorded range, or source name is not correct, a letter "B" is returned.

### Restream viewing:
After you have requested access to source, be it live or replay, use VLC or any video player you want, open the url:

```rtsp://127.0.0.1:8090/livingroom/test```

When a source is requested through web interface, the http client IP address and URL will be recorded. And the IP address of rtsp client that view the restream must match the http client IP address. If multiple clients access http server at the same time, assume that the URL is correct and "A" is returned, only the last client IP address and URL will be recorded.

Requested URL is only valid for one hour. After this, if a rtsp client wants to open a new connection, it has to be requested again.

HTTP request condition is only used to check when creating new rtsp connection, existing rtsp connections are not affected by http request.

### Converter
```convert_to_mkv path_to_raw_video path_to_raw_audio path_to_output_mkv```

Example:
```
convert_to_mkv vid_livingroom_2025_01_03 aud_livingroom_2025_01_03 livingroom.mkv
```

Converter can be run at anytime, even if NVR is writing to raw files. So if you want to have more advance control over replaying, you should export to mkv.

## Issues
- NVR terminate by itself at the end of day, local time. So to run it continuously, use while like this:
```while true; do 'gst_app';sleep 3; done```

Some seconds at the beginning of new day will be missed.
- Because only the last IP address of http access is recorded as I explained above, depend on how frequent the users view the restream, this program may not be suitable if too many people using it at the same time.
- Server port, http and rtsp, is hard coded. Rtsp port is 8090 and http is 8089. To change it, search for "8090" in gst_app.c and "8089" in gst_app_http_server.c
- There is audioanalysis appsink element, but it is stub and doesn't do anything yet
- Converter assume that vp8 and opus is used in NVR pipeline. If this is not the case in your pipeline, you need to search for "vp8" and "opus" in convert_to_mkv.c and change it.
- It's most likely that you will use rtsp source, because of that you won't need libpcap. But the part of code that use libpcap is not isolated, so bear with it and install libpcap for now.

## Donate
If you find it useful, consider donating
https://paypal.me/34h9wg
