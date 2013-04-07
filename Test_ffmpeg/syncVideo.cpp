#include "stdafx.h"
#include "syncVideo.h"
#include "utility.h"
#include "SDL.h"

namespace _Sync
{

#define FF_ALLOCATE_OVERLAY		SDL_USEREVENT + 1
#define FF_DISPLAY_VIDOEPIC		SDL_USEREVENT + 2

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20

#define DEFAULT_AV_SYNC_TYPE AV_SYNC_VIDEO_MASTER

// demux thread.
int SDLCALL DemuxThread(void* data);
// video decode thread.
int SDLCALL VideoDecodeThread(void* data);
// Audio data request.
void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len);

void AllocOverlay(void* data);
void DisplayPicture(VideoState* vs);
void video_refresh_timer(void* userdata);
int QueueVideoPicture(VideoState* aVideoState, AVFrame* aFrame, double pts);
double synchronize_video(VideoState* vs, AVFrame* src_frame, double pts);
int synchronize_audio(VideoState* vs, short* samples, int samples_size, double pts);
void stream_seek(VideoState* vs, int64_t pos, int rel);

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
uint64_t global_video_pkt_pts = AV_NOPTS_VALUE;

int private_get_buffer2(struct AVCodecContext *s, AVFrame *frame, int flags)
{
	int result = avcodec_default_get_buffer2(s, frame, flags);
	uint64_t* pts = (uint64_t*)av_malloc(sizeof(uint64_t));
	*pts = global_video_pkt_pts;
	frame->opaque = pts;

	return result;
}

void private_release_buffer(struct AVCodecContext* c, AVFrame* pic)
{
	if (pic) av_freep(&pic->opaque);
	avcodec_default_release_buffer(c, pic);
}

int stream_open(VideoState* vs, int streamIndex)
{
	AVFormatContext* fmtContext = vs->fmtCtx;
	if (streamIndex < 0 || streamIndex >= fmtContext->nb_streams)
		return -1;

	AVCodecContext* codecCtx = fmtContext->streams[streamIndex]->codec;

	AVCodec* codec = avcodec_find_decoder(codecCtx->codec_id);
	if (!codec) return -1;
	avcodec_open2(codecCtx, codec, NULL);

	if (codecCtx->codec_type == AVMEDIA_TYPE_AUDIO)
	{
		vs->audioStream = streamIndex;
		vs->stream_audio = fmtContext->streams[streamIndex];
		vs->audio_buf_size = 0;
		vs->audio_buf_index = 0;
		vs->audio_hw_buf_size = 0;
		memset(&vs->audio_pkt, 0, sizeof(vs->audio_pkt));
		Init_PacketQueue(&vs->audio_pktq);

		//averaging filter for audio sync.
		vs->audio_diff_avg_coef = exp(log(0.01 / AUDIO_DIFF_AVG_NB));
		vs->audio_diff_avg_count = 0;
		// Correct audio only if larger error than this.
		vs->audio_diff_threshold = 2.0 * SDL_AUDIO_BUFFER_SIZE / codecCtx->sample_rate;

		SDL_AudioSpec wanted_spec, spec;
		wanted_spec.channels = codecCtx->channels;
		wanted_spec.freq = codecCtx->sample_rate;
		wanted_spec.format = AUDIO_S16SYS;//codecFmt->sample_fmt;
		wanted_spec.silence = 0;
		wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		wanted_spec.userdata = (void*)vs;
		wanted_spec.callback = AudioCallback;
		SDL_OpenAudio(&wanted_spec, &spec);

		vs->audio_hw_buf_size = spec.size;
		SDL_PauseAudio(0);
	}
	else if (codecCtx->codec_type == AVMEDIA_TYPE_VIDEO)
	{
		vs->videoStream = streamIndex;
		vs->stream_video = fmtContext->streams[streamIndex];
		Init_PacketQueue(&vs->video_pktq);

		vs->frame_timer = (double)av_gettime() / 1000000.0;
		vs->frame_last_delay = 40e-3;
		vs->frame_last_pts = 0;
		vs->video_current_pts_time = av_gettime();
		codecCtx->get_buffer2 = private_get_buffer2;
		codecCtx->release_buffer = private_release_buffer;
		//vs.stream_video->codec->release_buffer = private_release_buffer;
	}
}

