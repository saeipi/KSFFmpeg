//
//  ks_media_player.c
//  KSMediaPlayer
//
//  Created by saeipi on 2020/6/1.
//  Copyright © 2020 saeipi. All rights reserved.
//

#include "ks_media_player.h"
#include <assert.h>
#include <math.h>

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "SDL2/SDL.h"
//#include "SDL2/SDL_video.h"
//#include "SDL2/SDL_render.h"
//#include "SDL2/SDL_rect.h"

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

/* 音频缓存区大小*/
#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000 //channels(2) * data_size(2) * sample_rate(48000)

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define AV_SYNC_THRESHOLD 0.01
/* 同步阈值。如果误差太大，则不进行校正，也不丢帧来做同步了 */
#define AV_NOSYNC_THRESHOLD 10.0

#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

#define DEFAULT_AV_SYNC_TYPE AV_SYNC_AUDIO_MASTER //AV_SYNC_VIDEO_MASTER

/*
 typedef struct AVPacketList {
 AVPacket pkt;
 struct AVPacketList *next;
 } AVPacketList;
 */
typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    /* 互斥锁结构 */
    SDL_mutex *mutex;
    /* 锁的条件变量 */
    SDL_cond *cond;
} PacketQueue;


typedef struct VideoPicture {
    /* 图片数据结构 */
    AVPicture *bmp;
    int width, height; /* source height & width */
    int allocated;
    double pts;
} VideoPicture;

typedef struct VideoState {
    
    //multi-media file
    char            filename[1024];
    AVFormatContext *pFormatCtx;
    int             videoStream, audioStream;
    
    //sync
    int             av_sync_type;
    /* 外部时钟 */
    double          external_clock; /* external clock base */
    int64_t         external_clock_time;
    
    double          audio_diff_cum; /* used for AV difference average computation */
    double          audio_diff_avg_coef;
    double          audio_diff_threshold;
    int             audio_diff_avg_count;
    
    /* 音频时钟 */
    double          audio_clock;
    double          frame_timer;
    double          frame_last_pts;
    double          frame_last_delay;
    
    /* 视讯时钟 */
    double          video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame
    double          video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)
    int64_t         video_current_pts_time;  ///<time (av_gettime) at which we updated video_current_pts - used to have running video pts
    
    //audio
    AVStream        *audio_st;
    AVCodecContext  *audio_ctx;
    PacketQueue     audioq;
    /* audio 缓冲区 */
    uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
    unsigned int    audio_buf_size;
    unsigned int    audio_buf_index;
    AVFrame         audio_frame;
    AVPacket        audio_pkt;
    uint8_t         *audio_pkt_data;
    int             audio_pkt_size;
    int             audio_hw_buf_size;
    
    //video
    AVStream        *video_st;
    AVCodecContext  *video_ctx;
    PacketQueue     videoq;
    /*
     SwsContext:
     数据对象，动态管理，外部接口
     视频原始数据格式转换对象
     */
    struct SwsContext *video_sws_ctx;
    struct SwrContext *audio_swr_ctx;
    
    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int             pictq_size, pictq_rindex, pictq_windex;
    /* 互斥结构体 */
    SDL_mutex       *pictq_mutex;
    /* 条件变量 */
    SDL_cond        *pictq_cond;
    
    /* 解析线程 */
    SDL_Thread      *parse_tid;
    /* 视屏线程 */
    SDL_Thread      *video_tid;
    
    int             quit;
} VideoState;

/* 互斥结构体 */
SDL_mutex    *text_mutex;
SDL_Window   *win = NULL;
SDL_Renderer *renderer;
SDL_Texture  *texture;

/* 同步方案 */
enum {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_MASTER,
};

/* Since we only have one decoding thread, the Big Struct
 can be global in case we need it. */
/* 全局视频状态管理 */
VideoState *global_video_state;
/*
 memset函数:将某一块内存中的内容全部设置为指定的值， 这个函数通常为新申请的内存做初始化工作。
 extern void *memset(void *buffer, int c, int count) buffer：为指针或是数组,c：是赋给buffer的值,count：是buffer的长度.
 */
