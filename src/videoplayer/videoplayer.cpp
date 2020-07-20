/**
 * 叶海辉
 * QQ群121376426
 * http://blog.yundiantech.com/
 */

#include "videoplayer.h"
#include <QDebug>

extern "C"
{
    #include "libavcodec/avcodec.h"
    #include "libavformat/avformat.h"
    #include "libavutil/pixfmt.h"
    #include "libavutil/pixdesc.h"
    #include "libavutil/opt.h"
    #include "libavutil/avassert.h"
    #include "libswscale/swscale.h"
    #include "libavutil/imgutils.h"
}

#include <stdio.h>

VideoPlayer::VideoPlayer()
{

}

VideoPlayer::~VideoPlayer()
{

}

void VideoPlayer::startPlay()
{
    ///调用 QThread 的start函数 将会自动执行下面的run函数 run函数是一个新的线程
    this->start();

}

static enum AVPixelFormat hw_pix_fmt;
static AVBufferRef *hw_device_ctx = NULL;
static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
    int err = 0;

    if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,
                                      NULL, NULL, 0)) < 0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt)
            return *p;
    }

    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

static int decode_write(AVCodecContext *avctx, AVPacket *packet)
{
    AVFrame *frame = NULL, *sw_frame = NULL;
    AVFrame *tmp_frame = NULL;
    uint8_t *buffer = NULL;
    int size;
    int ret = 0;

    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
        return ret;
    }

    while (1) {
        if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc())) {
            fprintf(stderr, "Can not alloc frame\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            av_frame_free(&sw_frame);
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error while decoding\n");
            goto fail;
        }

        if (frame->format == hw_pix_fmt) {
            /* retrieve data from GPU to CPU */
            if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0) {
                fprintf(stderr, "Error transferring the data to system memory\n");
                goto fail;
            }
            tmp_frame = sw_frame;
        } else
            tmp_frame = frame;
//enum AVPixelFormat pix_fmt, int width, int height, int align
        size = av_image_get_buffer_size(AVPixelFormat(tmp_frame->format), tmp_frame->width,
                                        tmp_frame->height, 1);
        buffer = (uint8_t*)av_malloc(size);
        if (!buffer) {
            fprintf(stderr, "Can not alloc buffer\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ret = av_image_copy_to_buffer(buffer, size,
                                      (const uint8_t * const *)tmp_frame->data,
                                      (const int *)tmp_frame->linesize, AVPixelFormat(tmp_frame->format),
                                      tmp_frame->width, tmp_frame->height, 1);
        if (ret < 0) {
            fprintf(stderr, "Can not copy image to buffer\n");
            goto fail;
        }

        qDebug() << "decode successful";
//        if ((ret = fwrite(buffer, 1, size, output_file)) < 0) {
//            fprintf(stderr, "Failed to dump raw data.\n");
//            goto fail;
//        }

    fail:
        av_frame_free(&frame);
        av_frame_free(&sw_frame);
        av_freep(&buffer);
        if (ret < 0)
            return ret;
    }
}

