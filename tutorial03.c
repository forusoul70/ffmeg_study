#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#include <SDL.h>
#include <SDL_thread.h>

#include <stdio.h>
#include <assert.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif


typedef struct PacketQueue {
  AVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  int size; // refer to a bytes size that we get from packetsize
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;

PacketQueue audioq;
static int quit = 0;

void saveFrame(AVFrame *pFrame, int width, int height, int iFrame);
void packet_queue_init(PacketQueue *q);
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);
int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size);
void audio_callback(void *userdata, Uint8 *stream, int len);

int main(int argc, char *argv[]) {
    printf("Input video : %s\n", argv[1]);
    SDL_Event event;
    // register all formats and codes. 
    av_register_all();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
	    fprintf(stderr, "Cloud not intitialized SDL - %s\n", SDL_GetError());
	    exit(1);
    }
    
    AVFormatContext *pFormatCtx = NULL;
    // open video file
    // auto format and buufer size 
    if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0) {        
        return -1; // Failed to open file
    } 
    
    // Retrieve stream information
    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
    	return -1;
    }
    
    // Dump informtion about file onto stardard error
    av_dump_format(pFormatCtx, 0, argv[1], 0);

    int videoStream = -1;
    int audioStream = -1;    
    AVCodecContext *pAudioCodecCtx = NULL;
    AVCodecParameters *pAudioCodecPar = NULL;
    AVCodecContext *pVideoCodecCtx = NULL;
    AVCodecParameters *pVideoCodecPar = NULL;
    
    // find the first video stream
    int i=0;
    for (i=0; i < pFormatCtx->nb_streams; i++) {
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

    // Get a pointer to the codec context for the audio steam    
    pAudioCodecPar = pFormatCtx->streams[audioStream]->codecpar;

    // find the decoder for the audio stream
    AVCodec *pCodec = pCodec = avcodec_find_decoder(pAudioCodecPar->codec_id);    
    if (pCodec == NULL) {
        fprintf(stderr, "Unsupported audio codec!\n");
        return -1;
    }
    // Copy context
    pAudioCodecCtx = avcodec_alloc_context3(pCodec);
    if (avcodec_parameters_to_context(pAudioCodecCtx, pAudioCodecPar) != 0) {
        fprintf(stderr, "Couldn't copy codec context\n");
        return -1;    
    }

    // Set Audio setting from codec info
    SDL_AudioSpec wanted_spec;
    SDL_AudioSpec spec;
    wanted_spec.freq = pAudioCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS; // singed 16bits, endian-order will depend on the system.
    wanted_spec.channels = pAudioCodecCtx->channels; // Number of audio channel
    wanted_spec.silence = 0; // Silence ? Since audio is signed, 0 is of cource the usual value.
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = pAudioCodecCtx; // SDL pass this void pointer to callback

    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }

    // open codec
    if (avcodec_open2(pAudioCodecCtx, pCodec, NULL) < 0) {
        fprintf(stderr, "Failed to open codec");
		return -1;
    }

    packet_queue_init(&audioq);
    SDL_PauseAudio(0); // finally start the audio device. It plays slience if it doesn't get data

    // Get a pointer to the codec contxt for video stream
    pVideoCodecPar = pFormatCtx->streams[videoStream]->codecpar;

    // find the decoder for the video stream
    pCodec = avcodec_find_decoder(pVideoCodecPar->codec_id);
    if (pCodec == NULL) {
        fprintf(stderr, "Unsupported video codec!\n");
        return -1;
    }
    // Copy Context
    pVideoCodecCtx = avcodec_alloc_context3(pCodec);
    if (avcodec_parameters_to_context(pVideoCodecCtx, pVideoCodecPar) != 0) {
        fprintf(stderr, "Couldn't copy codec context\n");
        return -1;
    }
    // Open codec
    if (avcodec_open2(pVideoCodecCtx, pCodec, NULL) < 0) {
        fprintf(stderr, "Failed to open codec");
        return -1;
    }

    // Allocate video frame
    AVFrame *pFrame = av_frame_alloc();    
	uint8_t *buffer = NULL;
    int numBytes;
    
    // Determine required buffer size and allocate buffer
    numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pVideoCodecCtx->width, pVideoCodecCtx->height, 1);
    buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
    // Assign appropriate parts of buffer to iamge planes in pFrame
    // Note that pFrame in an AVFrame, but AVFame is a superset of AVPicture
    avpicture_fill((AVPicture *)pFrame, buffer, AV_PIX_FMT_RGB24, pVideoCodecCtx->width, pVideoCodecCtx->height);
    
    struct SwsContext *sws_ctx = NULL;
	int frameFinished = 0;
    AVPacket packet;

	// create display
	SDL_Surface *screen = SDL_CreateWindow("Main display", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
		pVideoCodecCtx->width, pVideoCodecCtx->height, 0);
	if (screen == NULL) {
		fprintf(stderr, "SDL : could not set video mode - exiting\n");
		exit(1);
	}
    SDL_Renderer *renderer = SDL_CreateRenderer(screen, -1, 0);
	if (renderer == NULL) {
		fprintf(stderr, "SDL : could not create renderer - exiting\n");
		exit(1);
	}
	// Allocate a place to put our YUV image on that screen
	SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12,
			SDL_TEXTUREACCESS_STREAMING, pVideoCodecCtx->width, pVideoCodecCtx->height);
	if (texture == NULL) {
    	fprintf(stderr, "SDL: could not create texture - exiting\n");
		exit(1);
	}

	// initialize SWS context for software scaling
    sws_ctx = sws_getContext(pVideoCodecCtx->width, pVideoCodecCtx->height, pVideoCodecCtx->pix_fmt, 
                           pVideoCodecCtx->width, pVideoCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
						   NULL, NULL, NULL);

    // set up YV12 pixel array (12 bits per pixel)
	uint8_t *yPlane, *uPlane, *vPlane;
	size_t yPlaneSz, uvPlaneSz;
	yPlaneSz = pVideoCodecCtx->width * pVideoCodecCtx->height;
	uvPlaneSz = pVideoCodecCtx->width * pVideoCodecCtx->height / 4;
    yPlane = (uint8_t*)malloc(yPlaneSz);
	uPlane = (uint8_t*)malloc(uvPlaneSz);
	vPlane = (uint8_t*)malloc(uvPlaneSz);
	 
	int uvPitch = pVideoCodecCtx->width / 2;
	int count=0;
    while (av_read_frame(pFormatCtx, &packet) >= 0) {
       // Is this a packet from the video stream
       if (packet.stream_index == videoStream) {
           // Decode video frame
           avcodec_decode_video2(pVideoCodecCtx, pFrame, &frameFinished, &packet);         
           // Did we get a video frame?
           if (frameFinished) {
     		   AVPicture pict;
			   pict.data[0] = yPlane;
			   pict.data[1] = uPlane;
			   pict.data[2] = vPlane;

			   pict.linesize[0] = pVideoCodecCtx->width;
			   pict.linesize[1] = uvPitch;
			   pict.linesize[2] = uvPitch;
			   
      	       // Convert the image into YUV format that SDL uses
               sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize, 0,
                        pVideoCodecCtx->height, pict.data, pict.linesize);

               SDL_UpdateYUVTexture(texture, NULL, yPlane, pVideoCodecCtx->width, uPlane, uvPitch, vPlane, uvPitch);
			   SDL_RenderClear(renderer);
			   SDL_RenderCopy(renderer, texture, NULL, NULL);
			   SDL_RenderPresent(renderer);
           }
           // Free the packet that was allocated by av_read_frame
           av_free_packet(&packet);
       } else if (packet.stream_index == audioStream) {
           // feeding audio packet
           packet_queue_put(&audioq, &packet);
       } else {
           // Free the packet that was allocated by av_read_frame
           av_free_packet(&packet);
       }       

       SDL_PollEvent(&event);
       switch(event.type) {
       case SDL_QUIT:
           quit = 1;
           SDL_Quit();
           exit(0);
           break;
       }
    }

    // Free th YUV frame
    av_frame_free(&pFrame);
    free(yPlane);
    free(uPlane);
    free(vPlane);

    // Free the RGB image
    av_free(buffer);

    // Close the codecs
    avcodec_close(pAudioCodecCtx);
    avcodec_close(pVideoCodecCtx);

    // Close the video file
    avformat_close_input(&pFormatCtx);

    return 0;
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
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt1;
    if (av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (pkt1 == NULL) {
        return -1;
    }
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);

    if (q->last_pkt == NULL) {
        q->first_pkt = pkt1;        
    } else {
        q->last_pkt->next = pkt1;
    }

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond); // send singnal to our get function(if it's waiting)

    SDL_UnlockMutex(q->mutex);
    return 0;
}

