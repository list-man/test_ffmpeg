#ifndef AV_UTILITY_H
#define AV_UTILITY_H
#endif

#include "SDL_stdinc.h"
#include "SDL_audio.h"
#include "SDL_cdrom.h"
#include "SDL_cpuinfo.h"
#include "SDL_endian.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_loadso.h"
#include "SDL_mutex.h"
#include "SDL_rwops.h"
#include "SDL_thread.h"
#include "SDL_timer.h"
#include "SDL_video.h"
#include "SDL_version.h"
#include "SDL_video.h"
#include "SDL_thread.h"

#define VIDEO_PICTURE_QUEUE_SIZE	125	//25*5
#define SDL_AUDIO_BUFFER_SIZE	1024
#define AVCODEC_MAX_AUDIO_FRAME_SIZE	176400

int GetStreamIndexByMediaType(AVFormatContext* aFmtCtx, AVMediaType aMt);

typedef struct tagVideoPicture
{
	SDL_Overlay* overlaybmp;
	int width, height;
	int allocated;
	double pts;
}VideoPicture, *LPVideoPicture;

typedef struct tagPacketQueue {
	AVPacketList*	first;
	AVPacketList*	last;

	int	nb_packets;
	int size;

	SDL_mutex*	mutex;	//mutex for thread sync.
	SDL_cond*	cond;	//condition variable.
}PacketQueue, *LPPacketQueue;

void Init_PacketQueue(PacketQueue* pktQ);
int packet_queue_put(PacketQueue* pktq, AVPacket* packet);
int packet_queue_get(PacketQueue* pktq, AVPacket* packet, bool blockOrnot);
void packet_queue_flush(PacketQueue* pktq);
AVPacket* get_flush_pkt();

typedef struct tagVideoState
{
	AVFormatContext		*fmtCtx;
	int					videoStream, audioStream;		// stream index for audio/video.
	int					av_sync_type;
	double				external_clock;					//external clock base.
	int64_t				external_clock_time;

	double				audio_clock;
	AVStream			*stream_audio;
	PacketQueue			audio_pktq;						//audio packets queue.
	uint8_t				audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];	//缓存未
	unsigned int		audio_buf_size;					//audio_buf中未使用数据的大小
	unsigned int		audio_buf_index;
	AVPacket			audio_pkt;
	int					audio_hw_buf_size;
	double				audio_diff_cum;
	double				audio_diff_avg_coef;
	double				audio_diff_threshold;
	int					audio_diff_avg_count;
	double				frame_timer;		//下一个渲染Picture的当前microsecond，用于渲染下一个Frame时候计算下下个Frame的actual_delay
	double				frame_last_pts;		//最后一个显示的Frame的pts
	double				frame_last_delay;
	double				video_clock;

	PacketQueue			video_pktq;
	AVStream			*stream_video;
	VideoPicture		video_pics[VIDEO_PICTURE_QUEUE_SIZE];
	int					video_pic_size;
	int					video_pic_rindex;		// read index of video_pics array.
	int					video_pic_windex;		// write index of video_pics array.
	double				video_current_pts;
	int64_t				video_current_pts_time;	//time (av_gettime) at which we updated video_current_pts - used to have running video pts

	SDL_mutex			*video_pic_mutex;		//mutex for video queue sync.
	SDL_cond			*video_pic_cond;		//condition variable for video event.

	SDL_Thread			*demux_tid;
	SDL_Thread			*video_tid;			//video process thread.

	char				filename[1024];	//input file name, this should be a local file or remote file(e.g. file on shared disk or a http stream).
	int					quit;		//signal quit or not.

	int					seek_req;
	int					seek_flags;
	int64_t				seek_pos;
}VideoState, *LPVideoState;

enum
{
	AV_SYNC_AUDIO_MASTER,
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_MASTER,
};

// round an integer.
int rint(int x);
double get_audio_clock(VideoState* vs);
double get_video_clock(VideoState* vs);
double get_external_clock(VideoState* vs);
double get_master_clock(VideoState* vs);
