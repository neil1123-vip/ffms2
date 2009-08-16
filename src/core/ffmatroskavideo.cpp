//  Copyright (c) 2007-2009 Fredrik Mellbin
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include "ffvideosource.h"



void FFMatroskaVideo::Free(bool CloseCodec) {
	if (CS)
		cs_Destroy(CS);
	if (MC.ST.fp) {
		mkv_Close(MF);
		fclose(MC.ST.fp);
	}
	if (CloseCodec)
		avcodec_close(CodecContext);
	av_freep(&CodecContext);
}

FFMatroskaVideo::FFMatroskaVideo(const char *SourceFile, int Track,
	FFMS_Index *Index, int Threads)
	: Res(FFSourceResources<FFMS_VideoSource>(this)), FFMS_VideoSource(SourceFile, Index, Track) {

	AVCodec *Codec = NULL;
	CodecContext = NULL;
	TrackInfo *TI = NULL;
	CS = NULL;
	PacketNumber = 0;
	VideoTrack = Track;
	Frames = (*Index)[VideoTrack];

	MC.ST.fp = ffms_fopen(SourceFile, "rb");
	if (MC.ST.fp == NULL)
		throw FFMS_Exception(FFMS_ERROR_PARSER, FFMS_ERROR_FILE_READ,
			boost::format("Can't open '%1%': %2%") % SourceFile % strerror(errno));

	setvbuf(MC.ST.fp, NULL, _IOFBF, CACHESIZE);

	MF = mkv_OpenEx(&MC.ST.base, 0, 0, ErrorMessage, sizeof(ErrorMessage));
	if (MF == NULL) {
		fclose(MC.ST.fp);
		throw FFMS_Exception(FFMS_ERROR_PARSER, FFMS_ERROR_FILE_READ,
			boost::format("Can't parse Matroska file: %1%") % ErrorMessage);
	}

	mkv_SetTrackMask(MF, ~(1 << VideoTrack));
	TI = mkv_GetTrackInfo(MF, VideoTrack);

	if (TI->CompEnabled) {
		CS = cs_Create(MF, VideoTrack, ErrorMessage, sizeof(ErrorMessage));
		if (CS == NULL) {
			Free(false);
			throw FFMS_Exception(FFMS_ERROR_CODEC, FFMS_ERROR_UNSUPPORTED,
				boost::format("Can't create decompressor: %1%") % ErrorMessage);
		}
	}

	CodecContext = avcodec_alloc_context();
	CodecContext->thread_count = Threads;

	Codec = avcodec_find_decoder(MatroskaToFFCodecID(TI->CodecID, TI->CodecPrivate));
	if (Codec == NULL)
		throw FFMS_Exception(FFMS_ERROR_DECODING, FFMS_ERROR_CODEC,
			"Video codec not found");

	InitializeCodecContextFromMatroskaTrackInfo(TI, CodecContext);

	if (avcodec_open(CodecContext, Codec) < 0)
		throw FFMS_Exception(FFMS_ERROR_DECODING, FFMS_ERROR_CODEC,
			"Could not open video codec");

	Res.CloseCodec(true);

	// Always try to decode a frame to make sure all required parameters are known
	DecodeNextFrame();

	VP.Width = CodecContext->width;
	VP.Height = CodecContext->height;
	VP.FPSDenominator = 1;
	VP.FPSNumerator = 30;
	VP.RFFDenominator = CodecContext->time_base.num;
	VP.RFFNumerator = CodecContext->time_base.den;
	VP.NumFrames = Frames.size();
	VP.VPixelFormat = CodecContext->pix_fmt;
	VP.TopFieldFirst = DecodeFrame->top_field_first;
	VP.FirstTime = ((Frames.front().DTS * Frames.TB.Num) / (double)Frames.TB.Den) / 1000;
	VP.LastTime = ((Frames.back().DTS * Frames.TB.Num) / (double)Frames.TB.Den) / 1000;

	if (VP.Width <= 0 || VP.Height <= 0)
		throw FFMS_Exception(FFMS_ERROR_DECODING, FFMS_ERROR_CODEC,
			"Codec returned zero size video");

	// Calculate the average framerate
	if (Frames.size() >= 2) {
		double DTSDiff = (double)(Frames.back().DTS - Frames.front().DTS);
		VP.FPSDenominator = (unsigned int)(DTSDiff * mkv_TruncFloat(TI->TimecodeScale) / (double)1000 / (double)(VP.NumFrames - 1) + 0.5);
		VP.FPSNumerator = 1000000;
	}

	// Output the already decoded frame so it isn't wasted
	OutputFrame(DecodeFrame);

	// Set AR variables
	VP.SARNum = TI->AV.Video.DisplayWidth * TI->AV.Video.PixelHeight;
	VP.SARDen = TI->AV.Video.DisplayHeight * TI->AV.Video.PixelWidth;

	// Set crop variables
	VP.CropLeft = TI->AV.Video.CropL;
	VP.CropRight = TI->AV.Video.CropR;
	VP.CropTop = TI->AV.Video.CropT;
	VP.CropBottom = TI->AV.Video.CropB;
}

void FFMatroskaVideo::DecodeNextFrame() {
	int FrameFinished = 0;
	AVPacket Packet;
	InitNullPacket(Packet);

	unsigned int FrameSize;

	while (PacketNumber < static_cast<int>(Frames.size())) {
		FrameSize = Frames[PacketNumber].FrameSize;
		ReadFrame(Frames[PacketNumber].FilePos, FrameSize, CS, MC);

		Packet.data = MC.Buffer;
		Packet.size = FrameSize;
		if (Frames[PacketNumber].KeyFrame)
			Packet.flags = AV_PKT_FLAG_KEY;
		else
			Packet.flags = 0;

		PacketNumber++;

		if (CodecContext->codec_id == CODEC_ID_MPEG4 && IsNVOP(Packet))
			goto Done;

		avcodec_decode_video2(CodecContext, DecodeFrame, &FrameFinished, &Packet);

		if (FrameFinished)
			goto Done;
	}

	// Flush the last frames
	if (CodecContext->has_b_frames) {
		AVPacket NullPacket;
		InitNullPacket(NullPacket);
		avcodec_decode_video2(CodecContext, DecodeFrame, &FrameFinished, &NullPacket);
	}

	if (!FrameFinished)
		goto Error;

Error:
Done:;
}

FFMS_Frame *FFMatroskaVideo::GetFrame(int n) {
	if (LastFrameNum == n)
		return &LocalFrame;

	int ClosestKF = Frames.FindClosestVideoKeyFrame(n);
	if (CurrentFrame > n || ClosestKF > CurrentFrame + 10) {
		PacketNumber = ClosestKF;
		CurrentFrame = ClosestKF;
		avcodec_flush_buffers(CodecContext);
	}

	do {
		DecodeNextFrame();
		CurrentFrame++;
	} while (CurrentFrame <= n);

	LastFrameNum = n;
	return OutputFrame(DecodeFrame);
}
