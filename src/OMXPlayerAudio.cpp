/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */


#include "OMXPlayerAudio.h"

#include <stdio.h>
#include <unistd.h>



#include "XMemUtils.h"


#define MAX_DATA_SIZE    3 * 1024 * 1024

OMXPlayerAudio::OMXPlayerAudio()
{
	m_open          = false;
	omxClock      = NULL;
	omxReader    = NULL;
	m_decoder       = NULL;
	m_flush         = false;
	m_cached_size   = 0;
	m_pChannelMap   = NULL;
	m_pAudioCodec   = NULL;
	m_speed         = DVD_PLAYSPEED_NORMAL;
	m_player_error  = true;

	pthread_cond_init(&m_packet_cond, NULL);
	pthread_cond_init(&m_audio_cond, NULL);
	pthread_mutex_init(&m_lock, NULL);
	pthread_mutex_init(&m_lock_decoder, NULL);
}

OMXPlayerAudio::~OMXPlayerAudio()
{
	if(m_open)
	{
		Close();
	}


	pthread_cond_destroy(&m_audio_cond);
	pthread_cond_destroy(&m_packet_cond);
	pthread_mutex_destroy(&m_lock);
	pthread_mutex_destroy(&m_lock_decoder);
}

void OMXPlayerAudio::Lock()
{
	pthread_mutex_lock(&m_lock);
}

void OMXPlayerAudio::UnLock()
{
	pthread_mutex_unlock(&m_lock);
}

void OMXPlayerAudio::LockDecoder()
{
	pthread_mutex_lock(&m_lock_decoder);
}

void OMXPlayerAudio::UnLockDecoder()
{
	pthread_mutex_unlock(&m_lock_decoder);
}

bool OMXPlayerAudio::Open(OMXStreamInfo& hints, 
                          OMXClock *av_clock, 
                          OMXReader *omx_reader,
                          std::string device)
{
	if(ThreadHandle())
	{
		Close();
	}

	if (!av_clock)
	{
		return false;
	}


	m_hints       = hints;
	omxClock    = av_clock;
	omxReader  = omx_reader;
	deviceName      = device;
	m_iCurrentPts = DVD_NOPTS_VALUE;
	m_bAbort      = false;
	m_flush       = false;
	m_cached_size = 0;
	m_pAudioCodec = NULL;
	m_pChannelMap = NULL;
	m_speed       = DVD_PLAYSPEED_NORMAL;

// omxClock->SetMasterClock(false);

	m_player_error = OpenAudioCodec();
	if(!m_player_error)
	{
		Close();
		return false;
	}

	m_player_error = OpenDecoder();
	if(!m_player_error)
	{
		Close();
		return false;
	}

	Create();

	m_open        = true;

	return true;
}

bool OMXPlayerAudio::Close()
{
	m_bAbort  = true;
	m_flush   = true;

	Flush();

	if(ThreadHandle())
	{
		Lock();
		pthread_cond_broadcast(&m_packet_cond);
		UnLock();

		StopThread("OMXPlayerAudio");
	}

	CloseDecoder();
	CloseAudioCodec();

	m_open          = false;
	m_stream_id     = -1;
	m_iCurrentPts   = DVD_NOPTS_VALUE;
	m_speed         = DVD_PLAYSPEED_NORMAL;


	return true;
}



