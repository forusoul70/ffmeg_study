#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>

#include <SDL.h>
#include <SDL_thread.h>

#include <stdio.h>
#include <assert.h>

#include <unistd.h>

#define SDL_AUDIO_BUFFER_SIZE 4096
#define MAX_AUDIO_FRAME_SIZE 192000

#define VIDEO_PICTURE_QUEUE_SIZE 1
#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_CREATE_SURFACE (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

typedef struct PacketList {
    AVPacket *cur;
    AVPacket *next;
} PacketList;

typedef struct PacketQueue {
  PacketList *first_pl, *last_pl;
  int nb_packets;
  int size; // refer to a bytes size that we get from packetsize
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;

typedef struct Bmp {
    uint8_t *yPlane, *uPlane, *vPlane;
	size_t yPlaneSz, uvPlaneSz;
    SDL_mutex *mutex;
} Bmp;

typedef struct VideoPicture {
    Bmp *bmp;
    int width;
    int height;
    int allocated;
} VideoPicture;

typedef struct VideoState {
    AVFormatContext* pFormatCtx;
    int videoStream;
    int audioStream;
    AVStream *audio_st;
    AVCodecContext *audio_ctx;
    PacketQueue audioq;
    uint8_t audio_buf[MAX_AUDIO_FRAME_SIZE * 3 / 2];
    unsigned int audio_buf_size;
    unsigned int audio_buf_index;
    AVFrame audio_frame;
    AVPacket audio_pkt;
    uint8_t *audio_pkt_data;
    int audio_pkt_size;
    AVStream *video_st;
    AVCodecContext *video_ctx;
    PacketQueue videoq;
    struct SwsContext *sws_ctx;

    VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int pictq_size;
    int pictq_rindex;
    int pictq_windex;
    SDL_mutex *pictq_mutex;
    SDL_cond *pictq_cond;
    SDL_Thread *parse_tid;
    SDL_Thread *video_tid;

    char filename[1024];
    int quit;
} VideoState;

SDL_Surface *screen;
SDL_mutex *screen_mutex;
SDL_Renderer *renderer;
SDL_Texture *texture;

// Since we only have one decoding thread. the Big Struct can be global in case we need it.
VideoState *global_video_state; 

void saveFrame(AVFrame *pFrame, int width, int height, int iFrame);
void packet_queue_init(PacketQueue *q);
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
static int packet_queue_get(PacketQueue *q, AVPacket **pkt, int block);
int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size);
void audio_callback(void *userdata, Uint8 *stream, int len);
static void schedule_refresh(VideoState *is, int delay);
int decode_thread(void *arg);
int stream_component_open(VideoState *is, int stream_index);
int video_thread(void *arg);
int queue_picture(VideoState *is, AVFrame *pFrame);
void alloc_picture(void *userdata);
void video_refresh_timer(void *userdata);
void video_display(VideoState *is);
void create_sdl_surface(const int width, const int height);
void show_packet(VideoState *is, AVPacket* packet);
static int audio_resampling(AVCodecContext *audio_decode_ctx,
                            AVFrame *audio_decode_frame,
                            enum AVSampleFormat out_sample_fmt,
                            int out_channels,
                            int out_sample_rate,
                            uint8_t *out_buf);

int main(int args, char *argv[]) {
    SDL_Event event;
    VideoState *is = av_mallocz(sizeof(VideoState)); // zeros

    if (args < 2) {
        fprintf(stderr, "Usage: test <file>\n");
        exit(1);
    }

    // Register all formats and codecs
    av_register_all();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
	    fprintf(stderr, "Cloud not intitialized SDL - %s\n", SDL_GetError());
	    exit(1);
    }
    
    screen_mutex = SDL_CreateMutex();    
    av_strlcpy(is->filename, argv[1], argv[1], sizeof(is->filename));
    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();
    schedule_refresh(is, 40);

    is->parse_tid = SDL_CreateThread(decode_thread, "decode_thread", is);
    if (is->parse_tid == 0) {
        av_free(is);
        return -1;
    }

    for (;;) {        
        SDL_WaitEvent(&event);
        AVCodecContext *codec = NULL;
        switch (event.type) {
        case FF_CREATE_SURFACE:
            codec = (AVCodecContext*)event.user.data1;            
            create_sdl_surface(codec->width, codec->height);
            break;
        case FF_QUIT_EVENT:
        case SDL_QUIT:
            is->quit = 1;
            SDL_Quit();
            return 0;
        case FF_REFRESH_EVENT:
            video_refresh_timer(event.user.data1);
            break;
        defalut:
            break;
        }        
    }
    return 0;
}