/* 队列初始化 */
void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}
/*
 void *av_malloc(size_t size);
 分配一块字节数为 size 的内存区域，默认是从堆上，默认是按字节对齐，字节对齐的大小 和 CPU 有密切方便汇编指令处理。
 */
/* 入队列 */
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    /*
     av_dup_packet, 通过调用 av_malloc、memcpy、memset等函数， 将shared buffer 的AVPacket duplicate(复制)到独立的buffer中。并且修改AVPacket的析构函数指针av_destruct_pkt。
     */
    /*
     int av_dup_packet(AVPacket *pkt);
     复制src->data引用的数据缓存，赋值给dst。也就是创建两个独立packet，这个功能现在可用使用函数av_packet_ref来代替
     */
    AVPacketList *pkt1;
    if(av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    
    /* SDL_LockMutex :互斥锁(加锁) */
    SDL_LockMutex(q->mutex);
    
    /*
     1、如果没有last_pkt：代表没有first_pkt，更新first_pkt
     2、如果有last_pkt：更新first_pkt->next:AVPacketList
     */
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    
    /*
     更新last_pkt指针
     */
    q->last_pkt = pkt1;
    /*
     更新包数量
     */
    q->nb_packets++;
    /*
     包大小叠加
     */
    q->size += pkt1->pkt.size;
    
    /* SDL_CondSignal:发送通知解阻塞 */
    SDL_CondSignal(q->cond);
    
    /* SDL_UnlockMutex : 互斥锁(解锁) */
    SDL_UnlockMutex(q->mutex);
    return 0;
}

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;
    
    /* SDL_LockMutex :互斥锁(加锁) */
    SDL_LockMutex(q->mutex);
    
    /* 一直循环 */
    for(;;) {
        /* 是否退出 */
        if(global_video_state->quit) {
            ret = -1;
            break;
        }
        /* 获取队列第一个包 */
        pkt1 = q->first_pkt;
        if (pkt1) {
            /* 更新first_pktd指向 */
            q->first_pkt = pkt1->next;
            /* 如果q->first_pkt == NULL，则更新q->last_pkt */
            if (!q->first_pkt)
                q->last_pkt = NULL;
            /* 取packet后，数量更新减1 */
            q->nb_packets--;
            /* 更新包大小 */
            q->size -= pkt1->pkt.size;
            
            /* 通过指针更新外部变量 */
            *pkt = pkt1->pkt;
            
            /* 内存释放 */
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            /* SDL_CondWait :等待(线程阻塞) */
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    /* SDL_UnlockMutex : 互斥锁(解锁) */
    SDL_UnlockMutex(q->mutex);
    return ret;
}

double get_audio_clock(VideoState *is) {
    /* 显示时间戳 */
    double pts;
    int hw_buf_size, bytes_per_sec, n;
    
    /* 音频时钟 */
    pts = is->audio_clock; /* maintained in the audio thread */
    
    /* 缓存大小 - index*/
    hw_buf_size = is->audio_buf_size - is->audio_buf_index;
    bytes_per_sec = 0;
    n = is->audio_ctx->channels * 2;
    
    /* 判断是否有音频流 */
    if(is->audio_st) {
        /* is->audio_ctx->sample_rate:采样率 */
        bytes_per_sec = is->audio_ctx->sample_rate * n;
    }
    
    /* 计算播放时间 */
    if(bytes_per_sec) {
        pts -= (double)hw_buf_size / bytes_per_sec;
    }
    return pts;
}

/* 获取视屏时钟*/
double get_video_clock(VideoState *is) {
    double delta;
    
    delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
    return is->video_current_pts + delta;
}

/* 获取外部时钟 */
double get_external_clock(VideoState *is) {
    return av_gettime() / 1000000.0;
}

/* 主时钟 */
double get_master_clock(VideoState *is) {
    if(is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
         /* 视屏帧同步方案 */
        return get_video_clock(is);
    } else if(is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
         /* 音频同步方案 */
        return get_audio_clock(is);
    } else {
        /* 外部时钟同步方案 */
        return get_external_clock(is);
    }
}

/* Add or subtract samples to get a better sync, return new
 audio buffer size */
int synchronize_audio(VideoState *is, short *samples,
                      int samples_size, double pts) {
    int n;
    double ref_clock;
    
    n = 2 * is->audio_ctx->channels;
    /* 非音频同步方案 */
    if(is->av_sync_type != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int wanted_size, min_size, max_size /*, nb_samples */;
        /* 获取时钟 */
        ref_clock = get_master_clock(is);
        /* 音频时钟 - 主时钟*/
        diff = get_audio_clock(is) - ref_clock;
        /*
         diff小于0，视频比音频慢，丢帧
         diff大于0, 视频比音频快，不丢帧
         */
        /* 声音时钟和视频时钟的差异在我们的阀值范围内 */
        if(diff < AV_NOSYNC_THRESHOLD) {
            // accumulate the diffs
            /* 用公式diff_sum=new_diff+diff_sum*c来计算差异 */
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            /* 音频差异平均计数*/
            if(is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                is->audio_diff_avg_count++;
            } else {
                /* 当准备好去找平均差异的时候，我们用简单的计算方式：avg_diff=diff_sum*(1-c)来平均差异 */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
                /* 音频现在时钟大于视频现在时钟 */
                if(fabs(avg_diff) >= is->audio_diff_threshold) {
                    /* 记住audio_length*(sample_rate）*channels*2就是audio_length秒时间的样本数。*/
                    wanted_size = samples_size + ((int)(diff * is->audio_ctx->sample_rate) * n);
                    min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
                    max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);
                    if(wanted_size < min_size) {
                        wanted_size = min_size;
                    } else if (wanted_size > max_size) {
                        wanted_size = max_size;
                    }
                    if(wanted_size < samples_size) {
                        /* remove samples */
                        samples_size = wanted_size;
                    } else if(wanted_size > samples_size) {
                        uint8_t *samples_end, *q;
                        int nb;
                        
                        /* add samples by copying final sample*/
                        //nb = (samples_size - wanted_size);原版: 视乎写反了
                        nb = (wanted_size - samples_size);
                        
                        samples_end = (uint8_t *)samples + samples_size - n;
                        q = samples_end + n;
                        while(nb > 0) {
                            memcpy(q, samples_end, n);
                            q += n;
                            nb -= n;
                        }
                        samples_size = wanted_size;
                    }
                }
            }
        } else {
            /* 声音时钟和视频时钟的差异大于我们的阀值。失去同步 */
            /* difference is TOO big; reset diff stuff */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }
    return samples_size;
}

/* 音频解码 */
int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size, double *pts_ptr) {
    
    int len1, data_size = 0;
    AVPacket *pkt = &is->audio_pkt;
    double pts;
    int n;
    
    
    for(;;) {
        while(is->audio_pkt_size > 0) {
            int got_frame = 0;
            /*音频解码: int avcodec_decode_audio4(AVCodecContext *avctx, AVFrame *frame, int *got_frame_ptr, const AVPacket *avpkt);
             *@param avctx 编解码器上下文
             *@param [out] frame用于存储解码音频样本的AVFrame
             *@param [out] got_frame_ptr如果没有帧可以解码则为零，否则为非零
             *@param [in] avpkt包含输入缓冲区的输入AVPacket
             *@return 如果在解码期间发生错误，则返回否定错误代码，否则返回从输入AVPacket消耗的字节数。
             */
            len1 = avcodec_decode_audio4(is->audio_ctx, &is->audio_frame, &got_frame, pkt);
            if(len1 < 0) {
                /* if error, skip frame */
                is->audio_pkt_size = 0;
                break;
            }
            data_size = 0;
            if(got_frame) {
                /*
                 data_size = av_samples_get_buffer_size(NULL,
                 is->audio_ctx->channels,
                 is->audio_frame.nb_samples,
                 is->audio_ctx->sample_fmt,
                 1);
                 */
                data_size = 2 * is->audio_frame.nb_samples * 2;
                if (data_size <= buf_size) {
                    return -1;
                }
                /* 断言 */
                //assert(data_size <= buf_size);
                
                /*
                 int swr_convert(struct SwrContext *s, uint8_t **out, int out_count, const uint8_t **in, int in_count);
                 参数1：音频重采样的上下文
                 参数2：输出的指针。传递的输出的数组
                 参数3：输出的样本数量，不是字节数。单通道的样本数量。
                 参数4：输入的数组，AVFrame解码出来的DATA
                 参数5：输入的单通道的样本数量。
                 */
                swr_convert(is->audio_swr_ctx,
                            &audio_buf,
                            MAX_AUDIO_FRAME_SIZE*3/2,
                            (const uint8_t **)is->audio_frame.data,
                            is->audio_frame.nb_samples);
                
                memcpy(audio_buf, is->audio_frame.data[0], data_size);
            }
            is->audio_pkt_data += len1;
            is->audio_pkt_size -= len1;
            if(data_size <= 0) {
                /* No data yet, get more frames */
                continue;
            }
            pts = is->audio_clock;
            *pts_ptr = pts;
            n = 2 * is->audio_ctx->channels;
            is->audio_clock += (double)data_size /
            (double)(n * is->audio_ctx->sample_rate);
            /* We have data, return it and come back for more later */
            return data_size;
        }
        if(pkt->data)
            av_free_packet(pkt);
        
        if(is->quit) {
            return -1;
        }
        /* next packet */
        if(packet_queue_get(&is->audioq, pkt, 1) < 0) {
            return -1;
        }
        is->audio_pkt_data = pkt->data;
        is->audio_pkt_size = pkt->size;
        /* if update, update the audio clock w/pts */
        if(pkt->pts != AV_NOPTS_VALUE) {
            is->audio_clock = av_q2d(is->audio_st->time_base)*pkt->pts;
        }
    }
}