bool OMXPlayerAudio::Decode(OMXPacket *pkt)
{
	if(!pkt)
	{
		return false;
	}

	/* last decoder reinit went wrong */
	if(!m_decoder || !m_pAudioCodec)
	{
		return true;
	}

	if(!omxReader->IsActive(OMXSTREAM_AUDIO, pkt->stream_index))
	{
		return true;
	}

	int channels = pkt->hints.channels;

	/* 6 channel have to be mapped to 8 for PCM */
	if(!m_passthrough && !m_hw_decode)
	{
		if(channels == 6)
		{
			channels = 8;
		}
	}

	unsigned int old_bitrate = m_hints.bitrate;
	unsigned int new_bitrate = pkt->hints.bitrate;

	/* only check bitrate changes on CODEC_ID_DTS, CODEC_ID_AC3, CODEC_ID_EAC3 */
	if(m_hints.codec != CODEC_ID_DTS && m_hints.codec != CODEC_ID_AC3 && m_hints.codec != CODEC_ID_EAC3)
	{
		new_bitrate = old_bitrate = 0;
	}

	/* audio codec changed. reinit device and decoder */
	if(m_hints.codec         != pkt->hints.codec ||
	        m_hints.channels      != channels ||
	        m_hints.samplerate    != pkt->hints.samplerate ||
	        old_bitrate           != new_bitrate ||
	        m_hints.bitspersample != pkt->hints.bitspersample)
	{
       
		CloseDecoder();
		CloseAudioCodec();

		m_hints = pkt->hints;

		m_player_error = OpenAudioCodec();
		if(!m_player_error)
		{
			return false;
		}

		m_player_error = OpenDecoder();
		if(!m_player_error)
		{
			return false;
		}
	}
    
#if 1
	if(!((int)m_decoder->GetSpace() > pkt->size))
	{
		omxClock->sleep(10);
	}

	if((int)m_decoder->GetSpace() > pkt->size)
	{
		if(pkt->pts != DVD_NOPTS_VALUE)
		{
			m_iCurrentPts = pkt->pts;
		}
		else if(pkt->dts != DVD_NOPTS_VALUE)
		{
			m_iCurrentPts = pkt->dts;
		}

		uint8_t *data_dec = pkt->data;
		int            data_len = pkt->size;

		if(!m_passthrough && !m_hw_decode)
		{
			while(data_len > 0)
			{
				int len = m_pAudioCodec->Decode((BYTE *)data_dec, data_len);
				if( (len < 0) || (len >  data_len) )
				{
					m_pAudioCodec->Reset();
					break;
				}

				data_dec+= len;
				data_len -= len;

				uint8_t *decoded;
				int decoded_size = m_pAudioCodec->GetData(&decoded);

				if(decoded_size <=0)
				{
					continue;
				}

				int ret = 0;

				ret = m_decoder->AddPackets(decoded, decoded_size, pkt->dts, pkt->pts);

				if(ret != decoded_size)
				{
					ofLogError(__func__) << "ret : " << ret << " NOT EQUAL TO " << decoded_size;
				}
			}
		}
		else
		{
			m_decoder->AddPackets(pkt->data, pkt->size, pkt->dts, pkt->pts);
		}
		return true;
	}
	else
	{
		return false;
	}
#endif
}

void OMXPlayerAudio::Process()
{
	OMXPacket *omx_pkt = NULL;

	while(!m_bStop && !m_bAbort)
	{
		Lock();
		if(m_packets.empty())
		{
			pthread_cond_wait(&m_packet_cond, &m_lock);
		}
		UnLock();

		if(m_bAbort)
		{
			break;
		}

		Lock();
		if(m_flush && omx_pkt)
		{
			OMXReader::FreePacket(omx_pkt);
			omx_pkt = NULL;
			m_flush = false;
		}
		else if(!omx_pkt && !m_packets.empty())
		{
			omx_pkt = m_packets.front();
			m_cached_size -= omx_pkt->size;
			m_packets.pop_front();
		}
		UnLock();

		LockDecoder();
		if(m_flush && omx_pkt)
		{
			OMXReader::FreePacket(omx_pkt);
			omx_pkt = NULL;
			m_flush = false;
		}
		else if(omx_pkt && Decode(omx_pkt))
		{
			OMXReader::FreePacket(omx_pkt);
			omx_pkt = NULL;
		}
		UnLockDecoder();
	}

	if(omx_pkt)
	{
		OMXReader::FreePacket(omx_pkt);
	}
}

void OMXPlayerAudio::Flush()
{
	//ofLogVerbose(__func__) << "OMXPlayerAudio::Flush start";
	Lock();
	LockDecoder();
	m_flush = true;
	while (!m_packets.empty())
	{
		OMXPacket *pkt = m_packets.front();
		m_packets.pop_front();
		OMXReader::FreePacket(pkt);
	}
	m_iCurrentPts = DVD_NOPTS_VALUE;
	m_cached_size = 0;
	if(m_decoder)
	{
		m_decoder->Flush();
	}
	UnLockDecoder();
	UnLock();
	//ofLogVerbose(__func__) << "OMXPlayerAudio::Flush end";
}

bool OMXPlayerAudio::AddPacket(OMXPacket *pkt)
{
	bool ret = false;

	if(!pkt)
	{
		return ret;
	}

	if(m_bStop || m_bAbort)
	{
		return ret;
	}

	if((m_cached_size + pkt->size) < MAX_DATA_SIZE)
	{
		Lock();
		m_cached_size += pkt->size;
		m_packets.push_back(pkt);
		UnLock();
		ret = true;
		pthread_cond_broadcast(&m_packet_cond);
	}

	return ret;
}

bool OMXPlayerAudio::OpenAudioCodec()
{
	m_pAudioCodec = new OMXAudioCodecOMX();

	if(!m_pAudioCodec->Open(m_hints))
	{
		delete m_pAudioCodec;
		m_pAudioCodec = NULL;
		return false;
	}

	m_pChannelMap = m_pAudioCodec->GetChannelMap();
	return true;
}

