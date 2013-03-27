#include "stdafx.h"
#include "utility.h"

//@return >=0 if OK, <0 for failed.
int GetStreamIndexByMediaType(AVFormatContext* aFmtCtx, AVMediaType aMt)
{
	CheckPointer(aFmtCtx, AV_CODEC_ID_NONE);

	for (unsigned int i = 0; i < aFmtCtx->nb_streams; i++)
	{
		if (aMt == aFmtCtx->streams[i]->codec->codec_type)
		{
			return i;
		}
	}

	return -1;
}