/* 音频回调*/
void audio_callback(void *userdata, Uint8 *stream, int len) {
    
    VideoState *is = (VideoState *)userdata;
    int len1, audio_size;
    double pts;
    
    /* 清空sdl缓存中遗留的数据 */
    SDL_memset(stream, 0, len);
    
    while(len > 0) {
        /* 下标大于缓存大小 */
        if(is->audio_buf_index >= is->audio_buf_size) {
            /* We have already sent all our data; get more */
            /* 音频解码 */
            audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf), &pts);
            if(audio_size < 0) {
                /* 错误 */
                /* If error, output silence */
                is->audio_buf_size = 1024 * 2 * 2;
                memset(is->audio_buf, 0, is->audio_buf_size);
            } else {
                /* 同步音频 */
                audio_size = synchronize_audio(is, (int16_t *)is->audio_buf,
                                               audio_size, pts);
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if(len1 > len)
            len1 = len;
        
        /* 对音频数据进行混音，不要直接用memcpy。否则声音会失真 */
        SDL_MixAudio(stream,(uint8_t *)is->audio_buf + is->audio_buf_index, len1, SDL_MIX_MAXVOLUME);
        //memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

/* 刷新事件 */
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
/* 延迟刷新 */
static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

/* SDL 显示YUV数据 */
void video_display(VideoState *is) {
    
    SDL_Rect rect;
    VideoPicture *vp;
    float aspect_ratio;
    int w, h, x, y;
    int i;
    
    vp = &is->pictq[is->pictq_rindex];
    if(vp->bmp) {
        
        SDL_UpdateYUVTexture( texture, NULL,
                             vp->bmp->data[0], vp->bmp->linesize[0],
                             vp->bmp->data[1], vp->bmp->linesize[1],
                             vp->bmp->data[2], vp->bmp->linesize[2]);
        
        rect.x = 0;
        rect.y = 0;
        rect.w = is->video_ctx->width;
        rect.h = is->video_ctx->height;
        SDL_LockMutex(text_mutex);
        SDL_RenderClear( renderer );
        SDL_RenderCopy( renderer, texture, NULL, &rect);
        SDL_RenderPresent( renderer );
        SDL_UnlockMutex(text_mutex);
        
    }
}

void video_refresh_timer(void *userdata) {
    
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;
    double actual_delay, delay, sync_threshold, ref_clock, diff;
    
    /* 存在视屏流 */
    if(is->video_st) {
        if(is->pictq_size == 0) {
            schedule_refresh(is, 1);
            //fprintf(stderr, "no picture in the queue!!!\n");
        } else {
            //fprintf(stderr, "get picture from queue!!!\n");
            vp = &is->pictq[is->pictq_rindex];
            
            is->video_current_pts = vp->pts;
            is->video_current_pts_time = av_gettime();
            /* pts - last_pts */
            delay = vp->pts - is->frame_last_pts; /* the pts from last time */
            if(delay <= 0 || delay >= 1.0) {
                /* if incorrect delay, use previous one */
                delay = is->frame_last_delay;
            }
            /* save for next time */
            /* 保存下一个显示时间 */
            is->frame_last_delay = delay;
            is->frame_last_pts = vp->pts;
            
            /* update delay to sync to audio if not master source */
            /* 保证我们不会在视频为主时钟的时候也来同步视频 */
            if(is->av_sync_type != AV_SYNC_VIDEO_MASTER) {
                ref_clock = get_master_clock(is);
                diff = vp->pts - ref_clock;
                
                /* Skip or repeat the frame. Take delay into account
                 FFPlay still doesn't "know if this is the best guess." */
                sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
                if(fabs(diff) < AV_NOSYNC_THRESHOLD) {
                    /* vp->pts小于ref_clock,即当前播放视频帧慢 */
                    if(diff <= -sync_threshold) {
                        delay = 0;
                    } else if(diff >= sync_threshold) {
                        delay = 2 * delay;
                    }
                }
            }
            is->frame_timer += delay;
            /* computer the REAL delay */
            actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
            if(actual_delay < 0.010) {
                /* Really it should skip the picture instead */
                actual_delay = 0.010;
            }
            schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));
            
            /* show the picture! */
            video_display(is);
            
            /* update queue for next picture! */
            if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
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

void alloc_picture(void *userdata) {
    
    int ret;
    
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;
    
    vp = &is->pictq[is->pictq_windex];
    if(vp->bmp) {
        
        // we already have one make another, bigger/smaller
        avpicture_free(vp->bmp);
        free(vp->bmp);
        
        vp->bmp = NULL;
    }
    
    // Allocate a place to put our YUV image on that screen
    /* SDL_LockMutex :互斥锁(加锁) */
    SDL_LockMutex(text_mutex);
    
    vp->bmp = (AVPicture*)malloc(sizeof(AVPicture));
    /* 为图片的像素分配内存，并为其设置AVPicture字段 */
    ret = avpicture_alloc(vp->bmp, AV_PIX_FMT_YUV420P, is->video_ctx->width, is->video_ctx->height);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate temporary picture: %s\n", av_err2str(ret));
    }
    
    /* SDL_UnlockMutex : 互斥锁(解锁) */
    SDL_UnlockMutex(text_mutex);
    
    vp->width = is->video_ctx->width;
    vp->height = is->video_ctx->height;
    vp->allocated = 1;
    
}

int queue_picture(VideoState *is, AVFrame *pFrame, double pts) {
    
    VideoPicture *vp;
    
    /* wait until we have space for a new pic */
    /* SDL_LockMutex :互斥锁(加锁) */
    SDL_LockMutex(is->pictq_mutex);
    while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
          !is->quit) {
        /* SDL_CondWait :等待(线程阻塞) */
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    /* SDL_UnlockMutex : 互斥锁(解锁) */
    SDL_UnlockMutex(is->pictq_mutex);
    
    if(is->quit)
        return -1;
    
    // windex is set to 0 initially
    vp = &is->pictq[is->pictq_windex];
    
    /* allocate or resize the buffer! */
    /* 分配或调整缓冲区大小 */
    if(!vp->bmp ||
       vp->width != is->video_ctx->width ||
       vp->height != is->video_ctx->height) {
        
        vp->allocated = 0;
        alloc_picture(is);
        if(is->quit) {
            return -1;
        }
    }
    
    /* We have a place to put our picture on the queue */
    if(vp->bmp) {
        
        vp->pts = pts;
        
        // Convert the image into YUV format that SDL uses
        sws_scale(is->video_sws_ctx, (uint8_t const * const *)pFrame->data,
                  pFrame->linesize, 0, is->video_ctx->height,
                  vp->bmp->data, vp->bmp->linesize);
        
        /* now we inform our display thread that we have a pic ready */
        if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
            is->pictq_windex = 0;
        }
        /* SDL_LockMutex :互斥锁(加锁) */
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        /* SDL_UnlockMutex : 互斥锁(解锁) */
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

double synchronize_video(VideoState *is, AVFrame *src_frame, double pts) {
    
    double frame_delay;
    
    if(pts != 0) {
        /* if we have pts, set video clock to it */
        /* 如果我们有显示时间，就设置视频时钟 */
        is->video_clock = pts;
    } else {
        /* if we aren't given a pts, set it to the clock */
        /* 如果我们没有获得pts，请将其设置为时钟 */
        pts = is->video_clock;
    }
    /* update the video clock */
    frame_delay = av_q2d(is->video_ctx->time_base);
    /* if we are repeating a frame, adjust clock accordingly */
    /* 如果要重复一帧，请相应地调整时钟 */
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    is->video_clock += frame_delay;
    return pts;
}

/* 解码视屏线程 */
int decode_video_thread(void *arg) {
    VideoState *is = (VideoState *)arg;
    AVPacket pkt1, *packet = &pkt1;
    int frameFinished;
    AVFrame *pFrame;
    double pts;
    
    pFrame = av_frame_alloc();
    
    for(;;) {
        if(packet_queue_get(&is->videoq, packet, 1) < 0) {
            // means we quit getting packets
            break;
        }
        pts = 0;
        
        // Decode video frame
        avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet);
        
        /* 对解码后的AVFrame使用av_frame_get_best_effort_timestamp可以获取PTS */
        if((pts = av_frame_get_best_effort_timestamp(pFrame)) != AV_NOPTS_VALUE) {
        } else {
            pts = 0;
        }
        pts *= av_q2d(is->video_st->time_base);
        
        // Did we get a video frame?
        if(frameFinished) {
            /* 同步视屏 */
            pts = synchronize_video(is, pFrame, pts);
            if(queue_picture(is, pFrame, pts) < 0) {
                break;
            }
        }
        av_free_packet(packet);
    }
    av_frame_free(&pFrame);
    return 0;
}

int stream_component_open(VideoState *is, int stream_index) {
    
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx = NULL;
    AVCodec *codec = NULL;
    SDL_AudioSpec wanted_spec, spec;
    
    if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
        return -1;
    }
    
    codecCtx = avcodec_alloc_context3(NULL);
    
    /* 将AVCodecContext的成员复制到AVCodecParameters结构体*/
    int ret = avcodec_parameters_to_context(codecCtx, pFormatCtx->streams[stream_index]->codecpar);
    if (ret < 0)
        return -1;
    
    codec = avcodec_find_decoder(codecCtx->codec_id);
    if(!codec) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    
    
    if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
        
        // Set audio settings from codec info
        wanted_spec.freq = codecCtx->sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = 2;//codecCtx->channels;
        wanted_spec.silence = 0;
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = is;
        
        fprintf(stderr, "wanted spec: channels:%d, sample_fmt:%d, sample_rate:%d \n",
                2, AUDIO_S16SYS, codecCtx->sample_rate);
        
        if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
            fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
            return -1;
        }
        is->audio_hw_buf_size = spec.size;
    }
    
    if(avcodec_open2(codecCtx, codec, NULL) < 0) {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    
    switch(codecCtx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audioStream = stream_index;
            is->audio_st = pFormatCtx->streams[stream_index];
            is->audio_ctx = codecCtx;
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;
            memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
            packet_queue_init(&is->audioq);
            
            //Out Audio Param
            uint64_t out_channel_layout=AV_CH_LAYOUT_STEREO;
            
            //AAC:1024  MP3:1152
            int out_nb_samples= is->audio_ctx->frame_size;
            //AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
            
            int out_sample_rate=is->audio_ctx->sample_rate;
            int out_channels=av_get_channel_layout_nb_channels(out_channel_layout);
            //Out Buffer Size
            /*
             int out_buffer_size=av_samples_get_buffer_size(NULL,
             out_channels,
             out_nb_samples,
             AV_SAMPLE_FMT_S16,
             1);
             */
            
            //uint8_t *out_buffer=(uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE*2);
            int64_t in_channel_layout=av_get_default_channel_layout(is->audio_ctx->channels);
            
            struct SwrContext *audio_convert_ctx = swr_alloc_set_opts(NULL,// we're allocating a new context
                                                                      out_channel_layout,// out_ch_layout
                                                                      AV_SAMPLE_FMT_S16,// out_sample_fmt
                                                                      out_sample_rate,// out_sample_rate
                                                                      in_channel_layout,// in_ch_layout
                                                                      is->audio_ctx->sample_fmt,// in_sample_fmt
                                                                      is->audio_ctx->sample_rate,// in_sample_rate
                                                                      0,// log_offset
                                                                      NULL);// log_ctx

            /* 原版
            struct SwrContext *audio_convert_ctx;
            audio_convert_ctx = swr_alloc();
            swr_alloc_set_opts(audio_convert_ctx,// we're allocating a new context
                               out_channel_layout,// out_ch_layout
                               AV_SAMPLE_FMT_S16,// out_sample_fmt
                               out_sample_rate,// out_sample_rate
                               in_channel_layout,// in_ch_layout
                               is->audio_ctx->sample_fmt,// in_sample_fmt
                               is->audio_ctx->sample_rate,// in_sample_rate
                               0,// log_offset
                               NULL);// log_ctx
            fprintf(stderr, "swr opts: out_channel_layout:%lld, out_sample_fmt:%d, out_sample_rate:%d, in_channel_layout:%lld, in_sample_fmt:%d, in_sample_rate:%d",
                    out_channel_layout, AV_SAMPLE_FMT_S16, out_sample_rate, in_channel_layout, is->audio_ctx->sample_fmt, is->audio_ctx->sample_rate);
            swr_init(audio_convert_ctx);
            */
            fprintf(stderr, "swr opts: out_channel_layout:%lld, out_sample_fmt:%d, out_sample_rate:%d, in_channel_layout:%lld, in_sample_fmt:%d, in_sample_rate:%d",
            out_channel_layout, AV_SAMPLE_FMT_S16, out_sample_rate, in_channel_layout, is->audio_ctx->sample_fmt, is->audio_ctx->sample_rate);
            
            is->audio_swr_ctx = audio_convert_ctx;
            
            SDL_PauseAudio(0);
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->videoStream = stream_index;
            is->video_st = pFormatCtx->streams[stream_index];
            is->video_ctx = codecCtx;
            
            is->frame_timer = (double)av_gettime() / 1000000.0;
            is->frame_last_delay = 40e-3;
            is->video_current_pts_time = av_gettime();
            
            packet_queue_init(&is->videoq);
            is->video_sws_ctx = sws_getContext(is->video_ctx->width, is->video_ctx->height,
                                               is->video_ctx->pix_fmt, is->video_ctx->width,
                                               is->video_ctx->height, AV_PIX_FMT_YUV420P,
                                               SWS_BILINEAR, NULL, NULL, NULL
                                               );
            is->video_tid = SDL_CreateThread(decode_video_thread, "decode_video_thread", is);
            break;
        default:
            break;
    }
    return 0;
}