/*
* stream is the buffer we will be writing audio data to
* len is size of th buffer
*/
void audio_callback(void *userdata, uint8_t *stream, int len) {
    AVCodecContext *aCodecCtx = (AVCodecContext*)userdata;
    int len1, audio_size;

    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    while (len > 0) {
        if(audio_buf_index >= audio_buf_size) {
            // We have already sent all our data, get more
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));

            if (audio_size < 0) {
                // If error, output slience
                audio_buf_size = 1024;
                memset(audio_buf, 0, audio_buf_size);                
            } else {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len) {
            len1 = len;
        }
        memcpy(stream, (uint8_t*)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t* audio_buf, int buf_size) {
    static AVPacket pkt;
    static uint8_t *audio_pkt_data = NULL;
    static int audio_pkt_size = 0;
    static AVFrame frame;

    int len1, data_size = 0;

    for (;;) {
        while (audio_pkt_size > 0) {
            int got_frame = 0;
            // sister function avcodec_decode_vidoe()
            // expect int this case, a pakeet might have more than one frame.
            // SDL gives 8 bits int buffer
            // ffmpeg gives 16bits int buffer
            // len1 is how much of the packet we've used
            // data_size is amount of raw data returned
            len1 = avcodec_decode_audio4(aCodecCtx, &frame, &got_frame, &pkt);
            if (len1 < 0) {
                // if error, skip frame
                audio_pkt_size = 0;
                break;
            }
            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            data_size = 0;
            if (got_frame) {
                data_size = av_samples_get_buffer_size(NULL, aCodecCtx->channels,
                                                      frame.nb_samples, aCodecCtx->sample_fmt, 1);
                assert(data_size <= buf_size);
                memcpy(audio_buf, frame.data[0], data_size);
            }
            if (data_size <= 0) {
                // No data yet, get more frames
                continue;
            }
            // We hava data, return it and come back for more later
            return data_size;
        }
        if (pkt.data) {
            av_free_packet(&pkt);        
        }

        if (quit) {
            return -1;
        }

        if (packet_queue_get(&audioq, &pkt, 1) < 0) {
            return -1;
        }
        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
    }   
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);
    for (;;) {        
        if (quit) {
            ret = -1;
            break;
        }
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (q->first_pkt == NULL) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
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

