#include "stdafx.h"
#include "RenderFile.h"
#include "utility.h"
#include "SDL.h"

#define FF_ALLOCATE_OVERLAY		SDL_USEREVENT + 1
#define FF_DISPLAY_VIDOEPIC		SDL_USEREVENT + 2

// demux thread.
int SDLCALL DemuxThread(void* data);
// video decode thread.
int SDLCALL VideoDecodeThread(void* data);
// Audio data request.
void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len);

void AllocOverlay(void* data);
void DisplayPicture(void* data);
int QueueVideoPicture(VideoState* aVideoState, AVFrame* aFrame);

Uint32 SDLCALL refresh_callback(Uint32 interval, void *param)
{
	SDL_Event msg;
	msg.type = FF_DISPLAY_VIDOEPIC;
	msg.user.data1 = param;
	SDL_PushEvent(&msg);

	return 0;
}

void schedule_refresh(VideoState* vs, unsigned int interval)
{
	SDL_AddTimer(interval, refresh_callback, (void*)vs);
}

SDL_Surface* screen_surface = NULL;

int RenderAVFormatContext(AVFormatContext* aFmtCtx)
{
	VideoState vs;
	vs.fmtCtx = aFmtCtx;
	vs.audio_buf_size = vs.audio_buf_index = 0;
	vs.audioStream = vs.videoStream = NULL;
	vs.demux_tid = vs.video_tid = NULL;
	vs.video_pic_cond = NULL;
	vs.video_pic_mutex = NULL;
	vs.video_pic_rindex = vs.video_pic_windex = 0;
	vs.video_pic_size = 0;
	memset(&vs.audio_pkt, 0, sizeof(vs.audio_pkt));
	memset(vs.video_pics, 0, sizeof(vs.video_pics));
	vs.quit = 0;

	Init_PacketQueue(&vs.audio_pktq);
	Init_PacketQueue(&vs.video_pktq);

	strcpy(vs.filename, aFmtCtx->filename);

	unsigned int vStream = GetStreamIndexByMediaType(aFmtCtx, AVMEDIA_TYPE_VIDEO);
	unsigned int aStream = GetStreamIndexByMediaType(aFmtCtx, AVMEDIA_TYPE_AUDIO);

	AVCodec* video_decoder = NULL;
	AVCodec* audio_decoder = NULL;

	if (vStream >= 0)
	{
		vs.videoStream = vStream;
		vs.stream_video = aFmtCtx->streams[vStream];

		video_decoder = avcodec_find_decoder(aFmtCtx->streams[vStream]->codec->codec_id);
		if (video_decoder)
		{
			avcodec_open2(aFmtCtx->streams[vStream]->codec, video_decoder, NULL);
		}
	}
	if (aStream >= 0)
	{
		vs.audioStream = aStream;
		vs.stream_audio = aFmtCtx->streams[aStream];

		audio_decoder = avcodec_find_decoder(aFmtCtx->streams[aStream]->codec->codec_id);
		if (audio_decoder)
		{
			avcodec_open2(aFmtCtx->streams[aStream]->codec, audio_decoder, NULL);
		}

		AVCodecContext* codecFmt = aFmtCtx->streams[aStream]->codec;
		SDL_AudioSpec sas;
		sas.channels = codecFmt->channels;
		sas.freq = codecFmt->sample_rate;
		sas.format = AUDIO_S16SYS;//codecFmt->sample_fmt;
		sas.silence = 0;
		sas.samples = SDL_AUDIO_BUFFER_SIZE;
		sas.userdata = (void*)&vs;
		sas.callback = AudioCallback;
		SDL_OpenAudio(&sas, NULL);
		SDL_PauseAudio(0);
	}

	// Init SDL library.
	SDL_Init(SDL_INIT_AUDIO|SDL_INIT_VIDEO|SDL_INIT_TIMER);
	// Set up a video mode.
	screen_surface = SDL_SetVideoMode(vs.stream_video->codec->width, vs.stream_video->codec->height, 0, SDL_RESIZABLE);

	// create demux thread to demux audio/video packets.
	vs.demux_tid = SDL_CreateThread(DemuxThread, (void*)&vs);

	schedule_refresh(&vs, 40);

	// begin msg loop.
	SDL_Event msg;
	bool bQuit = false;
	while (SDL_WaitEvent(&msg) >= 0)
	{
		switch (msg.type)
		{
		case SDL_QUIT:
			SDL_Quit();
			bQuit = true;
			break;
		case FF_ALLOCATE_OVERLAY:
			{
				AllocOverlay(msg.user.data1);
			}
			break;
		case FF_DISPLAY_VIDOEPIC:
			{
				DisplayPicture(msg.user.data1);
			}
			break;
		case SDL_VIDEORESIZE:
			{
				SDL_SetVideoMode(msg.resize.w, msg.resize.h, 0, SDL_RESIZABLE);
			}
			break;
		case SDL_KEYUP:
			{
				//SDL_SetVideoMode(0, 0, 0, SDL_FULLSCREEN);
			}
			break;
		default:
			break;
		}

		if (bQuit)
			break;
	}

	if (video_decoder)
		avcodec_close(aFmtCtx->streams[vStream]->codec);

	if (audio_decoder)
		avcodec_close(aFmtCtx->streams[aStream]->codec);

	return 0;
}