int demux_thread(void *arg) {
    
    int err_code;
    char errors[1024] = {0,};
    
    VideoState *is = (VideoState *)arg;
    AVFormatContext *pFormatCtx = NULL;
    AVPacket pkt1, *packet = &pkt1;
    
    int video_index = -1;
    int audio_index = -1;
    int i;
    
    is->videoStream=-1;
    is->audioStream=-1;
    
    global_video_state = is;
    
    /* open input file, and allocate format context */
    if ((err_code=avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)) < 0) {
        av_strerror(err_code, errors, 1024);
        fprintf(stderr, "Could not open source file %s, %d(%s)\n", is->filename, err_code, errors);
        return -1;
    }
    
    is->pFormatCtx = pFormatCtx;
    
    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return -1; // Couldn't find stream information
    
    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, is->filename, 0);
    
    // Find the first video stream
    
    for(i=0; i<pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO &&
           video_index < 0) {
            video_index=i;
        }
        if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO &&
           audio_index < 0) {
            audio_index=i;
        }
    }
    if(audio_index >= 0) {
        stream_component_open(is, audio_index);
    }
    if(video_index >= 0) {
        stream_component_open(is, video_index);
    }
    
    if(is->videoStream < 0 || is->audioStream < 0) {
        fprintf(stderr, "%s: could not open codecs\n", is->filename);
        goto fail;
    }
    
    //creat window from SDL
    win = SDL_CreateWindow("Media Player",
                           SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED,
                           is->video_ctx->width, is->video_ctx->height,
                           SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
     
    if(!win) {
        fprintf(stderr, "SDL: could not set video mode - exiting\n");
        exit(1);
    }
    
    renderer = SDL_CreateRenderer(win, -1, 0);
    
    //IYUV: Y + U + V  (3 planes)
    //YV12: Y + V + U  (3 planes)
    Uint32 pixformat= SDL_PIXELFORMAT_IYUV;
    
    //create texture for render
    texture = SDL_CreateTexture(renderer,
                                pixformat,
                                SDL_TEXTUREACCESS_STREAMING,
                                is->video_ctx->width,
                                is->video_ctx->height);
    
 
    // main decode loop
    for(;;) {
        if(is->quit) {
            break;
        }
        // seek stuff goes here
        if(is->audioq.size > MAX_AUDIOQ_SIZE ||
           is->videoq.size > MAX_VIDEOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }
        if(av_read_frame(is->pFormatCtx, packet) < 0) {
            if(is->pFormatCtx->pb->error == 0) {
                SDL_Delay(100); /* no error; wait for user input */
                continue;
            } else {
                break;
            }
        }
        // Is this a packet from the video stream?
        if(packet->stream_index == is->videoStream) {
            packet_queue_put(&is->videoq, packet);
        } else if(packet->stream_index == is->audioStream) {
            packet_queue_put(&is->audioq, packet);
        } else {
            av_free_packet(packet);
        }
    }
    /* all done - wait for it */
    while(!is->quit) {
        SDL_Delay(100);
    }
    
fail:
    if(1){
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    return 0;
}

/* 创建SDL2 */
int create_sdl2(int width, int height) {
    //creat window from SDL
    win = SDL_CreateWindow("Media Player",
                           SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED,
                           width,
                           height,
                           SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    
    if(!win) {
        fprintf(stderr, "SDL: could not set video mode - exiting\n");
        return -1;
    }
    
    renderer = SDL_CreateRenderer(win, -1, 0);
    
    //IYUV: Y + U + V  (3 planes)
    //YV12: Y + V + U  (3 planes)
    Uint32 pixformat = SDL_PIXELFORMAT_IYUV;
    
    //create texture for render
    texture = SDL_CreateTexture(renderer,
                                pixformat,
                                SDL_TEXTUREACCESS_STREAMING,
                                width,
                                height);
    return 0;
}

int media_player(char *url) {

    SDL_Event       event;
    VideoState      *is;

    is = av_mallocz(sizeof(VideoState));
    
    if(!url) {
        printf("url is null");
        return -1;
    }
    
    // Register all formats and codecs
    av_register_all();
    
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }
    
    /* 互斥结构体 */
    text_mutex = SDL_CreateMutex();
    
    av_strlcpy(is->filename, url, sizeof(is->filename));
    
    is->pictq_mutex = SDL_CreateMutex();
    /* 条件变量 */
    is->pictq_cond = SDL_CreateCond();
    
    schedule_refresh(is, 40);
    
    is->av_sync_type = DEFAULT_AV_SYNC_TYPE;
    is->parse_tid = SDL_CreateThread(demux_thread,"demux_thread", is);
    
    if(!is->parse_tid) {
        av_free(is);
        return -1;
    }
    
    for(;;) {
        SDL_WaitEvent(&event);
        switch(event.type) {
            case FF_QUIT_EVENT:
            case SDL_QUIT:
                is->quit = 1;
                SDL_Quit();
                return 0;
                break;
            case FF_REFRESH_EVENT:
                video_refresh_timer(event.user.data1);
                break;
            default:
                break;
        }
    }
    return 0;
}
