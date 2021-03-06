#include "stdafx.h"
#include "utility.h"
#include "playAudio.h"
#include "SDL.h"
#include "SDL_thread.h"

PacketQueue	audioq;

int quit = 0;

int audio_decode_frame(AVCodecContext* aCodecCtx, uint8_t* audio_buf, int buf_size)
{
	static AVPacket pkt;
	static AVFrame* pFrame = NULL;
	int len1 = 0;
	int got_frame = 0;
	int flush_complete = 0;

	for(;;) {
		while(pkt.size > 0) {
			if (!pFrame)
				pFrame = av_frame_alloc();
			else
				avcodec_get_frame_defaults(pFrame);

			if (flush_complete)
				break;

			len1 = avcodec_decode_audio4(aCodecCtx, pFrame, &got_frame, &pkt);

			if(len1 < 0) {
				/* if error, skip frame */
				pkt.size = 0;
				break;
			}

			//memcpy(audio_buf, pkt.data, len1);
			memcpy(audio_buf, pFrame->data, len1);

			pkt.data += len1;
			pkt.size -= len1;

			if (!got_frame)
			{
				if (!pkt.data)
					flush_complete = 1;

				continue;
			}
			
			/* We have data, return it and come back for more later */
			return len1;
		}
		if(pkt.data)
			av_free_packet(&pkt);

		if(quit) {
			return -1;
		}

		if (packet_queue_get(&audioq, &pkt, 1) < 0) {
			return -1;
		}
	}

	//avcodec_free_frame(&pFrame);
	av_frame_free(&pFrame);
}

void SDLCALL audio_callback(void *userdata, Uint8 *stream, int len)
{
	const AVCodecContext* codecCtx = (const AVCodecContext*)userdata;
	int len1, audio_size;

	static uint8_t audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	while (len > 0)
	{
		if (audio_buf_index >= audio_buf_size)
		{
			audio_size = audio_decode_frame((AVCodecContext*)codecCtx, audio_buf, sizeof(audio_buf));
			if (audio_size < 0)
			{
				audio_buf_size = 1024;
				memset(audio_buf, 0, audio_buf_size);
			}
			else
			{
				audio_buf_size = audio_size;
			}

			audio_buf_index = 0;
		}

		len1 = audio_buf_size - audio_buf_index;
		if (len1 > len)
			len1 = len;

		memcpy(stream, (uint8_t*)audio_buf + audio_buf_index, len1);
		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
}

int DemuxPacket(AVFormatContext* aFmtCtx, unsigned int aStreamIdx)
{
	CheckPointer(aFmtCtx, -1);

	AVPacket packet;
	memset(&packet, 0, sizeof(packet));

	while (av_read_packet(aFmtCtx, &packet) >= 0)
	{
		if (packet.stream_index == aStreamIdx)
			packet_queue_put(&audioq, &packet);
		else
			av_free_packet(&packet);

		memset(&packet, 0, sizeof(packet));
	}

	return 0;
}

void RenderAudio(AVFormatContext* aFmtCtx, unsigned int aStreamIndex)
{
	AVCodecContext* codecCtx = aFmtCtx->streams[aStreamIndex]->codec;
	SDL_AudioSpec audioSepc;
	audioSepc.freq = codecCtx->sample_rate;
	audioSepc.format = AUDIO_S16SYS;
	audioSepc.samples = SDL_AUDIO_BUFFER_SIZE;
	audioSepc.channels = codecCtx->channels;
	audioSepc.callback = audio_callback;
	audioSepc.userdata = codecCtx;
	audioSepc.silence = 0;

	if (SDL_OpenAudio(&audioSepc, NULL) >= 0)
	{
		Init_PacketQueue(&audioq);
		SDL_PauseAudio(0);

		DemuxPacket(aFmtCtx, aStreamIndex);
	}
	else
	{
		printf("Open audio device failed!\n");
	}
}

int PlayAudioStream(AVFormatContext* aFmtCtx)
{
	int aStreamIdx = GetStreamIndexByMediaType(aFmtCtx, AVMEDIA_TYPE_AUDIO);
	if (aStreamIdx < 0)
		return AVERROR_STREAM_NOT_FOUND;

	AVCodecContext* codecCtx = aFmtCtx->streams[aStreamIdx]->codec;
	AVCodec* audioDecoder = avcodec_find_decoder(codecCtx->codec_id);
	if (!audioDecoder)
		return AVERROR_DECODER_NOT_FOUND;

	if (avcodec_open2(codecCtx, audioDecoder, NULL) >= 0)
	{
		RenderAudio(aFmtCtx, aStreamIdx);

		avcodec_close(codecCtx);
	}

	return 0;
}