void create_sdl_surface(const int width, const int height) {
    fprintf(stderr, "create sdl surface : %d x %d \n", width, height);
    // create display
    screen = SDL_CreateWindow("Main display", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
                             width,height, 0);
    if (screen == NULL) {
        fprintf(stderr, "SDL : could not set video mode - exiting\n");
        exit(1);
    }
    renderer = SDL_CreateRenderer(screen, -1, 0);
    if (renderer == NULL) {
           fprintf(stderr, "SDL : could not create renderer - exiting\n");
            exit(1);
    }
    // Allocate a place to put our YUV image on that screen
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (texture == NULL) {
        fprintf(stderr, "SDL: could not create texture - exiting\n");
        exit(1);
    }        
}

void saveFrame(AVFrame *pFrame, int width, int height, int iFrame) {
    FILE *pFile;
    char szFileName[32];

    // open file
    sprintf(szFileName, "frame%d.ppm", iFrame);
    pFile = fopen(szFileName, "wb");
    if (pFile == NULL) {
        fprintf(stderr, "Failed to open file");
        return;
    }          

    // Write header
    fprintf(pFile, "P6\n%d %d\n255\n", width, height);

    // Write pixel data
    for (int y = 0; y < height; y++) {
        fwrite(pFrame->data[0] + y*pFrame->linesize[0], 1, width*3, pFile);        
    }

    fclose(pFile);
}

void packet_queue_init(PacketQueue* q) {
    memset(q, 0, sizeof(PacketQueue));
    q->first_pl = NULL;
    q->last_pl = NULL;
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

    PacketList *pl = av_malloc(sizeof(PacketList));
    memset(pl, 0, sizeof(PacketList));
    
    if (pl == NULL) {
        return -1;
    }
    pl->cur = pkt;
    pl->next = NULL;
    
    SDL_LockMutex(q->mutex);
    if (q->last_pl == NULL) {
        q->first_pl = pl;        
    } else {
        q->last_pl->next = pl;
    }

    q->last_pl = pl;
    q->nb_packets++;
    q->size += pl->cur->size;
    SDL_CondSignal(q->cond); // send singnal to our get function(if it's waiting)

    SDL_UnlockMutex(q->mutex);
    return 0;
}


