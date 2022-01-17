//==============================================================================
//
//  Transcode
//
//  Created by Kwon Keuk Han
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "encoder_hevc.h"

#include <unistd.h>

#include "../../transcoder_private.h"
#include "../codec_utilities.h"

EncoderHEVC::~EncoderHEVC()
{
}

bool EncoderHEVC::SetCodecParams()
{
	_codec_context->framerate = ::av_d2q((_encoder_context->GetFrameRate() > 0) ? _encoder_context->GetFrameRate() : _encoder_context->GetEstimateFrameRate(), AV_TIME_BASE);
	_codec_context->bit_rate = _encoder_context->GetBitrate();
	_codec_context->rc_min_rate = _codec_context->rc_max_rate = _codec_context->bit_rate;
	_codec_context->rc_buffer_size = static_cast<int>(_codec_context->bit_rate / 2);
	_codec_context->sample_aspect_ratio = (AVRational){1, 1};

	// From avcodec.h:
	// For some codecs, the time base is closer to the field rate than the frame rate.
	// Most notably, H.264 and MPEG-2 specify time_base as half of frame duration
	// if no telecine is used ...
	// Set to time_base ticks per frame. Default 1, e.g., H.264/MPEG-2 set it to 2.
	_codec_context->ticks_per_frame = 2;
	// From avcodec.h:
	// For fixed-fps content, timebase should be 1/framerate and timestamp increments should be identically 1.
	// This often, but not always is the inverse of the frame rate or field rate for video. 1/time_base is not the average frame rate if the frame rate is not constant.

	_codec_context->time_base = ::av_inv_q(::av_mul_q(_codec_context->framerate, (AVRational){_codec_context->ticks_per_frame, 1}));

	// _codec_context->gop_size = _codec_context->framerate.num / _codec_context->framerate.den;
	_codec_context->max_b_frames = 0;
	_codec_context->pix_fmt = (AVPixelFormat)GetPixelFormat();
	_codec_context->width = _encoder_context->GetVideoWidth();
	_codec_context->height = _encoder_context->GetVideoHeight();

	// Limit the number of threads suitable for h264 encoding to between 4 and 8.
	_codec_context->thread_count = (_encoder_context->GetThreadCount() > 0) ? _encoder_context->GetThreadCount() : FFMIN(FFMAX(4, av_cpu_count() / 3), 8);

	// For browser compatibility
	_codec_context->profile = FF_PROFILE_HEVC_MAIN;

	// Preset
	if (_encoder_context->GetPreset() == "slower")
	{
		::av_opt_set(_codec_context->priv_data, "preset", "slower", 0);
	}
	else if (_encoder_context->GetPreset() == "slow")
	{
		::av_opt_set(_codec_context->priv_data, "preset", "slow", 0);
	}
	else if (_encoder_context->GetPreset() == "medium")
	{
		::av_opt_set(_codec_context->priv_data, "preset", "medium", 0);
	}
	else if (_encoder_context->GetPreset() == "fast")
	{
		::av_opt_set(_codec_context->priv_data, "preset", "fast", 0);
	}
	else if (_encoder_context->GetPreset() == "faster")
	{
		::av_opt_set(_codec_context->priv_data, "preset", "faster", 0);
	}
	else
	{
		// Default
		::av_opt_set(_codec_context->priv_data, "preset", "faster", 0);
	}

	// Encoding Delay
	::av_opt_set(_codec_context->priv_data, "tune", "zerolatency", 0);

	// Keyframe Intervasl
	::av_opt_set(_codec_context->priv_data, "x265-params", ov::String::FormatString("pass=1:bframes=0:no-scenecut=1:keyint=%.0f:min-keyint=%.0f:level-idc=4:no-open-gop=1", _encoder_context->GetFrameRate(), _encoder_context->GetFrameRate()).CStr(), 0);

	return true;
}

// Notes.
//
// - B-frame must be disabled. because, WEBRTC does not support B-Frame.
//
bool EncoderHEVC::Configure(std::shared_ptr<TranscodeContext> context)
{
	if (TranscodeEncoder::Configure(context) == false)
	{
		return false;
	}

	auto codec_id = GetCodecID();
	AVCodec *codec = ::avcodec_find_encoder(codec_id);

	if (codec == nullptr)
	{
		logte("Could not find encoder: %d (%s)", codec_id, ::avcodec_get_name(codec_id));
		return false;
	}

	_codec_context = ::avcodec_alloc_context3(codec);
	if (_codec_context == nullptr)
	{
		logte("Could not allocate codec context for %s (%d)", ::avcodec_get_name(codec_id), codec_id);
		return false;
	}

	if (SetCodecParams() == false)
	{
		logte("Could not set codec parameters for %s (%d)", ::avcodec_get_name(codec_id), codec_id);
		return false;
	}

	if (::avcodec_open2(_codec_context, codec, nullptr) < 0)
	{
		logte("Could not open codec. %s (%d)", ::avcodec_get_name(codec_id), codec_id);
		return false;
	}

	// Generates a thread that reads and encodes frames in the input_buffer queue and places them in the output queue.
	try
	{
		_kill_flag = false;

		_codec_thread = std::thread(&EncoderHEVC::CodecThread, this);
		pthread_setname_np(_codec_thread.native_handle(), ov::String::FormatString("Enc%s", avcodec_get_name(GetCodecID())).CStr());
	}
	catch (const std::system_error &e)
	{
		logte("Failed to start encoder thread.");
		_kill_flag = true;

		return false;
	}

	return true;
}

void EncoderHEVC::CodecThread()
{
	while (!_kill_flag)
	{
		auto obj = _input_buffer.Dequeue();
		if (obj.has_value() == false)
			continue;


		auto media_frame = std::move(obj.value());

		///////////////////////////////////////////////////
		// Request frame encoding to codec
		///////////////////////////////////////////////////
		if (TranscoderUtilities::CopyMediaFrameToAvFrame(cmn::MediaType::Video, media_frame, _frame) == false)
		{
			logte("Could not allocate the video frame data");
			break;
		}

		int ret = ::avcodec_send_frame(_codec_context, _frame);
		::av_frame_unref(_frame);
		if (ret < 0)
		{
			logte("Error sending a frame for encoding : %d", ret);
		}

		///////////////////////////////////////////////////
		// The encoded packet is taken from the codec.
		///////////////////////////////////////////////////
		while (true)
		{
			// Check frame is availble
			int ret = ::avcodec_receive_packet(_codec_context, _packet);
			if (ret == AVERROR(EAGAIN))
			{
				// More packets are needed for encoding.
				break;
			}
			else if (ret == AVERROR_EOF && ret < 0)
			{
				logte("Error receiving a packet for decoding : %d", ret);
				break;
			}
			else
			{
				auto media_packet = TranscoderUtilities::GetMediaPacketFromAvPacket(_packet, cmn::MediaType::Video, cmn::BitstreamFormat::H265_ANNEXB, cmn::PacketType::NALU);
				if (media_packet == nullptr)
				{
					logte("Could not allocate the media packet");
					break;
				}

				::av_packet_unref(_packet);

				SendOutputBuffer(std::move(media_packet));
			}
		}
	}
}
