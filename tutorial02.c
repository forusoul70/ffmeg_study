#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#include <SDL.h>
#include <SDL_thread.h>

#include <stdio.h>

void saveFrame(AVFrame *pFrame, int width, int height, int iFrame);

 
int main(int argc, char *argv[]) {    
    printf("Input video : %s\n", argv[1]);
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
    AVCodecContext *pCodecCtx = NULL;
    AVCodecParameters *pCodecPar = NULL;
    
    // find the first video stream
    int i=0;
    for (i=0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            printf("Found video stream %d\n", videoStream);
            break;
        }
    }

    if (videoStream == -1) {
        fprintf(stderr, "Failed to find video stream");
        return -1;
    }

    // Get a pointer to the codec context for the video steam    
    pCodecPar = pFormatCtx->streams[videoStream]->codecpar;
    
    AVCodec *pCodec = NULL;
    // find the decoder for the video stream
    pCodec = avcodec_find_decoder(pCodecPar->codec_id);
    if (pCodec == NULL) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    // Copy context
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (avcodec_parameters_to_context(pCodecCtx, pCodecPar) != 0) {
        fprintf(stderr, "Couldn't copy codec context");
        return -1;    
    }

    // open codec
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        fprintf(stderr, "Failed to open codec");
		return -1;
    }

    AVFrame *pFrame = NULL;
    // Allocate video frame
    pFrame = av_frame_alloc();
    
	uint8_t *buffer = NULL;
    int numBytes;
    // Determine required buffer size and allocate buffer
    numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 1);
    buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
    // Assign appropriate parts of buffer to iamge planes in pFrame
    // Note that pFrame in an AVFrame, but AVFame is a superset of AVPicture
    avpicture_fill((AVPicture *)pFrame, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
    
    struct SwsContext *sws_ctx = NULL;
	int frameFinished = 0;
    AVPacket packet;

	// create display
	SDL_Surface *screen = SDL_CreateWindow("Main display", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
		pCodecCtx->width, pCodecCtx->height, 0);
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
			SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);
	if (texture == NULL) {
    	fprintf(stderr, "SDL: could not create texture - exiting\n");
		exit(1);
	}

	// initialize SWS context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, 
                           pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
						   NULL, NULL, NULL);

    // set up YV12 pixel array (12 bits per pixel)
	uint8_t *yPlane, *uPlane, *vPlane;
	size_t yPlaneSz, uvPlaneSz;
	yPlaneSz = pCodecCtx->width * pCodecCtx->height;
	uvPlaneSz = pCodecCtx->width * pCodecCtx->height / 4;
    yPlane = (uint8_t*)malloc(yPlaneSz);
	uPlane = (uint8_t*)malloc(uvPlaneSz);
	vPlane = (uint8_t*)malloc(uvPlaneSz);
	 
	int uvPitch = pCodecCtx->width / 2;
	int count=0;
    while (av_read_frame(pFormatCtx, &packet) >= 0) {
       // Is this a packet from the video stream
       if (packet.stream_index == videoStream) {
           // Decode video frame
           avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);         
           // Did we get a video frame?
           if (frameFinished) {
     		   AVPicture pict;
			   pict.data[0] = yPlane;
			   pict.data[1] = uPlane;
			   pict.data[2] = vPlane;

			   pict.linesize[0] = pCodecCtx->width;
			   pict.linesize[1] = uvPitch;
			   pict.linesize[2] = uvPitch;
			   
      	       // Convert the image into YUV format that SDL uses
               sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize, 0,
                        pCodecCtx->height, pict.data, pict.linesize);

               SDL_UpdateYUVTexture(texture, NULL, yPlane, pCodecCtx->width, uPlane, uvPitch, vPlane, uvPitch);
			   SDL_RenderClear(renderer);
			   SDL_RenderCopy(renderer, texture, NULL, NULL);
			   SDL_RenderPresent(renderer);
           }           
       }
       // Free the packet that was allocated by av_read_frame
       av_free_packet(&packet);
    }

	// Free th YUV frame
	av_frame_free(&pFrame);
	free(yPlane);
	free(uPlane);
	free(vPlane);

    // Free the RGB image
    av_free(buffer);

    // Close the codecs
    avcodec_close(pCodecCtx);

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