static int packet_queue_get(PacketQueue *q, AVPacket **pkt, int block) {
    PacketList *pl = NULL;
    int ret;

    SDL_LockMutex(q->mutex);
    for (;;) {        
        if (global_video_state->quit) {
            ret = -1;
            break;
        }
        pl = q->first_pl;
        if (pl != NULL) {
            q->first_pl = pl->next;
            if (q->first_pl == NULL) {
                q->last_pl = NULL;
            }
            q->nb_packets--;
            q->size -= pl->cur->size;
            *pkt = pl->cur;
            av_free(pl);
            ret = 1;
            break;
        } else if (block == 0) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

/*
* stream is the buffer we will be writing audio data to
* len is size of th buffer
*/
void audio_callback(void *userdata, uint8_t *stream, int len) {    
    VideoState *is = (VideoState*)userdata;
    int len1, audio_size;

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
            // We hav already sent all our data; get more
            audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf));
            if (audio_size < 0) {
                // If error. output silence
                is->audio_buf_size = 1024;
                memset(is->audio_buf, 0, is->audio_buf_size);                
            } else {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        memcpy(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

int audio_decode_frame(VideoState *is, uint8_t* audio_buf, int buf_size) {
    int len1, data_size = 0;

    AVPacket *packet = NULL;
    for (;;) {
        while (is->audio_pkt_size > 0 && packet != NULL) {            
            int got_frame = 0;
            // sister function avcodec_decode_vidoe()
            // expect int this case, a pakeet might have more than one frame.
            // SDL gives 8 bits int buffer
            // ffmpeg gives 16bits int buffer
            // len1 is how much of the packet we've used
            // data_size is amount of raw data returned
            len1 = avcodec_decode_audio4(is->audio_ctx, &is->audio_frame, &got_frame, packet);
            if (len1 < 0) {
                // if error, skip frame
                is->audio_pkt_size = 0;
                break;
            }
            is->audio_pkt_data += len1;
            is->audio_pkt_size -= len1;
            data_size = 0;
            if (got_frame) {
                 data_size = audio_resampling(is->audio_ctx, &is->audio_frame,
                                              AV_SAMPLE_FMT_S16, is->audio_frame.channels,
                                              is->audio_frame.sample_rate, audio_buf);
                 assert(data_size <= buf_size);
                /*
                data_size = av_samples_get_buffer_size(NULL, is->audio_ctx->channels,
                                                       is->audio_frame.nb_samples,
                                                       is->audio_ctx->sample_fmt, 1);
                assert(data_size <= buf_size);
                memcpy(audio_buf, is->audio_frame.data[0], data_size);
                */
            }
            is->audio_pkt_data += len1;
            is->audio_pkt_size -= len1;
            
            if (data_size <= 0) {
                // No data yet, get more frames
                continue;
            }
            // We hava data, return it and come back for more later
            return data_size;
        }
        if (packet != NULL && packet->data) {
            av_free_packet(packet);
            packet = NULL;
        }

        if (is->quit) {
            return -1;
        }

        if (packet_queue_get(&is->audioq, &packet, 1) < 0) {
            return -1;
        }
        is->audio_pkt_data = packet->data;
        is->audio_pkt_size = packet->size;
    }   
}

static uint32_t sdl_refresh_timer_cb(uint32_t interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; // 0 means stop timer
}

/*
 * schedule a video refresh in 'delay' ms
 */
static void schedule_refresh(VideoState* is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

int decode_thread(void *arg) {
    VideoState *is = (VideoState*)arg;
    AVFormatContext *pFormatCtx;
    int videoStream = -1;
    int audioStream = -1;

    is->videoStream = -1;
    is->audioStream = -1;

    global_video_state = is;

    // open video file
    if (avformat_open_input(&pFormatCtx, is->filename, NULL, NULL) != 0) {
        fprintf(stderr, "Failed to open video file : %s", is->filename);
        return -1;
    }
    is->pFormatCtx = pFormatCtx;

    // Retrieve stream information
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        fprintf(stderr, "Failed to find stream info : %s", is->filename);
        return -1;
    }
    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, is->filename, 0);

    // Find video stream
    for (int i=0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream = i;           
        }
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0) {
            audioStream = i;
        }
    }
    
    if (videoStream == -1) {
        fprintf(stderr, "Failed to find video stream");
        return -1;
    }
    if (audioStream == -1) {
        fprintf(stderr, "Failed to find audio stream");
        return -1;
    }

    stream_component_open(is, audioStream);
    stream_component_open(is, videoStream);

    // main decode loop
    for (;;) {
        if (is->quit) {
            break;
        }
        // seek stuff gose here        
        if (is->audioq.size > MAX_AUDIOQ_SIZE ||
            is->videoq.size > MAX_VIDEOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }        

        // new packet
        AVPacket *packet = av_malloc(sizeof(AVPacket));
        av_init_packet(packet);
        if (av_read_frame(pFormatCtx, packet) < 0) {
            if (is->pFormatCtx->pb->error = 0) {
                SDL_Delay(100); // no error, wait fot user input
                continue;
            } else {
                break;
            }
        }

        // Is this a packet from the video stream?
        if (packet->stream_index == is->videoStream) {
            packet_queue_put(&is->videoq, packet);
        } else if (packet->stream_index == is->audioStream) {
            packet_queue_put(&is->audioq, packet);
        } else {
            av_free_packet(packet);
        }        
    }

    // all done, wait fot it
    while (!is->quit) {
        SDL_Delay(100);
    }

 fail:
    if (1) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    return 0;
}