void VideoPlayer::run()
{
    AVFormatContext *pFormatCtx;
    AVCodecContext *pCodecCtx;
    AVCodec *pCodec;
    AVFrame *pFrame, *pFrameRGB, *swFrame, *tempFrame;
    uint8_t *out_buffer;
    AVPacket packet;
    AVStream *video = NULL;

    static struct SwsContext *img_convert_ctx;

    int videoStream, i, numBytes;
    int ret, got_picture;

//    av_register_all(); //初始化FFMPEG  调用了这个才能正常使用用编码器和解码器

    avformat_network_init();
    //Allocate an AVFormatContext.
    pFormatCtx = avformat_alloc_context();

    const char *device_name = "cuda";
    enum AVHWDeviceType type = av_hwdevice_find_type_by_name(device_name);
    if(type == AV_HWDEVICE_TYPE_NONE)
    {
        printf("Device type %s is not supported.\n", device_name);
        fprintf(stderr, "Available device types:");
          while((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
              fprintf(stderr, " %s", av_hwdevice_get_type_name(type));
          fprintf(stderr, "\n");
    }

    AVDictionary *opt = nullptr;
//    av_dict_set(&opt,"buffer_size","1024000",0);
//    av_dict_set(&opt,"max_delay","0",0);
    av_dict_set(&opt,"rtsp_transport","tcp",0);
    av_dict_set(&opt,"stimeout","5000000",0);
    if (avformat_open_input(&pFormatCtx, mFileName.toUtf8().data(), NULL, &opt) != 0) {
        printf("can't open the file. \n");
        return;
    }

    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        printf("Could't find stream infomation.\n");
        return;
    }

    ret = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &pCodec, 0);
      if (ret < 0) {
          fprintf(stderr, "Cannot find a video stream in the input file\n");
          return;
      }
      videoStream = ret;

    for (i = 0;; i++) {
          const AVCodecHWConfig *config = avcodec_get_hw_config(pCodec, i);
          if (!config) {
              fprintf(stderr, "Decoder %s does not support device type %s.\n",
                      pCodec->name, av_hwdevice_get_type_name(type));
              return;
          }
          if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
              config->device_type == type) {
              hw_pix_fmt = config->pix_fmt;
              break;
          }
      }
    ///查找解码器
    if (!(pCodecCtx = avcodec_alloc_context3(pCodec)))
        return;

    video = pFormatCtx->streams[videoStream];
    if (avcodec_parameters_to_context(pCodecCtx, video->codecpar) < 0)
        return;

    pCodecCtx->get_format = get_hw_format;

    if (hw_decoder_init(pCodecCtx, type) < 0)
        return;

    ///打开解码器
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        printf("Could not open codec.\n");
        return;
    }

    pFrame = av_frame_alloc();
    pFrameRGB = av_frame_alloc();
    tempFrame = av_frame_alloc();
    swFrame = av_frame_alloc();

    ///这里我们改成了 将解码后的YUV数据转换成RGB32
    img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height,
            AV_PIX_FMT_NV12, pCodecCtx->width, pCodecCtx->height,
            AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

    numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, pCodecCtx->width,pCodecCtx->height);

    out_buffer = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
    avpicture_fill((AVPicture *) pFrameRGB, out_buffer, AV_PIX_FMT_RGB32,
            pCodecCtx->width, pCodecCtx->height);

    av_dump_format(pFormatCtx, 0, mFileName.toUtf8().data(), 0); //输出视频信息

    while (1)
    {
        if (av_read_frame(pFormatCtx, &packet) < 0)
        {
            break; //这里认为视频读取完了
        }

        if (packet.stream_index == videoStream) {
            ret = avcodec_send_packet(pCodecCtx, &packet);
            if (ret < 0) {
                printf("decode error.\n");
                return;
            }

            while (1) {
                ret = avcodec_receive_frame(pCodecCtx, pFrame);
                if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    break;
                }else if(ret < 0){
                    qDebug() << "Error while decoding";
                    break;
                }

                ret = av_hwframe_transfer_data(swFrame, pFrame, 0);
                if(ret < 0)
                {
                    qDebug() << "Error transferring the data to system memory";
                    break;
                }
                sws_scale(img_convert_ctx,
                          (uint8_t const * const *) swFrame->data,
                          swFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data,
                          pFrameRGB->linesize);
                static int index = 0;
                qDebug() << "frame" << index++;
                //把这个RGB数据 用QImage加载
                QImage tmpImg((uchar *)out_buffer,pCodecCtx->width,pCodecCtx->height,QImage::Format_RGB32);
                QImage image = tmpImg.copy(); //把图像复制一份 传递给界面显示
                emit sig_GetOneFrame(image);  //发送信号
            }
        }

        av_packet_unref(&packet);
        msleep(30); //停一停  不然放的太快了
    }
    av_free(out_buffer);
    av_free(pFrameRGB);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);
}