void AllocOverlay(void* data)
{
	VideoState* vs = (VideoState*)data;

	if (vs->video_pic_windex == vs->video_pic_rindex-1)
		return;

	VideoPicture* vp = &vs->video_pics[vs->video_pic_windex];

	if (vp->overlaybmp)
	{
		SDL_FreeYUVOverlay(vp->overlaybmp);
	}

	SDL_LockMutex(vs->video_pic_mutex);

	vp->overlaybmp = SDL_CreateYUVOverlay(vs->stream_video->codec->width, vs->stream_video->codec->height,
		SDL_YV12_OVERLAY, screen_surface);
	vp->width = vs->stream_video->codec->width;
	vp->height = vs->stream_video->codec->height;
	vp->allocated = 1;
	SDL_CondSignal(vs->video_pic_cond);
	SDL_UnlockMutex(vs->video_pic_mutex);
}

void DisplayPicture(void* data)
{
	VideoState* vs = (VideoState*)data;

	if (vs)
	{
/*		SDL_LockMutex(vs->video_pic_mutex);
		// wait for one available picture to display.
		while (vs->video_pic_size <= 0 && !vs->quit) {
			SDL_CondWait(vs->video_pic_cond, vs->video_pic_mutex);
		}
		SDL_UnlockMutex(vs->video_pic_mutex);*/
		if (vs->video_pic_size <= 0)
		{
			schedule_refresh((VideoState*)data, 1);
			return;
		}

		if (vs->video_pic_size > 0 && vs->video_pic_rindex < vs->video_pic_size&& !vs->quit)
		{
			SDL_LockMutex(vs->video_pic_mutex);
			VideoPicture* vp = &vs->video_pics[vs->video_pic_rindex];
			if (vp->overlaybmp)
			{
				SDL_Rect rc;
				rc.x = rc.y = 0;
				rc.w = vp->width;
				rc.h = vp->height;
				SDL_DisplayYUVOverlay(vp->overlaybmp, &rc);
				vp->allocated = 0;
			}

			vs->video_pic_rindex++;
			if (vs->video_pic_rindex >= vs->video_pic_size)
				vs->video_pic_rindex = 0;

			vs->video_pic_size--;
			SDL_CondSignal(vs->video_pic_cond);
			SDL_UnlockMutex(vs->video_pic_mutex);
		}
	}
}

int SDLCALL DemuxThread(void* data)
{
	VideoState* vs = (VideoState*)data;

	AVPacket packet;

	while (av_read_frame(vs->fmtCtx, &packet) >= 0)
	{
		if (vs->audioStream == packet.stream_index)
		{
			packet_queue_put(&vs->audio_pktq, &packet);
		}
		else if (vs->videoStream == packet.stream_index)
		{
			packet_queue_put(&vs->video_pktq, &packet);

			if (!vs->video_tid)
			{
				vs->video_tid = SDL_CreateThread(VideoDecodeThread, data);
				vs->video_pic_cond = SDL_CreateCond();
				vs->video_pic_mutex = SDL_CreateMutex();
			}
		}
		else
		{
			av_free_packet(&packet);
		}

		//av_free_packet(&packet);
		memset(&packet, 0, sizeof(packet));
	}

	return 0;
}

int SDLCALL VideoDecodeThread(void* data)
{
	VideoState* vs = (VideoState*)data;

	if (vs)
	{
		AVCodecContext* codecCtx = vs->stream_video->codec;
		AVPacket packet;
		AVFrame* frame = avcodec_alloc_frame();

		while (packet_queue_get(&vs->video_pktq, &packet, 1) >= 0)
		{
			int got_pic = 0;
			if (avcodec_decode_video2(codecCtx, frame, &got_pic, &packet) >= 0)
			{
				if (got_pic)
				{
					QueueVideoPicture(vs, frame);
				}

				if (codecCtx->refcounted_frames > 0)
					av_frame_unref(frame);
			}

			av_free_packet(&packet);
		}

		//av_frame_free(&frame);
	}

	return 0;
}