int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx = NULL;
    SDL_AudioSpec wanted_spec, spec;

    if (stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }

    AVCodec* codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codecpar->codec_id);
    if (codec == 0) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(codecCtx, pFormatCtx->streams[stream_index]->codecpar) != 0) {
        fprintf(stderr, "Failed to copy codec context");
        return -1;     }

    if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
         wanted_spec.freq = codecCtx->sample_rate;
         wanted_spec.format = AUDIO_S16SYS; // singed 16bits, endian-order will depend on the system.
         wanted_spec.channels = codecCtx->channels; // Number of audio channel
         wanted_spec.silence = 0; // Silence ? Since audio is signed, 0 is of cource the usual value.
         wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
         wanted_spec.callback = audio_callback;
         wanted_spec.userdata = is; // SDL pass this void pointer to callback

         if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
             fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
             return -1;
         }

         // debug
         fprintf(stderr, "Opened audio info\n");
         fprintf(stderr, "freq %d\n", spec.freq);
         fprintf(stderr, "format %d\n", spec.format);
         fprintf(stderr, "channels = %d\n", spec.channels);
        
    }
    // open codec
    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
         fprintf(stderr, "Failed to open codec");
         return -1;
    }

    SDL_Event createSurfaceEvent;
    switch (codecCtx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audioStream = stream_index;
        is->audio_st = pFormatCtx->streams[stream_index];
        is->audio_ctx = codecCtx;
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;
        memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
        packet_queue_init(&is->audioq);
        SDL_PauseAudio(0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        // send event to create sdl surface
        createSurfaceEvent.type = FF_CREATE_SURFACE;
        createSurfaceEvent.user.data1 = codecCtx;      
        SDL_PushEvent(&createSurfaceEvent);        
         
        is->videoStream = stream_index;
        is->video_st = pFormatCtx->streams[stream_index];
        is->video_ctx = codecCtx;
        packet_queue_init(&is->videoq);
        is->video_tid = SDL_CreateThread(video_thread, "video_thread", is);
        is->sws_ctx = sws_getContext(codecCtx->width, codecCtx->height, codecCtx->pix_fmt, 
                           codecCtx->width, codecCtx->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
						   NULL, NULL, NULL);
        break;
    default:
        break;
    }
}

int video_thread(void *arg) {
    VideoState *is = (VideoState*)arg;
    AVPacket *packet = NULL;
    int frameFinished;
    AVFrame *pFrame;
    
    pFrame = av_frame_alloc();

    for (;;) {
        AVPacket *packet = NULL;
        if (packet_queue_get(&is->videoq, &packet, 1) < 0) {
            // means we quit getting packets
            break;
        }
        
        //Decode video frame
        avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);

        // Did we got a video frame?
        if (frameFinished) {            
            if (queue_picture(is, pFrame) < 0) {
                break;
            }        
        }
        av_free_packet(packet);
        av_free(packet);
    }
    av_frame_free(&pFrame);
    return 0;
}

