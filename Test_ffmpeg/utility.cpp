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

int packet_queue_put(PacketQueue* pktq, AVPacket* packet)
{
	if (!pktq || !packet)
		return -1;

	if (av_dup_packet(packet) < 0)
		return -1;

	AVPacketList* pktl = (AVPacketList*)av_malloc(sizeof(AVPacketList));
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
	// e.g. packet_queue_get execute at SDL_CondWait(pktq->cond, pktq->mutex), if we signal SDL_CondSignal,
	// SDL_CondWait will re-locked the Mutex, but we have already locked it here. So what SDL_CondWait will?
	SDL_CondSignal(pktq->cond);
	SDL_UnlockMutex(pktq->mutex);

	return 0;
}

int packet_queue_get(PacketQueue* pktq, AVPacket* packet, bool blockOrnot)
{
	int result = 0;
	if (!pktq || !packet)
	{
		return -1;
	}

	SDL_LockMutex(pktq->mutex);

	if (pktq->first)
	{
		*packet = pktq->first->pkt;

		pktq->nb_packets--;
		pktq->size -= pktq->first->pkt.size;

		pktq->first = pktq->first->next;
	}
	else if (blockOrnot)
	{
		SDL_CondWait(pktq->cond, pktq->mutex);
		result = -1;
	}
	else
	{
		result = -1;
	}

	SDL_UnlockMutex(pktq->mutex);

	return result;
}

int rint(int x)
{
	return (int)(x + (x < 0 ? -0.5 : 0.5));
};
