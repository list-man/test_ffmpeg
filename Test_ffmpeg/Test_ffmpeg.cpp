// Test_ffmpeg.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <stdio.h>
#include <stdint.h>
#include "utility.h"
#include "saveFrame.h"

int main(int argc, char* argv[])
{
	av_register_all();

	AVFormatContext* pFormatCtx = NULL;
	AVDictionary* pAVDictionary = NULL;
	if (avformat_open_input(&pFormatCtx, argv[1], NULL, &pAVDictionary) == 0)
	{
		if (av_find_stream_info(pFormatCtx) < 0)
			return -2;//can't find stream info.

		SaveFrame(pFormatCtx);
	}

	av_close_input_file(pFormatCtx);

	return 0;
}