int queue_picture(VideoState *is, AVFrame *pFrame) {
    VideoPicture *vp;
    int dst_pix_fmt;
    AVPicture pict;

    // wait until we hava space for a view pic
    SDL_LockMutex(is->pictq_mutex);
    while (is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if (is->quit) {
        return -1;
    }

    // windex is set to 0 initially
    vp = &is->pictq[is->pictq_windex];

    // allocate or resize the buffer
    if (vp->bmp == NULL || vp->width != is->video_ctx->width ||
        vp->height != is->video_ctx->height) {    
        vp->allocated = 0;
        alloc_picture(is);
        if (is->quit) {
            return -1;
        }
    }

    // We have a place to put our picture on the queue
    if (vp->bmp != NULL) {
        SDL_LockMutex(vp->bmp->mutex);
        int uvPitch = is->video_ctx->width / 2; 
        pict.data[0] = vp->bmp->yPlane;
        pict.data[1] = vp->bmp->uPlane;
        pict.data[2] = vp->bmp->vPlane;
        pict.linesize[0] = is->video_ctx->width;
        pict.linesize[1] = uvPitch;
        pict.linesize[2] = uvPitch;
        
       // Convert the image into YUV format that SDL uses
	   sws_scale(is->sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize, 0,
				is->video_ctx->height, pict.data, pict.linesize);

       SDL_UpdateYUVTexture(texture, NULL, vp->bmp->yPlane, is->video_ctx->width,
                            vp->bmp->uPlane, uvPitch, vp->bmp->vPlane, uvPitch);
	   SDL_RenderClear(renderer);
	   SDL_RenderCopy(renderer, texture, NULL, NULL);
	   SDL_RenderPresent(renderer);       
       SDL_UnlockMutex(vp->bmp->mutex);

       // now we inform out display thread that we have a pic ready
       if (++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
           is->pictq_windex = 0;
       }
       SDL_LockMutex(is->pictq_mutex);
       is->pictq_size++;
       SDL_UnlockMutex(is->pictq_mutex);
    }
}

void alloc_picture(void *userdata) {
    VideoState *is = (VideoState*)userdata;
    VideoPicture *vp;

    vp = &is->pictq[is->pictq_windex];
    if (vp->bmp != NULL) {
        // We already have one make another, bigger/smaller
        free(vp->bmp->yPlane);
        free(vp->bmp->uPlane);
        free(vp->bmp->vPlane);
        free(vp->bmp);
        vp->bmp->yPlaneSz = 0;
        vp->bmp->uvPlaneSz = 0;
        SDL_DestroyMutex(vp->bmp->mutex);
    }

    // Allocate a place to put our YUV image on the screen
    SDL_LockMutex(screen_mutex);
    Bmp *bmp = (Bmp*)malloc(sizeof(Bmp));
    memset(bmp, 0, sizeof(Bmp));
    bmp->yPlaneSz = is->video_ctx->width * is->video_ctx->height;
    bmp->uvPlaneSz = is->video_ctx->width * is->video_ctx->height / 4; 
   bmp->yPlane = (uint8_t*)malloc(bmp->yPlaneSz);
    bmp->uPlane = (uint8_t*)malloc(bmp->uvPlaneSz);
    bmp->vPlane = (uint8_t*)malloc(bmp->uvPlaneSz);
    bmp->mutex = SDL_CreateMutex();

    SDL_UnlockMutex(screen_mutex);
    
    vp->bmp = bmp;
    vp->width = is->video_ctx->width;
    vp->height = is->video_ctx->height;
    vp->allocated = 1;
}

void video_refresh_timer(void *userdata) {
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;

    if (is->video_st) {
        if (is->pictq_size == 0) {
            schedule_refresh(is, 1);            
        } else {
            vp = &is->pictq[is->pictq_rindex];
            // Now, normally here goes a ton of code about time
            // etc, we're just goting to guess at a delay for now.
            // Yot can increase and decrease this value and hard code the timing
            // but I don't suggest that;
            schedule_refresh(is, 40);

            // show the picture
            video_display(is);

            // update queue for next picture
            if (++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                is->pictq_rindex = 0;
            }
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    } else {
        schedule_refresh(is, 100);
    }
}

void video_display(VideoState *is) {
    VideoPicture *vp = &is->pictq[is->pictq_rindex];
    if (vp->bmp != NULL) {
        SDL_LockMutex(screen_mutex);          
        int uvPitch = is->video_ctx->width / 2;     
        SDL_UpdateYUVTexture(texture, NULL, vp->bmp->yPlane, is->video_ctx->width,
                           vp->bmp->uPlane, uvPitch, vp->bmp->vPlane, uvPitch);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_UnlockMutex(screen_mutex);
    }   
}

void show_packet(VideoState *is, AVPacket* packet) {
    int frameFinished = 0;
    AVFrame *pFrame = av_frame_alloc();
    avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);
    if (!frameFinished) {
        return;
    }
   
	uint8_t *yPlane, *uPlane, *vPlane;
    size_t yPlaneSz, uvPlaneSz;
    yPlaneSz = is->video_ctx->width * is->video_ctx->height;
    uvPlaneSz = is->video_ctx->width * is->video_ctx->height / 4;
    yPlane = (uint8_t*)malloc(yPlaneSz);
    uPlane = (uint8_t*)malloc(uvPlaneSz);
    vPlane = (uint8_t*)malloc(uvPlaneSz);            

    int uvPitch = is->video_ctx->width / 2;
    AVPicture pict;
	pict.data[0] = yPlane;
	pict.data[1] = uPlane;
	pict.data[2] = vPlane;
    pict.linesize[0] = is->video_ctx->width;
	pict.linesize[1] = uvPitch;
	pict.linesize[2] = uvPitch;
		   
    // Convert the image into YUV format that SDL uses
    sws_scale(is->sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize, 0,
              is->video_ctx->height, pict.data, pict.linesize);

    SDL_UpdateYUVTexture(texture, NULL, yPlane, is->video_ctx->width, uPlane,
                              uvPitch, vPlane, uvPitch);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, NULL);
	SDL_RenderPresent(renderer);
    
    // Free the packet that was allocated by av_read_frame
    av_free_packet(packet);

    av_free(pFrame);
}