void OMXPlayerAudio::CloseAudioCodec()
{
	if(m_pAudioCodec)
	{
		delete m_pAudioCodec;
	}
	m_pAudioCodec = NULL;
}

OMXAudio::EEncoded OMXPlayerAudio::IsPassthrough(OMXStreamInfo hints)
{
    if(deviceName == "omx:local")
    {
        return OMXAudio::ENCODED_NONE;
    }
    
    OMXAudio::EEncoded passthrough = OMXAudio::ENCODED_NONE;
    
    if(hints.codec == CODEC_ID_AC3)
    {
        passthrough = OMXAudio::ENCODED_IEC61937_AC3;
    }
    if(hints.codec == CODEC_ID_EAC3)
    {
        passthrough = OMXAudio::ENCODED_IEC61937_EAC3;
    }
    if(hints.codec == CODEC_ID_DTS)
    {
        passthrough = OMXAudio::ENCODED_IEC61937_DTS;
    }
    
    return passthrough;

}

bool OMXPlayerAudio::OpenDecoder()
{
	//ofLogVerbose(__func__) << "doHardwareDecode: " << doHardwareDecode;
	//ofLogVerbose(__func__) << "doPassthrough: " << doPassthrough;
	bool bAudioRenderOpen = false;

	m_decoder = new OMXAudio();
	m_decoder->SetClock(omxClock);

	if(doPassthrough)
	{
		m_passthrough = IsPassthrough(m_hints);
	}

	if(!m_passthrough && doHardwareDecode)
	{
		m_hw_decode = OMXAudio::HWDecode(m_hints.codec);
	}

	if(m_passthrough || doHardwareDecode)
	{
		if(m_passthrough)
		{
			m_hw_decode = false;
		}
        stringstream ss;
        ss << deviceName.substr(4);
        string name = ss.str();
		bAudioRenderOpen = m_decoder->init(name, 
                                           m_pChannelMap,
                                           m_hints, 
                                           omxClock,
                                           m_passthrough,
                                           m_hw_decode, 
                                           doBoostOnDownmix);
	}
	
	m_codec_name = omxReader->GetCodecName(OMXSTREAM_AUDIO);

	if(!bAudioRenderOpen)
	{
		delete m_decoder;
		m_decoder = NULL;
		return false;
	}
	else
	{
		if(m_passthrough)
		{
			ofLog(OF_LOG_VERBOSE, "USING PASSTHROUGH, Audio codec %s channels %d samplerate %d bitspersample %d\n",
			      m_codec_name.c_str(), 2, m_hints.samplerate, m_hints.bitspersample);
		}
		else
		{
			ofLog(OF_LOG_VERBOSE, "PASSTHROUGH DISABLED, Audio codec %s channels %d samplerate %d bitspersample %d\n",
			      m_codec_name.c_str(), m_hints.channels, m_hints.samplerate, m_hints.bitspersample);
		}
	}
	//ofLogVerbose(__func__) << "m_hw_decode: " << m_hw_decode;
	return true;
}

bool OMXPlayerAudio::CloseDecoder()
{
	if(m_decoder)
	{
		delete m_decoder;
	}
	m_decoder   = NULL;
	return true;
}

void OMXPlayerAudio::submitEOS()
{
	if(m_decoder)
	{
		m_decoder->submitEOS();
	}
}

bool OMXPlayerAudio::EOS()
{
	return m_packets.empty() && (!m_decoder || m_decoder->EOS());
}

void OMXPlayerAudio::WaitCompletion()
{
	//ofLogVerbose(__func__) << "OMXPlayerAudio::WaitCompletion";
	if(!m_decoder)
	{
		return;
	}

	unsigned int nTimeOut = 2.0f * 1000;
	while(nTimeOut)
	{
		if(EOS())
		{
			ofLog(OF_LOG_VERBOSE, "%s::%s - got eos\n", "OMXPlayerAudio", __func__);
			break;
		}

		if(nTimeOut == 0)
		{
			ofLog(OF_LOG_ERROR, "%s::%s - wait for eos timed out\n", "OMXPlayerAudio", __func__);
			break;
		}
		omxClock->sleep(50);
		nTimeOut -= 50;
	}
}

void OMXPlayerAudio::setCurrentVolume(long nVolume)
{
	//ofLogVerbose(__func__) << "nVolume: " << nVolume;
	if(m_decoder)
	{
		m_decoder->setCurrentVolume(nVolume);
	}
}

long OMXPlayerAudio::getCurrentVolume()
{
	if(m_decoder)
	{
		return m_decoder->getCurrentVolume();
	}
	else
	{
		return 0;
	}
}

void OMXPlayerAudio::setSpeed(int speed)
{
	m_speed = speed;
}

