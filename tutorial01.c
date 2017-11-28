#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>

#include <stdio.h>

void saveFrame(AVFrame *pFrame, int width, int height, int iFrame);

 
int main(int argc, char *argv[]) {    
    printf("Input video : %s\n", argv[1]);
    // register all formats and codes. 
    av_register_all();
    
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
    //av_dump_format(pFormatCtx, 0, argv[1], 0);
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
    AVFrame *pFrameRGB = NULL;
    // Allocate video frame
    pFrame = av_frame_alloc();
    // Allocate an AVFrame structure
    pFrameRGB = av_frame_alloc();
    if (pFrameRGB == NULL) {
		return -1;    
    }
    uint8_t *buffer = NULL;
    int numBytes;
    // Determine required buffer size and allocate buffer
    numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 1);
    buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
    // Assign appropriate parts of buffer to iamge planes in pFrameRGB
    // Note that pFrameRGB in an AVFrame, but AVFame is a superset of AVPicture
    avpicture_fill((AVPicture *)pFrameRGB, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
    
    struct SwsContext *sws_ctx = NULL;
    int frameFinished = 0;
    AVPacket packet;
    // initialize SWS context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, 
                           pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);
   
    while (av_read_frame(pFormatCtx, &packet) >= 0 && (i < 100)) {
       // Is this a packet from the video stream
       if (packet.stream_index == videoStream) {
           // Decode video frame
           avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);         
           // Did we get a video frame?
           if (frameFinished) {
	       // conver the image from its native format RGB
               sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize, 0,
                        pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);

               if (++i <= 100) {
                   saveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
               }
           }           
       }
       // Free the packet that was allocated by av_read_frame
       av_free_packet(&packet);
    }

    // Free the RGB image
    av_free(buffer);
    av_free(pFrameRGB);
    
    // Free the YUV frame
    av_free(pFrame);  

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