// https://gist.github.com/MarcoQin/4fde3c9fb0cab64e5389d8808f3093bd
static int audio_resampling(AVCodecContext *audio_decode_ctx,
                            AVFrame *audio_decode_frame,
                            enum AVSampleFormat out_sample_fmt,
                            int out_channels,
                            int out_sample_rate,
                            uint8_t *out_buf) {
    SwrContext *swr_ctx = NULL;
    int ret = 0;
    int64_t in_channel_layout = audio_decode_ctx->channel_layout;
    int64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    int out_nb_channels = 0;
    int out_linesize = 0;
    int in_nb_samples = 0;
    int out_nb_samples = 0;
    int max_out_nb_samples = 0;
    uint8_t **resampled_data = NULL;
    int resampled_data_size = 0;

    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        printf("swr_alloc error\n");
        return -1;
    }

    in_channel_layout = (audio_decode_ctx->channels ==
                     av_get_channel_layout_nb_channels(audio_decode_ctx->channel_layout)) ?
                     audio_decode_ctx->channel_layout :
                     av_get_default_channel_layout(audio_decode_ctx->channels);
    if (in_channel_layout <=0) {
        printf("in_channel_layout error\n");
        return -1;
    }

    if (out_channels == 1) {
        out_channel_layout = AV_CH_LAYOUT_MONO;
    } else if (out_channels == 2) {
        out_channel_layout = AV_CH_LAYOUT_STEREO;
    } else {
        out_channel_layout = AV_CH_LAYOUT_SURROUND;
    }

    in_nb_samples = audio_decode_frame->nb_samples;
    if (in_nb_samples <=0) {
        printf("in_nb_samples error\n");
        return -1;
    }

    av_opt_set_int(swr_ctx, "in_channel_layout", in_channel_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", audio_decode_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", audio_decode_ctx->sample_fmt, 0);

    av_opt_set_int(swr_ctx, "out_channel_layout", out_channel_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);

    if ((ret = swr_init(swr_ctx)) < 0) {
        printf("Failed to initialize the resampling context\n");
        return -1;
    }

    max_out_nb_samples = out_nb_samples = av_rescale_rnd(in_nb_samples,
                                                         out_sample_rate,
                                                         audio_decode_ctx->sample_rate,
                                                         AV_ROUND_UP);

    if (max_out_nb_samples <= 0) {
        printf("av_rescale_rnd error\n");
        return -1;
    }

    out_nb_channels = av_get_channel_layout_nb_channels(out_channel_layout);

    ret = av_samples_alloc_array_and_samples(&resampled_data, &out_linesize, out_nb_channels, out_nb_samples, out_sample_fmt, 0);
    if (ret < 0) {
        printf("av_samples_alloc_array_and_samples error\n");
        return -1;
    }

    out_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, audio_decode_ctx->sample_rate) + in_nb_samples,
                                    out_sample_rate, audio_decode_ctx->sample_rate, AV_ROUND_UP);
    if (out_nb_samples <= 0) {
        printf("av_rescale_rnd error\n");
        return -1;
    }

    if (out_nb_samples > max_out_nb_samples) {
        av_free(resampled_data[0]);
        ret = av_samples_alloc(resampled_data, &out_linesize, out_nb_channels, out_nb_samples, out_sample_fmt, 1);
        max_out_nb_samples = out_nb_samples;
    }

    if (swr_ctx) {
        ret = swr_convert(swr_ctx, resampled_data, out_nb_samples,
                          (const uint8_t **)audio_decode_frame->data, audio_decode_frame->nb_samples);
        if (ret < 0) {
            printf("swr_convert_error\n");
            return -1;
        }

        resampled_data_size = av_samples_get_buffer_size(&out_linesize, out_nb_channels, ret, out_sample_fmt, 1);
        if (resampled_data_size < 0) {
            printf("av_samples_get_buffer_size error\n");
            return -1;
        }
    } else {
        printf("swr_ctx null error\n");
        return -1;
    }

    memcpy(out_buf, resampled_data[0], resampled_data_size);

    if (resampled_data) {
        av_freep(&resampled_data[0]);
    }
    av_freep(&resampled_data);
    resampled_data = NULL;

    if (swr_ctx) {
        swr_free(&swr_ctx);
    }
    return resampled_data_size;
}