void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len)
{
	if (!userdata) return;

	VideoState* vs = (VideoState*)userdata;
	AVPacket packet;
	AVFrame* frame = av_frame_alloc();
	AVCodecContext* codecCtx = vs->stream_audio->codec;

	int len1 = len;

	for (; ;)
	{
		if (vs->audio_buf_size > 0)
		{
			if (vs->audio_buf_size > len1)
			{
				memcpy(stream, vs->audio_buf + vs->audio_buf_index, len1);

				vs->audio_buf_size -= len1;
				vs->audio_buf_index += len1;
				break;
			}
			else if (len1 > 0)
			{
				memcpy(stream, vs->audio_buf + vs->audio_buf_index, vs->audio_buf_size);
				len1 -= vs->audio_buf_size;
				stream += vs->audio_buf_size;
				vs->audio_buf_size = 0;
				vs->audio_buf_index = 0;
			}
			else
			{
				break;
			}
		}

		int got_ptr = 0;
		AVPacket* pkt = NULL;

		if (vs->audio_pkt.data)
		{
			pkt = &vs->audio_pkt;
		}
		else if (packet_queue_get(&vs->audio_pktq, &packet, true) >= 0)
		{
			vs->audio_pkt = packet;
			pkt = &vs->audio_pkt;
		}
		else
		{
			memset(stream+len-len1, 0, len1);
			break;
		}

		int frame_length = avcodec_decode_audio4(codecCtx, frame, &got_ptr, pkt);

		if (!got_ptr)
		{
			av_free_packet(&vs->audio_pkt);
			memset(&vs->audio_pkt, 0, sizeof(vs->audio_pkt));
			continue;
		}

		if (frame_length > 0)
		{
			pkt->data += frame_length;
			pkt->size -= frame_length;
			if (frame_length <= sizeof(vs->audio_buf))
			{
				memcpy(vs->audio_buf, frame->data, frame_length);
				vs->audio_buf_size = frame_length;
			}
			else
			{
				int max_buf_length = sizeof(vs->audio_buf);
				memcpy(vs->audio_buf, frame->data, max_buf_length);
				vs->audio_buf_size = max_buf_length;
			}

			vs->audio_buf_index = 0;

			if (codecCtx->refcounted_frames > 0)
				av_frame_unref(frame);
		}
		else
		{
			break;
		}
	}

	//av_frame_free(&frame);
	avcodec_free_frame(&frame);
}

int QueueVideoPicture(VideoState* aVideoState, AVFrame* aFrame)
{
	SDL_LockMutex(aVideoState->video_pic_mutex);

	//video picture buffer full. Wait to consume.
	while (aVideoState->video_pic_size >= VIDEO_PICTURE_QUEUE_SIZE &&
			!aVideoState->quit) {
				SDL_CondWait(aVideoState->video_pic_cond, aVideoState->video_pic_mutex);
	}

	SDL_UnlockMutex(aVideoState->video_pic_mutex);

	if (aVideoState->quit)
		return -1;

	VideoPicture* vp = &aVideoState->video_pics[aVideoState->video_pic_windex];
	if (!vp->overlaybmp ||
		aFrame->width != vp->width ||
		aFrame->height != vp->height)
	{
		SDL_Event msg;
		msg.type = FF_ALLOCATE_OVERLAY;
		msg.user.data1 = (void*)aVideoState;
		int result = SDL_PushEvent(&msg);

		SDL_LockMutex(aVideoState->video_pic_mutex);
		if (!vp->allocated && !aVideoState->quit) {
			SDL_CondWait(aVideoState->video_pic_cond, aVideoState->video_pic_mutex);
		}

		SDL_UnlockMutex(aVideoState->video_pic_mutex);

		if (aVideoState->quit)
			return -1;
	}

	SwsContext* sws = NULL;
	if (vp->overlaybmp)
	{
		AVPicture picture;
		SDL_LockYUVOverlay(vp->overlaybmp);

		AVPixelFormat pixfmt = AV_PIX_FMT_YUV420P;

		picture.data[0] = vp->overlaybmp->pixels[0];
		picture.data[1] = vp->overlaybmp->pixels[2];
		picture.data[2] = vp->overlaybmp->pixels[1];
		picture.linesize[0] = vp->overlaybmp->pitches[0];
		picture.linesize[1] = vp->overlaybmp->pitches[2];
		picture.linesize[2] = vp->overlaybmp->pitches[1];

		sws = sws_getCachedContext(sws, aFrame->width, aFrame->height, (AVPixelFormat)aFrame->format,
			vp->width, vp->height, pixfmt, SWS_BICUBIC, NULL, NULL, NULL);
		if (sws)
		{
			sws_scale(sws, aFrame->data, aFrame->linesize, 0, aFrame->height, picture.data, picture.linesize);
		}
		
		SDL_UnlockYUVOverlay(vp->overlaybmp);

		aVideoState->video_pic_windex++;
		if (aVideoState->video_pic_windex == VIDEO_PICTURE_QUEUE_SIZE) {
			aVideoState->video_pic_windex = 0;
		}

		SDL_LockMutex(aVideoState->video_pic_mutex);
		aVideoState->video_pic_size++;
		SDL_CondSignal(aVideoState->video_pic_cond);
		SDL_UnlockMutex(aVideoState->video_pic_mutex);
	}

	return 0;
}
