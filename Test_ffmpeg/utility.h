#ifndef AV_UTILITY_H
#define AV_UTILITY_H
#endif

#define VIDEO_PICTURE_QUEUE_SIZE	1

#define SDL_AUDIO_BUFFER_SIZE	1024
#define AVCODEC_MAX_AUDIO_FRAME_SIZE	176400

int GetStreamIndexByMediaType(AVFormatContext* aFmtCtx, AVMediaType aMt);

typedef struct tagVideoPictrue
{
	SDL_Overlay* overlaybmp;
	int width, height;
	int allocated;
};

typedef struct tagPacketQueue {
	AVPacketList*	first;
	AVPacketList*	last;

	int	nb_packetSize;
	int size;

	SDL_mutex*	mutex;	//mutex for thread sync.
	SDL_cond*	cond;	//condition variable.
}PacketQueue, *LPPacketQueue;

int packet_queue_put(PacketQueue* pktq, AVPacket* packet)
{
	if (!pktq || !packet)
		return -1;

	if (av_dup_packet(packet) < 0)
		return -1;

	AVPacketList* pktl = av_malloc(sizeof(AVPacketList));
	if (!pktl)
	{
		printf("out of memory to hold AVPacketList!\n");
		return -1;
	}

	pktl->next = NULL;
	pktl->pkt = *packet;

	SDL_LockMutex(pktq->mutex);

	if (!pktq->first){
		pktq->first = pktl;
	}
	else {
		pktq->last->next = pktl;
	}

	pktq->nb_packets++;
	pktq->size += packet->size;

	pktq->last = pktl;

	//signal it before unlock? yes. SDL_ConWait will unlock the mutex, otherwise SDL_ConWait will re-locked
	SDL_CondSignal(pktq->cond);
	SDL_UnlockMutex(pktq->mutex);

	return 0;
}

int packet_queue_get(PacketQueue* pktq, AVPacket* packet, bool blockOrnot)
{
	if (!pktq || !packet)
	{
		return -1;
	}

	if (pktq->first)
	{
		SDL_LockMutex(pktq->mutex);

		*packet = pktq->first;

		pktq->nb_packets--;
		pktq->size -= pktq->first->pkt.size;

		pktq->first = pktq->first->next;

		SDL_UnlockMutex(pktq->mutex);
	}
	else if (blockOrnot)
	{
		SDL_CondWait(pktq->cond, pktq->mutex);
	}
	else
	{
		return -1;
	}

	return 0;
}

void Init_PacketQueue(PacketQueue* pktQ)
{
	if (pktQ)
	{
		pktQ->first = pktQ->last = NULL;
		pktQ->nb_packets = pktQ->size = 0;

		pktQ->mutex = SDL_CreateMutex();
		pktQ->cond = SDL_CreateCond();
	}
}

typedef struct tagVideoState
{
	AVCodecContext		*codecCtx;
	int					videoStream, audioStream;
	AVStream			*stream_audio;
	PacketQueue			audio_pktq;
	uint8_t				audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
	unsigned int		audio_buf_size;
	unsigned int		audio_buf_index;
	AVPacket			audio_pkt;

	PacketQueue			video_pktq;
	AVStream			*stream_video;
	VideoPicture		video_pics[VIDEO_PICTURE_QUEUE_SIZE];
	int					video_pic_size;
	int					video_pic_rindex;		// read index of video_pics array.
	int					video_pic_windex;		// write index of video_pics array.

	SDL_mutex			*video_pic_mutex;		//mutex for video queue sync.
	SDL_cond			*video_pic_cond;		//condition variable for video event.

	SDL_Thread			*parse_tid;
	SDL_Thread			*video_tid;			//video process thread.

	char				filename[1024];	//input file name, this should be a local file or remote file(e.g. file on shared disk or a http stream).
	int					quit;		//signal quit or not.
}VideoState, *LPVideoState;