int SyncVideo(AVFormatContext* aFmtCtx)
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
	vs.av_sync_type = DEFAULT_AV_SYNC_TYPE;
	vs.seek_req = 0;
	memset(&vs.audio_pkt, 0, sizeof(vs.audio_pkt));
	memset(vs.video_pics, 0, sizeof(vs.video_pics));
	vs.quit = 0;

	strcpy(vs.filename, aFmtCtx->filename);

	AVPacket* flu_pkt = get_flush_pkt();
	av_init_packet(flu_pkt);
	flu_pkt->data = (uint8_t *)"FLUSH";

	unsigned int vStream = GetStreamIndexByMediaType(aFmtCtx, AVMEDIA_TYPE_VIDEO);
	unsigned int aStream = GetStreamIndexByMediaType(aFmtCtx, AVMEDIA_TYPE_AUDIO);

	stream_open(&vs, vStream);	//初始化视频流
	stream_open(&vs, aStream);	//初始化音频流

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
	while (!bQuit)
	{
		SDL_WaitEvent(&msg);

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
				video_refresh_timer(msg.user.data1);
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
		case SDL_KEYDOWN:
			{
				int increments = 0;
				switch (msg.key.keysym.sym)
				{
				case SDLK_LEFT:
					increments -= 5;
					break;
				case SDLK_RIGHT:
					increments += 5;
					break;
				}

				if (increments)
				{
					vs.seek_req = 1;
					double pos = get_master_clock(&vs);
					pos += increments;
					stream_seek(&vs, (int64_t)(pos * AV_TIME_BASE), increments);
				}
			}
			break;
		default:
			break;
		}

		if (bQuit)
			break;
	}

	if (vStream >= 0)
		avcodec_close(aFmtCtx->streams[vStream]->codec);

	if (aStream >= 0)
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

	vp->overlaybmp = SDL_CreateYUVOverlay(vs->stream_video->codec->width, vs->stream_video->codec->height,
		SDL_YV12_OVERLAY, screen_surface);
	vp->width = vs->stream_video->codec->width;
	vp->height = vs->stream_video->codec->height;

	SDL_LockMutex(vs->video_pic_mutex);
	vp->allocated = 1;
	SDL_CondSignal(vs->video_pic_cond);
	SDL_UnlockMutex(vs->video_pic_mutex);
}

void DisplayPicture(VideoState* vs)
{
	SDL_Rect rect;
	VideoPicture* vp = NULL;
	AVPicture pict;
	float aspect_ratio;
	int w, h, x, y;

	vp = &vs->video_pics[vs->video_pic_rindex];
	if (vp->overlaybmp)
	{
		if (vs->stream_video->codec->sample_aspect_ratio.num == 0) {
			aspect_ratio = 0;
		} else {
			aspect_ratio = av_q2d(vs->stream_video->codec->sample_aspect_ratio) *
				vs->stream_video->codec->width / vs->stream_video->codec->height;
		}

		if (aspect_ratio <= 0.0) {
			aspect_ratio = (float)vs->stream_video->codec->width /
				(float)vs->stream_video->codec->height;
		}

		h = screen_surface->h;
		w = ((int)rint(h * aspect_ratio)) & -3;
		if (w > screen_surface->h) {
			w = screen_surface->w;
			h = ((int)rint(w / aspect_ratio)) & -3;
		}

		x = (screen_surface->w - w) / 2;
		y = (screen_surface->h - h) / 2;

		rect.x = x;
		rect.y = y;
		rect.w = w;
		rect.h = h;

		SDL_DisplayYUVOverlay(vp->overlaybmp, &rect);
	}
}

void video_refresh_timer(void* userdata)
{
	VideoState* vs = (VideoState*)userdata;
	VideoPicture* vp = NULL;
	double actual_delay, delay, sync_threshold, ref_clock, diff;

	if (!vs)	return;

	if (vs->stream_video)
	{
		if (vs->video_pic_size == 0)
		{
			schedule_refresh(vs, 1);
		}
		else
		{
			vp = &vs->video_pics[vs->video_pic_rindex];
			delay = vp->pts - vs->frame_last_pts; //pts from the last time.
			if (delay <= 0 || delay >= 1.0) {
				// if meet incorrect delay, use the previous one.
				delay = vs->frame_last_delay;
			}

			// Save for next time.
			vs->frame_last_delay = delay;
			vs->frame_last_pts = vp->pts;

			if (vs->av_sync_type != AV_SYNC_VIDEO_MASTER)
			{
				ref_clock = get_master_clock(vs);
				diff = vp->pts - ref_clock;

				// skip or repeat the frame. Take delay into account.
				sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
				if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
					if (diff <= -sync_threshold) {
						delay = 0;
					}
					else if (diff >= sync_threshold) {
						delay = 2 * delay;
					}
				}
			}

			vs->frame_timer += delay;
			// compute the real delay.
			actual_delay = vs->frame_timer - (av_gettime() / 1000000.0);
			if (actual_delay < 0.010) {
				// really it should skip the picture instead.
				actual_delay = 0.010;
			}

			schedule_refresh(vs, (int)(actual_delay * 1000 + 0.5));

			//SDL_LockMutex(vs->video_pic_mutex);
			DisplayPicture(vs);

			// update queue for next picture!
			if (++vs->video_pic_rindex >= VIDEO_PICTURE_QUEUE_SIZE) {
				vs->video_pic_rindex = 0;
			}

			SDL_LockMutex(vs->video_pic_mutex);
			vs->video_pic_size--;
			SDL_CondSignal(vs->video_pic_cond);
			SDL_UnlockMutex(vs->video_pic_mutex);
		}
	}
	else
	{
		schedule_refresh(vs, 100);
	}
}

