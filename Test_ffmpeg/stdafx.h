// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#include "targetver.h"

#include <stdio.h>

extern "C"
{
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
}

#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")  
#pragma comment(lib, "avcodec.lib") 
#pragma comment(lib, "avdevice.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "avfilter.lib")
#pragma comment(lib, "postproc.lib")

#ifndef CheckPointer
#define CheckPointer(p, r) \
	if (!p)	\
	return r;
#endif

// TODO: reference additional headers your program requires here
