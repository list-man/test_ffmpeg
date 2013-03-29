#include "stdafx.h"
#include "saveFrame.h"
#include "utility.h"

#define _USE_FRAME_GET_BUFFER_

void SaveFrame(AVFrame* aAvFrame, unsigned int aWidth, unsigned int aHeight, wchar_t* aFileName);
void DecodePacket(AVFormatContext* aFmtCtx, unsigned int aStreamIndex);

int SaveFrame(AVFormatContext* aFmtCtx)
{
	int stream = GetStreamIndexByMediaType(aFmtCtx, AVMEDIA_TYPE_VIDEO);
	if (stream >= 0)
	{
		AVCodec* pDecoder = avcodec_find_decoder(aFmtCtx->streams[stream]->codec->codec_id);
		if (!pDecoder)
		{
			return -3;//no decoder found.
		}

		if (0 != avcodec_open2(aFmtCtx->streams[stream]->codec, pDecoder, NULL))
		{
			return -4;//open decoder failed.
		}

		DecodePacket(aFmtCtx, stream);

		avcodec_close(aFmtCtx->streams[stream]->codec);
	}	
}

void SaveFrame(AVFrame* aAvFrame, unsigned int aWidth, unsigned int aHeight, wchar_t* aFileName)
{
	FILE* fp = _wfopen(aFileName, L"wb");

	// Write header.
	fprintf(fp, "P6\n%d %d\n255\n", aWidth, aHeight);

	// Write pixel data.
	for (int i = 0; i < aHeight; i++)
	{
		fwrite(aAvFrame->data[0] + i * aAvFrame->linesize[0], 1, aWidth*3, fp);
	}

	fclose(fp);
}

void DecodePacket(AVFormatContext* aFmtCtx, unsigned int aStreamIndex)
{
	AVPacket packet;
	memset(&packet, 0, sizeof(packet));

	AVCodecContext* codecCtx = aFmtCtx->streams[aStreamIndex]->codec;
	static SwsContext* swsc = NULL;

	int counter = 0;

	/*
	AVFrame is typically allocated once and then reused multiple times to hold
	* different data (e.g. a single AVFrame to hold frames received from a
	* decoder). In such a case, av_frame_unref() will free any references held by
	* the frame and reset it to its original clean state before it
	* is reused again.
	*/
	AVFrame* pFrame = av_frame_alloc();//only allocates the AVFrame itself, not the data buffers. Set its fields to default values.
	AVFrame* pFrameRGB = av_frame_alloc();

#ifdef _USE_FRAME_GET_BUFFER_	//we can use av_frame_free to free the Frame resource.
	pFrameRGB->format = PIX_FMT_RGB24;
	pFrameRGB->width = codecCtx->width;
	pFrameRGB->height = codecCtx->height;
	av_frame_get_buffer(pFrameRGB, 1);

#else	//we can't use av_frame_free if use this way.otherwise a crash occurs when avcodec_close invoked.
 	int numBytes = avpicture_get_size(PIX_FMT_RGB24, codecCtx->width, codecCtx->height);
 	uint8_t* buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
 	avpicture_fill((AVPicture*)pFrameRGB, buffer, PIX_FMT_RGB24, codecCtx->width, codecCtx->height);

#endif

	while (0 == av_read_frame(aFmtCtx, &packet))
	{
		if (packet.stream_index == aStreamIndex)
		{
			int got_picture_ptr = 0;
			if (avcodec_decode_video2(codecCtx, pFrame, &got_picture_ptr, &packet) > 0)
			{
				if (got_picture_ptr)
				{
					swsc = sws_getCachedContext(swsc, codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
							codecCtx->width, codecCtx->height, PIX_FMT_RGB24,
							SWS_BICUBIC, NULL, NULL, NULL);

					//avpicture_alloc((AVPicture*)&frameRGB, PIX_FMT_RGB24, codecCtx->width, codecCtx->height);
					sws_scale(swsc, pFrame->data, pFrame->linesize, 0, codecCtx->height, pFrameRGB->data, pFrameRGB->linesize);

					if (counter>255 && counter<270)
					{
						wchar_t szPath[_MAX_PATH] = {0};
						swprintf(szPath, L"frame%d.ppm", counter);
						SaveFrame(pFrameRGB, codecCtx->width, codecCtx->height, szPath);
					}

					counter++;

					//sws_scale don't add reference counted of AVFrame, so we can't do av_frame_unref.
					//To see from the commit of avcodec_decode_video2:
					/*
						When AVCodecContext.refcounted_frames is set to 1, the frame is
						reference counted and the returned reference belongs to the
						caller. The caller must release the frame using av_frame_unref()
						when the frame is no longer needed.
					*/
					//I guess AVFrame may add reference count when used with AVCodecContext together.
					//av_frame_unref(pFrameRGB);
				}
				
				if (codecCtx->refcounted_frames > 0)
					av_frame_unref(pFrame);
			}
		}

		av_free_packet(&packet);
		memset(&packet, 0, sizeof(packet));
	}

	//用av_malloc分配然后用avpicture_fill填充进AVFrame的，不能使用av_frame_free来释放，通过这样的方式分配之后结果
	//AVFrame中的bug并没有reference count.av_frame_free释放dynamically allocated objects的方式也许并不是av_free,而
	//AVFrame中的dynamically allocated objects 却是通过av_malloc来获得的。也许该看看源码?
 #ifdef _USE_FRAME_GET_BUFFER_
 	av_frame_free(&pFrameRGB);	//we use av_frame_free instead of avcodec_free_frame because we know how we allocated the memory.
 #else
	av_free(buffer);
	avcodec_free_frame(&pFrameRGB);
#endif

	/*
	this function does NOT free the data buffers themselves
	* (it does not know how, since they might have been allocated with
	*  a custom get_buffer()).
	Yes, that's what we want.the data buffers were free by av_frame_unref already.
	*/
	avcodec_free_frame(&pFrame);
}