int SDLCALL DemuxThread(void* data)
{
	VideoState* vs = (VideoState*)data;

	AVPacket packet;

	while (true)
	{
		if (vs->seek_req)
		{
			int stream_index = -1;
			int64_t seek_target = vs->seek_pos;
			if (vs->videoStream >= 0) stream_index = vs->videoStream;
			else if (vs->audioStream >= 0) stream_index = vs->audioStream;

			if (stream_index >= 0)
			{
				//seek_target = av_rescale_q(seek_target, AV_TIME_BASE_Q, vs->fmtCtx->streams[stream_index]->time_base);
			}

			if (!av_seek_frame(vs->fmtCtx, stream_index, seek_target, vs->seek_flags))
			{
				// error.
			}
			else
			{
				if (vs->audioStream >= 0)
				{
					packet_queue_flush(&vs->audio_pktq);
					packet_queue_put(&vs->audio_pktq, get_flush_pkt());
				}
				if (vs->videoStream >= 0)
				{
					packet_queue_flush(&vs->video_pktq);
					packet_queue_put(&vs->video_pktq, get_flush_pkt());
				}
			}

			vs->seek_req = 0;
		}

		if (vs->audio_pktq.size > MAX_AUDIOQ_SIZE ||
			vs->video_pktq.size > MAX_VIDEOQ_SIZE)
		{
			SDL_Delay(10);
			continue;
		}

		if (av_read_frame(vs->fmtCtx, &packet) < 0)
			break;
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
		double pts = 0;

		while (packet_queue_get(&vs->video_pktq, &packet, 1) >= 0)
		{
			if (packet.data == get_flush_pkt()->data)
			{
				avcodec_flush_buffers(vs->stream_video->codec);
				continue;
			}

			pts = 0;
			int got_pic = 0;
			global_video_pkt_pts = packet.pts;
			if (avcodec_decode_video2(codecCtx, frame, &got_pic, &packet) >= 0)
			{
				if (packet.dts == AV_NOPTS_VALUE && frame->opaque && *(uint64_t*)frame->opaque != AV_NOPTS_VALUE)
				{
					pts = *(uint64_t*)frame->opaque;
				}
				else if (packet.dts != AV_NOPTS_VALUE)
				{
					pts = packet.dts;
				}
				else
				{
					pts = 0;
				}

				pts *= av_q2d(vs->stream_video->time_base);

				if (got_pic)
				{
					pts = synchronize_video(vs, frame, pts);
					QueueVideoPicture(vs, frame, pts);
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

double synchronize_video(VideoState* vs, AVFrame* src_frame, double pts)
{
	double frame_delay = 0;

	if (pts != 0) {
		// if we have pts. set video clock to it.
		vs->video_clock = pts;
	}
	else {
		pts = vs->video_clock;
	}

	// update the video clock.
	frame_delay = av_q2d(vs->stream_video->codec->time_base);
	// if we are repeating a frame, adjust clock according.
	frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
	vs->video_clock += frame_delay;
	return pts;
}

int synchronize_audio(VideoState* vs, short* samples, int samples_size, double pts)
{
	int n;
	double ref_clock;
	n = 2 * vs->stream_audio->codec->channels;

	if (vs->av_sync_type != AV_SYNC_AUDIO_MASTER)
	{
		double diff, avg_diff;
		int wanted_size, min_size, max_size, nb_samples;

		ref_clock = get_master_clock(vs);
		diff = get_audio_clock(vs) - ref_clock;

		if (diff < AV_NOSYNC_THRESHOLD)
		{
			vs->audio_diff_cum = diff + vs->audio_diff_avg_coef * vs->audio_diff_cum;
			if (vs->audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
			{
				vs->audio_diff_avg_count ++;
			}
			else
			{
				avg_diff = vs->audio_diff_cum * (1.0 - vs->audio_diff_avg_coef);
				if (fabs(avg_diff) >= vs->audio_diff_threshold)
				{
					wanted_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					max_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					if (wanted_size < min_size)
					{
						wanted_size = min_size;
					}
					else if (wanted_size > max_size)
					{
						wanted_size = max_size;
					}

					if (wanted_size < samples_size)
					{
						samples_size = wanted_size;
					}
					else if (wanted_size > samples_size)
					{
						uint8_t *sample_end, *q;
						int nb;

						// add samples by copying final sample
						nb = (samples_size - wanted_size);
						sample_end = (uint8_t*)samples + samples_size - n;
						q = sample_end + n;
						while (nb > 0)
						{
							memcpy(q, sample_end, n);
							q += n;
							nb -= n;
						}

						samples_size = wanted_size;
					}
				}
			}
		}
		else
		{
			vs->audio_diff_avg_count = 0;
			vs->audio_diff_cum = 0;
		}
	}

	return samples_size;
}

/*void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len)
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
}*/

int audio_decode_frame(VideoState* vs, uint8_t* audio_buf, int buf_size, double *pts_ptr)
{
	int len1, data_size, n;
	AVPacket* pkt = NULL;
	double pts;
	AVFrame* frame = av_frame_alloc();

	for (; ;)
	{
		pkt = &vs->audio_pkt;
		while (pkt && pkt->size > 0)
		{
			data_size = buf_size;
			int got_ptr = 0;
			len1 = avcodec_decode_audio4(vs->stream_audio->codec, frame, &got_ptr, pkt);
			
			if (len1 < 0)
			{
				av_free_packet(pkt);
				memset(&vs->audio_pkt, 0, sizeof(vs->audio_pkt));
				//vs->audio_pkt_size = 0;
				break;
			}
			else if (got_ptr)
			{
				int plane_size;
				data_size = av_samples_get_buffer_size(&plane_size, vs->stream_audio->codec->channels,
					frame->nb_samples, vs->stream_audio->codec->sample_fmt, 1);
			}

			pkt->data += len1;
			pkt->size -= len1;
			if (data_size <= 0)
			{
				continue;
			}

			pts = vs->audio_clock;
			*pts_ptr = pts;
			n = 2 * vs->stream_audio->codec->channels;
			vs->audio_clock += (double)data_size / (double)(n * vs->stream_audio->codec->sample_rate);

			return data_size;
		}

		if (pkt->data)
		{
			av_free_packet(pkt);
			memset(&vs->audio_pkt, 0, sizeof(vs->audio_pkt));
		}

		if (vs->quit)
			return -1;

		if (packet_queue_get(&vs->audio_pktq, &vs->audio_pkt, 1) < 0)
		{
			return -1;
		}

		if (pkt->data == get_flush_pkt()->data)
		{
			avcodec_flush_buffers(vs->stream_audio->codec);
			continue;
		}

		if (pkt->pts != AV_NOPTS_VALUE)
		{
			vs->audio_clock = av_q2d(vs->stream_audio->time_base) * pkt->pts;
		}
	}

	return -1;
}

void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len)
{
	VideoState* vs = (VideoState*)userdata;
	int len1, audio_size;
	double pts;

	while (len > 0) {
		if (vs->audio_buf_index >= vs->audio_buf_size)
		{
			audio_size = audio_decode_frame(vs, vs->audio_buf, sizeof(vs->audio_buf), &pts);
			if (audio_size < 0)
			{
				vs->audio_buf_size = 1024;
				memset(vs->audio_buf, 0, vs->audio_buf_size);
			}
			else
			{
				audio_size = synchronize_audio(vs, (int16_t*)vs->audio_buf, audio_size, pts);
				vs->audio_buf_size = audio_size;
			}

			vs->audio_buf_index = 0;
		}

		len1 = vs->audio_buf_size - vs->audio_buf_index;
		if (len1 > len)
			len1 = len;

		memcpy(stream, (uint8_t*)vs->audio_buf + vs->audio_buf_index, len1);
		len -= len1;
		stream += len1;
		vs->audio_buf_index += len1;
	}
}

int QueueVideoPicture(VideoState* aVideoState, AVFrame* aFrame, double pts)
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
		vp->allocated = 0;

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
		vp->pts = pts;
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

void stream_seek(VideoState* vs, int64_t pos, int rel)
{
	vs->seek_pos = pos;
	vs->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
	vs->seek_req = 1;
}
}
