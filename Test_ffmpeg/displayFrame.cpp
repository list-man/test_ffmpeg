#include "stdafx.h"
#include "utility.h"
#include "displayFrame.h"
#include "SDL.h"
#include "SDL_thread.h"

#pragma comment(lib, "SDL.lib")

void DisplayPacket(AVFormatContext* aFmtCtx, unsigned int aStreamIndex);

int DisplayFrame(AVFormatContext* aFmtCtx)
{
	int vstreamIdx = GetStreamIndexByMediaType(aFmtCtx, AVMEDIA_TYPE_VIDEO);
	if (vstreamIdx < 0)
		return AVERROR_STREAM_NOT_FOUND;

	AVCodecContext* codecCtx = aFmtCtx->streams[vstreamIdx]->codec;
	AVCodec* decoder = avcodec_find_decoder(codecCtx->codec_id);
	if (!decoder)
		return AVERROR_DECODER_NOT_FOUND;

	if (avcodec_open2(codecCtx, decoder, NULL) >= 0)
	{
		DisplayPacket(aFmtCtx, vstreamIdx);

		avcodec_close(codecCtx);
	}

	return 0;
}

void DisplayPacket(AVFormatContext* aFmtCtx, unsigned int aStreamIndex)
{
	AVCodecContext* codecCtx = aFmtCtx->streams[aStreamIndex]->codec;

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);
	SDL_Surface* scrSurface = SDL_SetVideoMode(codecCtx->width, codecCtx->height, 0, 0);
	SDL_Overlay* bmpOverlay = SDL_CreateYUVOverlay(codecCtx->width, codecCtx->height, SDL_YV12_OVERLAY, scrSurface);

	AVFrame* pFrame = av_frame_alloc();
	AVPacket packet;
	memset(&packet, 0, sizeof(packet));
	SwsContext* swsc = NULL;

	int counter = 0;
	while (av_read_frame(aFmtCtx, &packet) >= 0)
	{
		if (packet.stream_index == aStreamIndex)
		{
			int got_pic = 0;
			if (avcodec_decode_video2(codecCtx, pFrame, &got_pic, &packet) > 0)
			{
				counter++;
				if (got_pic)
				{
					SDL_LockYUVOverlay(bmpOverlay);
					AVPicture pic;
					pic.data[0] = bmpOverlay->pixels[0];
					pic.data[1] = bmpOverlay->pixels[2];
					pic.data[2] = bmpOverlay->pixels[1];

					pic.linesize[0] = bmpOverlay->pitches[0];
					pic.linesize[1] = bmpOverlay->pitches[2];
					pic.linesize[2] = bmpOverlay->pitches[1];

					swsc = sws_getCachedContext(swsc, codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
						codecCtx->width, codecCtx->height, AV_PIX_FMT_YUV420P,
						SWS_BICUBIC, NULL, NULL, NULL);
					sws_scale(swsc, pFrame->data, pFrame->linesize, 0, codecCtx->height, pic.data, pic.linesize);

					SDL_UnlockYUVOverlay(bmpOverlay);

					SDL_Rect rc;
					rc.x = rc.y = 0;
					rc.w = codecCtx->width;
					rc.h = codecCtx->height;
					SDL_DisplayYUVOverlay(bmpOverlay, &rc);
				}

				/*
				When AVCodecContext.refcounted_frames is set to 1, the frame is
				reference counted and the returned reference belongs to the
				caller. The caller must release the frame using av_frame_unref()
				when the frame is no longer needed.
				----from commit of avcodec_decode_video2. I meat a exception when calling av_frame_unref directly
				without this if condition. Finally I find the solution by reading the commit of avcode_decode_video2.
				*/
				if (codecCtx->refcounted_frames > 0)
					av_frame_unref(pFrame);
			}
		}

		av_free_packet(&packet);
		memset(&packet, 0, sizeof(packet));

		if (counter > 1000)
			break;
	}

	avcodec_free_frame(&pFrame);

	//while (SDL_PollEvent(&evnt)) //SDL_PollEvent will return 0 if there is none pending event available, Unlinke GetMessage in Windows.
	SDL_Event evnt;
	while (true)
	{
		SDL_PollEvent(&evnt);
		switch (evnt.type)
		{
		case SDL_QUIT:
			SDL_Quit();
			return ;
		case SDL_MOUSEBUTTONDOWN:
			{
				printf("mouse down, which=%d, state=%d, x=%d, y=%d, xrel=%d, yrel=%d\n", evnt.motion.which, evnt.motion.state,
					evnt.motion.x, evnt.motion.y, evnt.motion.xrel, evnt.motion.yrel);
			}
		}
	}
}