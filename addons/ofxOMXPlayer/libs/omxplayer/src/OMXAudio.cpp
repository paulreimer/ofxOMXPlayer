/*
* XBMC Media Center
* Copyright (c) 2002 d7o3g4q and RUNTiME
* Portions Copyright (c) by the authors of ffmpeg and xvid
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/



#include "OMXAudio.h"

#define CLASSNAME "COMXAudio"

#include "linux/XMemUtils.h"


#ifndef VOLUME_MINIMUM
#define VOLUME_MINIMUM -6000  // -60dB
#endif

#include <algorithm>


#define OMX_MAX_CHANNELS 9

static enum PCMChannels OMXChannelMap[OMX_MAX_CHANNELS] =
{
  PCM_FRONT_LEFT  , PCM_FRONT_RIGHT,
  PCM_FRONT_CENTER, PCM_LOW_FREQUENCY,
  PCM_BACK_LEFT   , PCM_BACK_RIGHT,
  PCM_SIDE_LEFT   , PCM_SIDE_RIGHT,
  PCM_BACK_CENTER
};

static enum OMX_AUDIO_CHANNELTYPE OMXChannels[OMX_MAX_CHANNELS] =
{
  OMX_AUDIO_ChannelLF, OMX_AUDIO_ChannelRF,
  OMX_AUDIO_ChannelCF, OMX_AUDIO_ChannelLFE,
  OMX_AUDIO_ChannelLR, OMX_AUDIO_ChannelRR,
  OMX_AUDIO_ChannelLS, OMX_AUDIO_ChannelRS,
  OMX_AUDIO_ChannelCS
};

static unsigned int WAVEChannels[OMX_MAX_CHANNELS] =
{
  SPEAKER_FRONT_LEFT,       SPEAKER_FRONT_RIGHT,
  SPEAKER_TOP_FRONT_CENTER, SPEAKER_LOW_FREQUENCY,
  SPEAKER_BACK_LEFT,        SPEAKER_BACK_RIGHT,
  SPEAKER_SIDE_LEFT,        SPEAKER_SIDE_RIGHT,
  SPEAKER_BACK_CENTER
};

static const uint16_t AC3Bitrates[] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
static const uint16_t AC3FSCod   [] = {48000, 44100, 32000, 0};

static const uint16_t DTSFSCod   [] = {0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 0, 0};

// Dolby 5.1 downmixing coefficients
const float downmixing_coefficients_6[16] = {
  //        L       R
  /* L */   1,      0,
  /* R */   0,      1,
  /* C */   0.7071, 0.7071,
  /* LFE */ 0.7071, 0.7071,
  /* Ls */  0.7071, 0,
  /* Rs */  0,      0.7071,
  /* Lr */  0,      0,
  /* Rr */  0,      0
};

// 7.1 downmixing coefficients
const float downmixing_coefficients_8[16] = {
  //        L       R
  /* L */   1,      0,
  /* R */   0,      1,
  /* C */   0.7071, 0.7071,
  /* LFE */ 0.7071, 0.7071,
  /* Ls */  0.7071, 0,
  /* Rs */  0,      0.7071,
  /* Lr */  0.7071, 0,
  /* Rr */  0,      0.7071
};


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
//***********************************************************************************************
COMXAudio::COMXAudio() :
  m_Initialized     (false  ),
  m_Pause           (false  ),
  m_CanPause        (false  ),
  m_CurrentVolume   (0      ),
  m_Passthrough     (false  ),
  m_HWDecode        (false  ),
  m_normalize_downmix(true   ),
  m_BytesPerSec     (0      ),
  m_BufferLen       (0      ),
  m_ChunkLen        (0      ),
  m_InputChannels   (0      ),
  m_OutputChannels  (0      ),
  m_downmix_channels(0      ),
  m_BitsPerSample   (0      ),
  m_omx_clock       (NULL   ),
  m_av_clock        (NULL   ),
  m_external_clock  (false  ),
  m_setStartTime    (false  ),
  m_SampleSize      (0      ),
  m_first_frame     (true   ),
  m_LostSync        (true   ),
  m_SampleRate      (0      ),
  m_eEncoding       (OMX_AUDIO_CodingPCM),
  m_extradata       (NULL   ),
  m_extrasize       (0      )
{
}

COMXAudio::~COMXAudio()
{
  if(m_Initialized)
    Deinitialize();
}


bool COMXAudio::Initialize(const CStdString& device, enum PCMChannels *channelMap,
                           COMXStreamInfo &hints, OMXClock *clock, EEncoded bPassthrough, bool bUseHWDecode,
                           bool boostOnDownmix)
{
  m_HWDecode = false;
  m_Passthrough = false;

  if(bPassthrough != IAudioRenderer::ENCODED_NONE)
  {
    m_Passthrough = true;
    SetCodingType(hints.codec);
  }
  else if(bUseHWDecode)
  {
    m_HWDecode = CanHWDecode(hints.codec);
  }
  else
  {
    SetCodingType(CODEC_ID_PCM_S16LE);
  }

  if(hints.extrasize > 0 && hints.extradata != NULL)
  {
    m_extrasize = hints.extrasize;
    m_extradata = (uint8_t *)malloc(m_extrasize);
    memcpy(m_extradata, hints.extradata, hints.extrasize);
  }
  ofLogVerbose(__PRETTY_FUNCTION__) << " m_Passthrough: " << m_Passthrough;
  return Initialize(device, hints.channels, channelMap, hints.channels, hints.samplerate, hints.bitspersample, false, boostOnDownmix, false, bPassthrough);
}

bool COMXAudio::Initialize(const CStdString& device, int iChannels, enum PCMChannels *channelMap, unsigned int downmixChannels, unsigned int uiSamplesPerSec, unsigned int uiBitsPerSample, bool bResample, bool boostOnDownmix, bool bIsMusic, EEncoded bPassthrough)
{
  std::string deviceuse;
  if(device == "hdmi") {
    deviceuse = "hdmi";
  } else {
    deviceuse = "local";
  }
	
  if(!m_dllAvUtil.Load())
    return false;

  m_Passthrough = false;

  if(bPassthrough != IAudioRenderer::ENCODED_NONE)
    m_Passthrough =true;

  memset(&m_wave_header, 0x0, sizeof(m_wave_header));

 m_CurrentVolume = 0;
ofLogVerbose(__PRETTY_FUNCTION__) << " m_Passthrough: " << m_Passthrough;
  m_downmix_channels = downmixChannels;
  m_normalize_downmix = !boostOnDownmix;

  m_InputChannels = iChannels;
  m_remap.Reset();

  OMX_INIT_STRUCTURE(m_pcm_output);
  m_OutputChannels = 2;
  m_pcm_output.nChannels = m_OutputChannels;
  m_pcm_output.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
  m_pcm_output.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
  m_pcm_output.eChannelMapping[2] = OMX_AUDIO_ChannelMax;

  OMX_INIT_STRUCTURE(m_pcm_input);
  m_pcm_input.nChannels = m_OutputChannels;
  m_pcm_input.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
  m_pcm_input.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
  m_pcm_input.eChannelMapping[2] = OMX_AUDIO_ChannelMax;

  m_wave_header.Format.nChannels  = m_OutputChannels;
  m_wave_header.dwChannelMask     = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  // set the input format, and get the channel layout so we know what we need to open
  enum PCMChannels *outLayout = m_remap.SetInputFormat (iChannels, channelMap, uiBitsPerSample / 8, uiSamplesPerSec);;

  if (!m_Passthrough && channelMap && outLayout)
  {
    /* setup output channel map */
    m_OutputChannels = 0;
    int ch = 0, map;
    int chan = 0;
    while(outLayout[ch] != PCM_INVALID && chan < OMX_AUDIO_MAXCHANNELS)
    {
      for(map = 0; map < OMX_MAX_CHANNELS; ++map)
      {
        if (outLayout[ch] == OMXChannelMap[map])
        {
          m_pcm_output.eChannelMapping[chan] = OMXChannels[map]; 
          chan++;
          break;
        }
      }
      ++ch;
    }

    m_OutputChannels = chan;

    /* setup input channel map */
    for (chan=0; chan < OMX_AUDIO_MAXCHANNELS; chan++)
      m_pcm_input.eChannelMapping[chan] = OMX_AUDIO_ChannelNone;

    ch = 0;
    map = 0;
    chan = 0;

    while(channelMap[ch] != PCM_INVALID && chan < iChannels)
    {
      for(map = 0; map < OMX_MAX_CHANNELS; ++map)
      {
        if (channelMap[ch] == OMXChannelMap[map])
        {
          m_pcm_input.eChannelMapping[chan] = OMXChannels[map]; 
          m_wave_header.dwChannelMask |= WAVEChannels[map];
          chan++;
          break;
        }
      }
      ++ch;
    }
  }

  // set the m_pcm_output parameters
  m_pcm_output.eNumData            = OMX_NumericalDataSigned;
  m_pcm_output.eEndian             = OMX_EndianLittle;
  m_pcm_output.bInterleaved        = OMX_TRUE;
  m_pcm_output.nBitPerSample       = uiBitsPerSample;
  m_pcm_output.ePCMMode            = OMX_AUDIO_PCMModeLinear;
  m_pcm_output.nChannels           = m_OutputChannels;
  m_pcm_output.nSamplingRate       = uiSamplesPerSec;

  m_SampleRate    = uiSamplesPerSec;
  m_BitsPerSample = uiBitsPerSample;
  m_BufferLen     = m_BytesPerSec = uiSamplesPerSec * (uiBitsPerSample >> 3) * m_InputChannels;
  m_BufferLen     *= AUDIO_BUFFER_SECONDS;
  m_ChunkLen      = 6144;
  //m_ChunkLen      = 2048;

  m_wave_header.Samples.wValidBitsPerSample = uiBitsPerSample;
  m_wave_header.Samples.wSamplesPerBlock    = 0;
  m_wave_header.Format.nChannels            = m_InputChannels;
  m_wave_header.Format.nBlockAlign          = m_InputChannels * (uiBitsPerSample >> 3);
  m_wave_header.Format.wFormatTag           = WAVE_FORMAT_PCM;
  m_wave_header.Format.nSamplesPerSec       = uiSamplesPerSec;
  m_wave_header.Format.nAvgBytesPerSec      = m_BytesPerSec;
  m_wave_header.Format.wBitsPerSample       = uiBitsPerSample;
  m_wave_header.Samples.wValidBitsPerSample = uiBitsPerSample;
  m_wave_header.Format.cbSize               = 0;
  m_wave_header.SubFormat                   = KSDATAFORMAT_SUBTYPE_PCM;

  //m_SampleSize              = (m_pcm_output.nChannels * m_pcm_output.nBitPerSample * m_pcm_output.nSamplingRate)>>3;
  m_SampleSize              = (m_pcm_input.nChannels * m_pcm_output.nBitPerSample * m_pcm_output.nSamplingRate)>>3;

  m_pcm_input.eNumData              = OMX_NumericalDataSigned;
  m_pcm_input.eEndian               = OMX_EndianLittle;
  m_pcm_input.bInterleaved          = OMX_TRUE;
  m_pcm_input.nBitPerSample         = uiBitsPerSample;
  m_pcm_input.ePCMMode              = OMX_AUDIO_PCMModeLinear;
  m_pcm_input.nChannels             = m_InputChannels;
  m_pcm_input.nSamplingRate         = uiSamplesPerSec;

  //PrintPCM(&m_pcm_input);
  //PrintPCM(&m_pcm_output);

  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  std::string componentName = "";

  componentName = "OMX.broadcom.audio_render";
  if(!m_omx_render.Initialize(componentName, OMX_IndexParamAudioInit))
    return false;

  OMX_CONFIG_BOOLEANTYPE configBool;
  OMX_INIT_STRUCTURE(configBool);
  configBool.bEnabled = OMX_FALSE;

	omx_err = m_omx_render.SetConfig(OMX_IndexConfigBrcmClockReferenceSource, &configBool);
	if (omx_err != OMX_ErrorNone)
	{
		return false;
	}
	
  OMX_CONFIG_BRCMAUDIODESTINATIONTYPE audioDest;
  OMX_INIT_STRUCTURE(audioDest);
  strncpy((char *)audioDest.sName, device.c_str(), strlen(device.c_str()));

  omx_err = m_omx_render.SetConfig(OMX_IndexConfigBrcmAudioDestination, &audioDest);
  if (omx_err != OMX_ErrorNone)
    return false;

  componentName = "OMX.broadcom.audio_decode";
  if(!m_omx_decoder.Initialize(componentName, OMX_IndexParamAudioInit))
    return false;

  if(!m_Passthrough)
  {
    componentName = "OMX.broadcom.audio_mixer";
    if(!m_omx_mixer.Initialize(componentName, OMX_IndexParamAudioInit))
      return false;
  }

  if(m_Passthrough)
  {
    OMX_CONFIG_BOOLEANTYPE boolType;
    OMX_INIT_STRUCTURE(boolType);
    boolType.bEnabled = OMX_TRUE;
    omx_err = m_omx_decoder.SetParameter(OMX_IndexParamBrcmDecoderPassThrough, &boolType);
    if(omx_err != OMX_ErrorNone)
    {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - Error OMX_IndexParamBrcmDecoderPassThrough 0x%08x", omx_err);
      return false;
    }
  }

  // set up the number/size of buffers
  OMX_PARAM_PORTDEFINITIONTYPE port_param;
  OMX_INIT_STRUCTURE(port_param);
  port_param.nPortIndex = m_omx_decoder.GetInputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_param);
  if(omx_err != OMX_ErrorNone)
  {
    ofLog(OF_LOG_ERROR, "COMXAudio::Initialize error get OMX_IndexParamPortDefinition omx_err(0x%08x)\n", omx_err);
    return false;
  }

  port_param.format.audio.eEncoding = m_eEncoding;

  port_param.nBufferSize = m_ChunkLen;
  port_param.nBufferCountActual = m_BufferLen / m_ChunkLen;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &port_param);
  if(omx_err != OMX_ErrorNone)
  {
    ofLog(OF_LOG_ERROR, "COMXAudio::Initialize error set OMX_IndexParamPortDefinition omx_err(0x%08x)\n", omx_err);
    return false;
  }

  if(m_HWDecode)
  {
    OMX_AUDIO_PARAM_PORTFORMATTYPE formatType;
    OMX_INIT_STRUCTURE(formatType);
    formatType.nPortIndex = m_omx_decoder.GetInputPort();

    formatType.eEncoding = m_eEncoding;

    omx_err = m_omx_decoder.SetParameter(OMX_IndexParamAudioPortFormat, &formatType);
    if(omx_err != OMX_ErrorNone)
    {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize error OMX_IndexParamAudioPortFormat omx_err(0x%08x)\n", omx_err);
      return false;
    }
  }

  if(m_av_clock == NULL)
  {
    /* no external clock set. generate one */
    m_external_clock = false;

    m_av_clock = new OMXClock();
    
    if(!m_av_clock->OMXInitialize(false, true))
    {
      delete m_av_clock;
      m_av_clock = NULL;
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize error creating av clock");
      return false;
    }
  }

  m_omx_clock = m_av_clock->GetOMXClock();

  m_omx_tunnel_clock.Initialize(m_omx_clock, m_omx_clock->GetInputPort(), &m_omx_render, m_omx_render.GetInputPort()+1);

  omx_err = m_omx_tunnel_clock.Establish(false);
  if(omx_err != OMX_ErrorNone)
  {
    ofLog(OF_LOG_ERROR, "COMXAudio::Initialize m_omx_tunnel_clock.Establish");
    return false;
  }

  if(!m_external_clock)
  {
    omx_err = m_omx_clock->SetStateForComponent(OMX_StateExecuting);
    if (omx_err != OMX_ErrorNone)
    {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize m_omx_clock.SetStateForComponent");
      return false;
    }
  }

  /*
  m_pcm_output.nPortIndex          = m_omx_render.GetInputPort();
  omx_err = m_omx_render.SetParameter(OMX_IndexParamAudioPcm, &m_pcm_output);
  if(omx_err != OMX_ErrorNone)
  {
    ofLog(OF_LOG_VERBOSE, "COMXAudio::Initialize OMX_IndexParamAudioPcm omx_err(0x%08x)\n", omx_err);
    return false;
  }
  */

  omx_err = m_omx_decoder.AllocInputBuffers();
  if(omx_err != OMX_ErrorNone) 
  {
    ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - Error alloc buffers 0x%08x", omx_err);
    return false;
  }

  if(!m_Passthrough)
  {
    m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_mixer, m_omx_mixer.GetInputPort());
    omx_err = m_omx_tunnel_decoder.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - Error m_omx_tunnel_decoder.Establish 0x%08x", omx_err);
      return false;
    }
  
    omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone) {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - Error setting OMX_StateExecuting 0x%08x", omx_err);
      return false;
    }

    m_omx_tunnel_mixer.Initialize(&m_omx_mixer, m_omx_mixer.GetOutputPort(), &m_omx_render, m_omx_render.GetInputPort());
    omx_err = m_omx_tunnel_mixer.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - Error m_omx_tunnel_decoder.Establish 0x%08x", omx_err);
      return false;
    }
  
    omx_err = m_omx_mixer.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone) {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - Error setting OMX_StateExecuting 0x%08x", omx_err);
      return false;
    }
  }
  else
  {
    m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_render, m_omx_render.GetInputPort());
    omx_err = m_omx_tunnel_decoder.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - Error m_omx_tunnel_decoder.Establish 0x%08x", omx_err);
      return false;
    }
  
    omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone) {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - Error setting OMX_StateExecuting 0x%08x", omx_err);
      return false;
    }
  }

  omx_err = m_omx_render.SetStateForComponent(OMX_StateExecuting);
  if(omx_err != OMX_ErrorNone) 
  {
    ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - Error setting OMX_StateExecuting 0x%08x", omx_err);
    return false;
  }

  if(m_eEncoding == OMX_AUDIO_CodingPCM)
  {
    OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();
    if(omx_buffer == NULL)
    {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - buffer error 0x%08x", omx_err);
      return false;
    }

    omx_buffer->nOffset = 0;
    omx_buffer->nFilledLen = sizeof(m_wave_header);
    if(omx_buffer->nFilledLen > omx_buffer->nAllocLen)
    {
      ofLog(OF_LOG_ERROR, "COMXAudio::Initialize - omx_buffer->nFilledLen > omx_buffer->nAllocLen");
      return false;
    }
    memset((unsigned char *)omx_buffer->pBuffer, 0x0, omx_buffer->nAllocLen);
    memcpy((unsigned char *)omx_buffer->pBuffer, &m_wave_header, omx_buffer->nFilledLen);
    omx_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;

    omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      ofLog(OF_LOG_ERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
  } 
  else if(m_HWDecode)
  {
    // send decoder config
    if(m_extrasize > 0 && m_extradata != NULL)
    {
      OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();
  
      if(omx_buffer == NULL)
      {
        ofLog(OF_LOG_ERROR, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
        return false;
      }
  
      omx_buffer->nOffset = 0;
      omx_buffer->nFilledLen = m_extrasize;
      if(omx_buffer->nFilledLen > omx_buffer->nAllocLen)
      {
        ofLog(OF_LOG_ERROR, "%s::%s - omx_buffer->nFilledLen > omx_buffer->nAllocLen", CLASSNAME, __func__);
        return false;
      }

      memset((unsigned char *)omx_buffer->pBuffer, 0x0, omx_buffer->nAllocLen);
      memcpy((unsigned char *)omx_buffer->pBuffer, m_extradata, omx_buffer->nFilledLen);
      omx_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;
  
      omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
      if (omx_err != OMX_ErrorNone)
      {
        ofLog(OF_LOG_ERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
        return false;
      }
    }
  }

  m_Initialized   = true;
  m_setStartTime  = true;
  m_first_frame   = true;

  SetCurrentVolume(m_CurrentVolume);

  ofLog(OF_LOG_VERBOSE, "COMXAudio::Initialize Ouput bps %d samplerate %d channels %d device %s buffer size %d bytes per second %d passthrough %d hwdecode %d", 
      (int)m_pcm_output.nBitPerSample, (int)m_pcm_output.nSamplingRate, (int)m_pcm_output.nChannels, deviceuse.c_str(), m_BufferLen, m_BytesPerSec, m_Passthrough, m_HWDecode);

  return true;
}

//***********************************************************************************************
bool COMXAudio::Deinitialize()
{
  if(!m_Initialized)
    return true;

  if(!m_external_clock && m_av_clock != NULL)
    m_av_clock->OMXStop();

  m_omx_tunnel_decoder.Flush();
  if(!m_Passthrough)
    m_omx_tunnel_mixer.Flush();
  m_omx_tunnel_clock.Flush();

	bool noWait = true;
  m_omx_tunnel_clock.Deestablish(noWait);
  if(!m_Passthrough)
    m_omx_tunnel_mixer.Deestablish(noWait);
  m_omx_tunnel_decoder.Deestablish(noWait);

  //m_omx_decoder.FlushInput();

	bool doFlush = false;
  m_omx_render.Deinitialize(doFlush);
  if(!m_Passthrough)
    //m_omx_mixer.Deinitialize(doFlush);
  m_omx_decoder.Deinitialize(doFlush);

  m_Initialized = false;
  m_BytesPerSec = 0;
  m_BufferLen   = 0;

  if(!m_external_clock && m_av_clock != NULL)
  {
    delete m_av_clock;
    m_av_clock  = NULL;
    m_external_clock = false;
  }

  m_omx_clock = NULL;
  m_av_clock  = NULL;

  m_Initialized = false;
  m_LostSync    = true;
  m_HWDecode    = false;

  if(m_extradata)
    free(m_extradata);
  m_extradata = NULL;
  m_extrasize = 0;

  m_dllAvUtil.Unload();

  m_setStartTime  = true;
  m_first_frame   = true;

  return true;
}

void COMXAudio::Flush()
{
  if(!m_Initialized)
    return;

  m_omx_decoder.FlushInput();
  m_omx_tunnel_decoder.Flush();
  if(!m_Passthrough)
    m_omx_tunnel_mixer.Flush();
  
  //m_setStartTime  = true;
  m_LostSync      = true;
  //m_first_frame   = true;
}

//***********************************************************************************************
bool COMXAudio::Pause()
{
  if (!m_Initialized)
     return -1;

  if(m_Pause) return true;
  m_Pause = true;

  m_omx_decoder.SetStateForComponent(OMX_StatePause);

  return true;
}

//***********************************************************************************************
bool COMXAudio::Resume()
{
  if (!m_Initialized)
     return -1;

  if(!m_Pause) return true;
  m_Pause = false;

  m_omx_decoder.SetStateForComponent(OMX_StateExecuting);

  return true;
}

//***********************************************************************************************
bool COMXAudio::Stop()
{
  if (!m_Initialized)
     return -1;

  Flush();

  m_Pause = false;

  return true;
}

//***********************************************************************************************
long COMXAudio::GetCurrentVolume() const
{
  return m_CurrentVolume;
}

//***********************************************************************************************
void COMXAudio::Mute(bool bMute)
{
  if(!m_Initialized)
    return;

  if (bMute)
    SetCurrentVolume(VOLUME_MINIMUM);
  else
    SetCurrentVolume(m_CurrentVolume);
}

//***********************************************************************************************
bool COMXAudio::SetCurrentVolume(long nVolume)
{
  if(!m_Initialized || m_Passthrough)
    return false;

  m_CurrentVolume = nVolume;

  if((m_downmix_channels == 6 || m_downmix_channels == 8) &&
     m_OutputChannels == 2)
  {
    // Convert from millibels to amplitude ratio
    double r = pow(10, nVolume / 2000.0);

    const float* coeff = NULL;

    switch(m_downmix_channels)
    {
      case 6:
        coeff = downmixing_coefficients_6;
        break;
      case 8:
        coeff = downmixing_coefficients_8;
        break;
      default:
        assert(0);
    }

    if(m_normalize_downmix)
    {
      double sum_L = 0;
      double sum_R = 0;

      for(size_t i = 0; i < 16; ++i)
      {
        if(i & 1)
          sum_R += coeff[i];
        else
          sum_L += coeff[i];
      }

      r /= max(sum_L, sum_R);
    }

    OMX_CONFIG_BRCMAUDIODOWNMIXCOEFFICIENTS mix;
    OMX_INIT_STRUCTURE(mix);
    mix.nPortIndex = m_omx_mixer.GetInputPort();

    if(sizeof(mix.coeff)/sizeof(mix.coeff[0]) == 16)
	{
		ofLogError() << "Unexpected OMX_CONFIG_BRCMAUDIODOWNMIXCOEFFICIENTS::coeff length";
	}
        

    for(size_t i = 0; i < 16; ++i)
      mix.coeff[i] = static_cast<unsigned int>(0x10000 * (coeff[i] * r));

    OMX_ERRORTYPE omx_err =
      m_omx_mixer.SetConfig(OMX_IndexConfigBrcmAudioDownmixCoefficients, &mix);

    if(omx_err != OMX_ErrorNone)
    {
      ofLog(OF_LOG_ERROR, "%s::%s - error setting OMX_IndexConfigBrcmAudioDownmixCoefficients, error 0x%08x\n",
                CLASSNAME, __func__, omx_err);
      return false;
    }
  }
  else
  {
	  
    OMX_AUDIO_CONFIG_VOLUMETYPE volume;
    OMX_INIT_STRUCTURE(volume);
    volume.nPortIndex = m_omx_render.GetInputPort();

    volume.sVolume.nValue = nVolume;

    OMX_ERRORTYPE omx_err =
      m_omx_render.SetConfig(OMX_IndexConfigAudioVolume, &volume);

    if(omx_err != OMX_ErrorNone)
    {
      ofLog(OF_LOG_ERROR, "%s::%s - error setting OMX_IndexConfigAudioVolume, error 0x%08x\n",
                CLASSNAME, __func__, omx_err);
      return false;
    }else 
	{
		ofLogVerbose(__func__) << "Set Volume to " << nVolume;
		ofLogVerbose(__func__) << "volume.bLinear: " << volume.bLinear;
	}

  }

  return true;
}


//***********************************************************************************************
unsigned int COMXAudio::GetSpace()
{
  int free = m_omx_decoder.GetInputBufferSpace();
  return free;
}

unsigned int COMXAudio::AddPackets(const void* data, unsigned int len)
{
  return AddPackets(data, len, 0, 0);
}

//***********************************************************************************************
unsigned int COMXAudio::AddPackets(const void* data, unsigned int len, double dts, double pts)
{
  if(!m_Initialized) {
    ofLog(OF_LOG_ERROR,"COMXAudio::AddPackets - sanity failed. no valid play handle!");
    return len;
  }

  if(m_eEncoding == OMX_AUDIO_CodingDTS && m_LostSync && (m_Passthrough || m_HWDecode))
  {
    int skip = SyncDTS((uint8_t *)data, len);
    if(skip > 0)
      return len;
  }

  if(m_eEncoding == OMX_AUDIO_CodingDDP && m_LostSync && (m_Passthrough || m_HWDecode))
  {
    int skip = SyncAC3((uint8_t *)data, len);
    if(skip > 0)
      return len;
  }

  unsigned int demuxer_bytes = (unsigned int)len;
  uint8_t *demuxer_content = (uint8_t *)data;

  OMX_ERRORTYPE omx_err;

  OMX_BUFFERHEADERTYPE *omx_buffer = NULL;

  while(demuxer_bytes)
  {
    // 200ms timeout
    omx_buffer = m_omx_decoder.GetInputBuffer(200);

    if(omx_buffer == NULL)
    {
      ofLog(OF_LOG_ERROR, "COMXAudio::Decode timeout");
      return len;
    }

    omx_buffer->nOffset = 0;
    omx_buffer->nFlags  = 0;

    omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
    memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

    /*
    if (m_SampleSize > 0 && pts != DVD_NOPTS_VALUE && !(omx_buffer->nFlags & OMX_BUFFERFLAG_TIME_UNKNOWN) && !m_Passthrough && !m_HWDecode)
    {
      pts += ((double)omx_buffer->nFilledLen * DVD_TIME_BASE) / m_SampleSize;
    }
    printf("ADec : pts %f omx_buffer 0x%08x buffer 0x%08x number %d\n", 
          (float)pts / AV_TIME_BASE, omx_buffer, omx_buffer->pBuffer, (int)omx_buffer->pAppPrivate);
    */

	  uint64_t val  = (uint64_t)(pts == DVD_NOPTS_VALUE) ? 0 : pts;
	  
	  if(m_setStartTime)
	  {
		  omx_buffer->nFlags = OMX_BUFFERFLAG_STARTTIME;
		  
		  m_setStartTime = false;
	  }
	  else if(pts == DVD_NOPTS_VALUE)
	  {
		  omx_buffer->nFlags = OMX_BUFFERFLAG_TIME_UNKNOWN;
	  }
	  
	  omx_buffer->nTimeStamp = ToOMXTime(val);
	  
	  demuxer_bytes -= omx_buffer->nFilledLen;
	  demuxer_content += omx_buffer->nFilledLen;
	  
	  if(demuxer_bytes == 0)
		  omx_buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;
	  
	  int nRetry = 0;
	  
    while(true)
    {
      omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
      if (omx_err == OMX_ErrorNone)
      {
        break;
      }
      else
      {
        ofLog(OF_LOG_ERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
        nRetry++;
      }
      if(nRetry == 5)
      {
        ofLog(OF_LOG_VERBOSE, "%s::%s - OMX_EmptyThisBuffer() finaly failed\n", CLASSNAME, __func__);
        printf("%s::%s - OMX_EmptyThisBuffer() finaly failed\n", CLASSNAME, __func__);
        return 0;
      }
    }

    if(m_first_frame)
    {
      m_first_frame = false;
      //m_omx_render.WaitForEvent(OMX_EventPortSettingsChanged);

      m_omx_render.DisablePort(m_omx_render.GetInputPort(), false);
      if(!m_Passthrough)
      {
        m_omx_mixer.DisablePort(m_omx_mixer.GetOutputPort(), false);
        m_omx_mixer.DisablePort(m_omx_mixer.GetInputPort(), false);
      }
      m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), false);

      if(!m_Passthrough)
      {
        if(m_HWDecode)
        {
          OMX_INIT_STRUCTURE(m_pcm_input);
          m_pcm_input.nPortIndex      = m_omx_decoder.GetOutputPort();
          omx_err = m_omx_decoder.GetParameter(OMX_IndexParamAudioPcm, &m_pcm_input);
          if(omx_err != OMX_ErrorNone)
          {
            ofLog(OF_LOG_ERROR, "COMXAudio::AddPackets error GetParameter 1 omx_err(0x%08x)\n", omx_err);
          }
        }

        /* setup mixer input */
        m_pcm_input.nPortIndex      = m_omx_mixer.GetInputPort();
        omx_err = m_omx_mixer.SetParameter(OMX_IndexParamAudioPcm, &m_pcm_input);
        if(omx_err != OMX_ErrorNone)
        {
          ofLog(OF_LOG_ERROR, "COMXAudio::AddPackets error SetParameter 1 omx_err(0x%08x)\n", omx_err);
        }
        omx_err = m_omx_mixer.GetParameter(OMX_IndexParamAudioPcm, &m_pcm_input);
        if(omx_err != OMX_ErrorNone)
        {
          ofLog(OF_LOG_ERROR, "COMXAudio::AddPackets error GetParameter 2  omx_err(0x%08x)\n", omx_err);
        }

        /* setup mixer output */
        m_pcm_output.nPortIndex      = m_omx_mixer.GetOutputPort();
        omx_err = m_omx_mixer.SetParameter(OMX_IndexParamAudioPcm, &m_pcm_output);
        if(omx_err != OMX_ErrorNone)
        {
          ofLog(OF_LOG_ERROR, "COMXAudio::AddPackets error SetParameter 1 omx_err(0x%08x)\n", omx_err);
        }
        omx_err = m_omx_mixer.GetParameter(OMX_IndexParamAudioPcm, &m_pcm_output);
        if(omx_err != OMX_ErrorNone)
        {
          ofLog(OF_LOG_ERROR, "COMXAudio::AddPackets error GetParameter 2  omx_err(0x%08x)\n", omx_err);
        }

        m_pcm_output.nPortIndex      = m_omx_render.GetInputPort();
        omx_err = m_omx_render.SetParameter(OMX_IndexParamAudioPcm, &m_pcm_output);
        if(omx_err != OMX_ErrorNone)
        {
          ofLog(OF_LOG_ERROR, "COMXAudio::AddPackets error SetParameter 1 omx_err(0x%08x)\n", omx_err);
        }
        omx_err = m_omx_render.GetParameter(OMX_IndexParamAudioPcm, &m_pcm_output);
        if(omx_err != OMX_ErrorNone)
        {
          ofLog(OF_LOG_ERROR, "COMXAudio::AddPackets error GetParameter 2  omx_err(0x%08x)\n", omx_err);
        }

        //PrintPCM(&m_pcm_input);
        //PrintPCM(&m_pcm_output);
      }
      else
      {
        m_pcm_output.nPortIndex      = m_omx_decoder.GetOutputPort();
        m_omx_decoder.GetParameter(OMX_IndexParamAudioPcm, &m_pcm_output);
       // PrintPCM(&m_pcm_output);

        OMX_AUDIO_PARAM_PORTFORMATTYPE formatType;
        OMX_INIT_STRUCTURE(formatType);
        formatType.nPortIndex = m_omx_render.GetInputPort();

        omx_err = m_omx_render.GetParameter(OMX_IndexParamAudioPortFormat, &formatType);
        if(omx_err != OMX_ErrorNone)
        {
          ofLog(OF_LOG_ERROR, "COMXAudio::AddPackets error OMX_IndexParamAudioPortFormat omx_err(0x%08x)\n", omx_err);
          assert(0);
        }

        formatType.eEncoding = m_eEncoding;

        omx_err = m_omx_render.SetParameter(OMX_IndexParamAudioPortFormat, &formatType);
        if(omx_err != OMX_ErrorNone)
        {
          ofLog(OF_LOG_ERROR, "COMXAudio::AddPackets error OMX_IndexParamAudioPortFormat omx_err(0x%08x)\n", omx_err);
          assert(0);
        }

        if(m_eEncoding == OMX_AUDIO_CodingDDP)
        {
          OMX_AUDIO_PARAM_DDPTYPE m_ddParam;
          OMX_INIT_STRUCTURE(m_ddParam);

          m_ddParam.nPortIndex      = m_omx_render.GetInputPort();

          m_ddParam.nChannels       = m_InputChannels; //(m_InputChannels == 6) ? 8 : m_InputChannels;
          m_ddParam.nSampleRate     = m_SampleRate;
          m_ddParam.eBitStreamId    = OMX_AUDIO_DDPBitStreamIdAC3;
          m_ddParam.nBitRate        = 0;

          for(unsigned int i = 0; i < OMX_MAX_CHANNELS; i++)
          {
            if(i >= m_ddParam.nChannels)
              break;

            m_ddParam.eChannelMapping[i] = OMXChannels[i];
          }
  
          m_omx_render.SetParameter(OMX_IndexParamAudioDdp, &m_ddParam);
          m_omx_render.GetParameter(OMX_IndexParamAudioDdp, &m_ddParam);
          PrintDDP(&m_ddParam);
        }
        else if(m_eEncoding == OMX_AUDIO_CodingDTS)
        {
          m_dtsParam.nPortIndex      = m_omx_render.GetInputPort();

          m_dtsParam.nChannels       = m_InputChannels; //(m_InputChannels == 6) ? 8 : m_InputChannels;
          m_dtsParam.nBitRate        = 0;

          for(unsigned int i = 0; i < OMX_MAX_CHANNELS; i++)
          {
            if(i >= m_dtsParam.nChannels)
              break;

            m_dtsParam.eChannelMapping[i] = OMXChannels[i];
          }
  
          m_omx_render.SetParameter(OMX_IndexParamAudioDts, &m_dtsParam);
          m_omx_render.GetParameter(OMX_IndexParamAudioDts, &m_dtsParam);
          PrintDTS(&m_dtsParam);
        }
      }

      m_omx_render.EnablePort(m_omx_render.GetInputPort(), false);
      if(!m_Passthrough)
      {
        m_omx_mixer.EnablePort(m_omx_mixer.GetOutputPort(), false);
        m_omx_mixer.EnablePort(m_omx_mixer.GetInputPort(), false);
      }
      m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), false);
    }
 
  }

  return len;
}

//***********************************************************************************************
float COMXAudio::GetDelay()
{
  unsigned int free = m_omx_decoder.GetInputBufferSize() - m_omx_decoder.GetInputBufferSpace();
  return (float)free / (float)m_BytesPerSec;
}

float COMXAudio::GetCacheTime()
{
  float fBufferLenFull = (float)m_BufferLen - (float)GetSpace();
  if(fBufferLenFull < 0)
    fBufferLenFull = 0;
  float ret = fBufferLenFull / (float)m_BytesPerSec;
  return ret;
}

float COMXAudio::GetCacheTotal()
{
  return (float)m_BufferLen / (float)m_BytesPerSec;
}

//***********************************************************************************************
unsigned int COMXAudio::GetChunkLen()
{
  return m_ChunkLen;
}
//***********************************************************************************************
int COMXAudio::SetPlaySpeed(int iSpeed)
{
  return 0;
}

unsigned int COMXAudio::GetAudioRenderingLatency()
{
  OMX_PARAM_U32TYPE param;
  OMX_INIT_STRUCTURE(param);
  param.nPortIndex = m_omx_render.GetInputPort();

  OMX_ERRORTYPE omx_err =
    m_omx_render.GetConfig(OMX_IndexConfigAudioRenderingLatency, &param);

  if(omx_err != OMX_ErrorNone)
  {
    ofLog(OF_LOG_VERBOSE, "COMXAudio::GetAudioRenderingLatency: "
                        "error getting OMX_IndexConfigAudioRenderingLatency");
    return 0;
  }

  return param.nU32;
}

void COMXAudio::WaitCompletion()
{
 ofLogVerbose() << "COMXAudio::WaitCompletion";
  if(!m_Initialized || m_Pause)
    return;

  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();

  if(omx_buffer == NULL)
  {
    ofLog(OF_LOG_VERBOSE, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
    return;
  }

  omx_buffer->nOffset     = 0;
  omx_buffer->nFilledLen  = 0;
  omx_buffer->nTimeStamp  = ToOMXTime(0LL);

  omx_buffer->nFlags = OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_EOS | OMX_BUFFERFLAG_TIME_UNKNOWN;

  omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
  if (omx_err != OMX_ErrorNone)
  {
    ofLog(OF_LOG_VERBOSE, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return;
  }

  // clock_gettime(CLOCK_REALTIME, &starttime);

  while(true)
  {
    if(m_omx_render.IsEOS())
      break;
    OMXClock::OMXSleep(50);
  }

  while(true)
  {
    if(!GetAudioRenderingLatency())
      break;

    OMXClock::OMXSleep(50);
  }

  return;
}





bool COMXAudio::SetClock(OMXClock *clock)
{
  if(m_av_clock != NULL)
    return false;

  m_av_clock = clock;
  m_external_clock = true;
  return true;
}
 
void COMXAudio::SetCodingType(CodecID codec)
{
	switch(codec)
	{ 
		case CODEC_ID_DTS:
			ofLogVerbose(__func__) << "OMX_AUDIO_CodingDTS";
			m_eEncoding = OMX_AUDIO_CodingDTS;
			break;
		case CODEC_ID_AC3:
		case CODEC_ID_EAC3:
			ofLogVerbose(__func__) << "OMX_AUDIO_CodingDDP";
			m_eEncoding = OMX_AUDIO_CodingDDP;
			break;
		default:
			ofLogVerbose(__func__) << "OMX_AUDIO_CodingPCM";
			m_eEncoding = OMX_AUDIO_CodingPCM;
			break;
	} 
}

bool COMXAudio::CanHWDecode(CodecID codec)
{
	string codecName = "UNKNOWN";
	switch(codec)
	{ 
			
		case CODEC_ID_VORBIS:
			 codecName =  "OMX_AUDIO_CodingVORBIS";
			 m_eEncoding = OMX_AUDIO_CodingVORBIS;
			 m_HWDecode = false;
			 break;
		case CODEC_ID_AAC:
			codecName =  "OMX_AUDIO_CodingAAC";
			 m_eEncoding = OMX_AUDIO_CodingAAC;
			 m_HWDecode = false;
			 break;
			 
		case CODEC_ID_MP2:
		case CODEC_ID_MP3:
			codecName = "OMX_AUDIO_CodingMP3";
			m_eEncoding = OMX_AUDIO_CodingMP3;
			m_HWDecode = true;
			break;
		case CODEC_ID_DTS:
			codecName =  "OMX_AUDIO_CodingDTS";
			m_eEncoding = OMX_AUDIO_CodingDTS;
			m_HWDecode = true;
			break;
		case CODEC_ID_AC3:
		case CODEC_ID_EAC3:
			codecName =  "OMX_AUDIO_CodingDDP";
			m_eEncoding = OMX_AUDIO_CodingDDP;
			m_HWDecode = true;
			break;
		default:
			codecName =  "OMX_AUDIO_CodingPCM";
			m_eEncoding = OMX_AUDIO_CodingPCM;
			m_HWDecode = false;
			break;
	} 
	ofLogVerbose(__func__) << "codecName is: " <<  codecName << " m_HWDecode IS:  " << m_HWDecode;
	return m_HWDecode;
}

bool COMXAudio::HWDecode(CodecID codec)
{
	bool ret = false;
	string codecName = "UNKNOWN";
	switch(codec)
	{ 
			
		case CODEC_ID_VORBIS:
			codecName =  "CODEC_ID_VORBIS";
			 ret = false;
			 break;
		case CODEC_ID_AAC:
			 codecName ="CODEC_ID_AAC";
			 ret = false;
			 break;
			 
		case CODEC_ID_MP2:
		case CODEC_ID_MP3:
			codecName ="CODEC_ID_MP2 / CODEC_ID_MP3";
			ret = true;
			break;
		case CODEC_ID_DTS:
			codecName = "CODEC_ID_DTS";
			ret = true;
			break;
		case CODEC_ID_AC3:
		case CODEC_ID_EAC3:
			codecName = "CODEC_ID_AC3 / CODEC_ID_EAC3";
			ret = true;
			break;
		default:
			ret = false;
			break;
	} 
	ofLogVerbose(__func__) << "codecName is " <<  codecName << " RETURNING " << ret;
	return ret;
}

void COMXAudio::PrintChannels(OMX_AUDIO_CHANNELTYPE eChannelMapping[])
{
	for(int i = 0; i < OMX_AUDIO_MAXCHANNELS; i++)
	{
		switch(eChannelMapping[i])
		{
			case OMX_AUDIO_ChannelLF:
				ofLogVerbose(__func__) << "OMX_AUDIO_ChannelLF";
				break;
			case OMX_AUDIO_ChannelRF:
				ofLogVerbose(__func__) << "OMX_AUDIO_ChannelRF";
				break;
			case OMX_AUDIO_ChannelCF:
				ofLogVerbose(__func__) << "OMX_AUDIO_ChannelCF";
				break;
			case OMX_AUDIO_ChannelLS:
				ofLogVerbose(__func__) << "OMX_AUDIO_ChannelLS";
				break;
			case OMX_AUDIO_ChannelRS:
				ofLogVerbose(__func__) << "OMX_AUDIO_ChannelRS";
				break;
			case OMX_AUDIO_ChannelLFE:
				ofLogVerbose(__func__) << "OMX_AUDIO_ChannelLFE";
				break;
			case OMX_AUDIO_ChannelCS:
				ofLogVerbose(__func__) << "OMX_AUDIO_ChannelCS";
				break;
			case OMX_AUDIO_ChannelLR:
				ofLogVerbose(__func__) << "OMX_AUDIO_ChannelLR";
				break;
			case OMX_AUDIO_ChannelRR:
				ofLogVerbose(__func__) << "OMX_AUDIO_ChannelRR";
				break;
			case OMX_AUDIO_ChannelNone:
			case OMX_AUDIO_ChannelKhronosExtensions:
			case OMX_AUDIO_ChannelVendorStartUnused:
			case OMX_AUDIO_ChannelMax:
			default:
				break;
		}
	}
}

void COMXAudio::PrintPCM(OMX_AUDIO_PARAM_PCMMODETYPE *pcm)
{
	
	stringstream info;
	info << "PCM PROPERTIES"	<< "\n";
	info << "nPortIndex: "		<< (int)pcm->nPortIndex			<< "\n";
	info << "eNumData: "		<< pcm->eNumData				<< "\n";
	info << "eEndian: "			<< pcm->eEndian					<< "\n";
	info << "bInterleaved: "	<< (int)pcm->bInterleaved		<< "\n";
	info << "nBitPerSample: "	<< (int)pcm->nBitPerSample		<< "\n";
	info << "ePCMMode: "		<< pcm->ePCMMode				<< "\n";
	info << "nChannels: "		<< (int)pcm->nChannels			<< "\n";
	info << "nSamplingRate: "	<< (int)pcm->nSamplingRate		<< "\n";
	ofLogVerbose(__func__) << "\n" <<  info.str();

  PrintChannels(pcm->eChannelMapping);
}

void COMXAudio::PrintDDP(OMX_AUDIO_PARAM_DDPTYPE *ddparm)
{
	stringstream info;
	info << "DDP PROPERTIES"	<< "\n";	
	info << "nPortIndex         : " <<  (int)ddparm->nPortIndex			<< "\n";
	info << "nChannels          : " <<  (int)ddparm->nChannels			<< "\n";
	info << "nBitRate           : " <<  (int)ddparm->nBitRate			<< "\n";
	info << "nSampleRate        : " <<  (int)ddparm->nSampleRate		<< "\n";
	info << "eBitStreamId       : " <<  (int)ddparm->eBitStreamId		<< "\n";
	info << "eBitStreamMode     : " <<  (int)ddparm->eBitStreamMode		<< "\n";
	info << "eDolbySurroundMode : " <<  (int)ddparm->eDolbySurroundMode << "\n";
	
	ofLogVerbose(__func__) << "\n" <<  info.str();
	
	PrintChannels(ddparm->eChannelMapping);
}
void COMXAudio::PrintDTS(OMX_AUDIO_PARAM_DTSTYPE *dtsparam)
{
	stringstream info;
	info << "DTS PROPERTIES"	<< "\n";
	info << "nPortIndex         : " <<  (int)dtsparam->nPortIndex 				<< "\n";
	info << "nChannels          : " <<  (int)dtsparam->nChannels 				<< "\n";
	info << "nBitRate           : " <<  (int)dtsparam->nBitRate 				<< "\n";
	info << "nSampleRate        : " <<  (int)dtsparam->nSampleRate				<< "\n";
	info << "nFormat            : " <<  (int)dtsparam->nFormat					<< "\n";
	info << "nDtsType           : " <<  (int)dtsparam->nDtsType 				<< "\n";	
	info << "nDtsFrameSizeBytes : " <<  (int)dtsparam->nDtsFrameSizeBytes 		<< "\n";

	ofLogVerbose(__func__) << "\n" <<  info.str();
	
  PrintChannels(dtsparam->eChannelMapping);
}

/* ========================== SYNC FUNCTIONS ========================== */
unsigned int COMXAudio::SyncDTS(BYTE* pData, unsigned int iSize)
{
  OMX_INIT_STRUCTURE(m_dtsParam);

  unsigned int skip;
  unsigned int srCode;
  unsigned int dtsBlocks;
  bool littleEndian;

  for(skip = 0; iSize - skip > 8; ++skip, ++pData)
  {
    if (pData[0] == 0x7F && pData[1] == 0xFE && pData[2] == 0x80 && pData[3] == 0x01) 
    {
      /* 16bit le */
      littleEndian = true; 
      dtsBlocks    = ((pData[4] >> 2) & 0x7f) + 1;
      m_dtsParam.nFormat = 0x1 | 0x2;
    }
    else if (pData[0] == 0x1F && pData[1] == 0xFF && pData[2] == 0xE8 && pData[3] == 0x00 && pData[4] == 0x07 && (pData[5] & 0xF0) == 0xF0) 
    {
      /* 14bit le */
      littleEndian = true;
      dtsBlocks    = (((pData[4] & 0x7) << 4) | (pData[7] & 0x3C) >> 2) + 1;
      m_dtsParam.nFormat = 0x1 | 0x0;
    }
    else if (pData[1] == 0x7F && pData[0] == 0xFE && pData[3] == 0x80 && pData[2] == 0x01) 
    {
      /* 16bit be */ 
      littleEndian = false;
      dtsBlocks    = ((pData[5] >> 2) & 0x7f) + 1;
      m_dtsParam.nFormat = 0x0 | 0x2;
    }
    else if (pData[1] == 0x1F && pData[0] == 0xFF && pData[3] == 0xE8 && pData[2] == 0x00 && pData[5] == 0x07 && (pData[4] & 0xF0) == 0xF0) 
    {
      /* 14bit be */
      littleEndian = false; 
      dtsBlocks    = (((pData[5] & 0x7) << 4) | (pData[6] & 0x3C) >> 2) + 1;
      m_dtsParam.nFormat = 0x0 | 0x0;
    }
    else
    {
      continue;
    }

    if (littleEndian)
    {
      /* if it is not a termination frame, check the next 6 bits are set */
      if ((pData[4] & 0x80) == 0x80 && (pData[4] & 0x7C) != 0x7C)
        continue;

      /* get the frame size */
      m_dtsParam.nDtsFrameSizeBytes = ((((pData[5] & 0x3) << 8 | pData[6]) << 4) | ((pData[7] & 0xF0) >> 4)) + 1;
      srCode = (pData[8] & 0x3C) >> 2;
   }
   else
   {
      /* if it is not a termination frame, check the next 6 bits are set */
      if ((pData[5] & 0x80) == 0x80 && (pData[5] & 0x7C) != 0x7C)
        continue;

      /* get the frame size */
      m_dtsParam.nDtsFrameSizeBytes = ((((pData[4] & 0x3) << 8 | pData[7]) << 4) | ((pData[6] & 0xF0) >> 4)) + 1;
      srCode = (pData[9] & 0x3C) >> 2;
   }

    /* make sure the framesize is sane */
    if (m_dtsParam.nDtsFrameSizeBytes < 96 || m_dtsParam.nDtsFrameSizeBytes > 16384)
      continue;

    m_dtsParam.nSampleRate = DTSFSCod[srCode];

    switch(dtsBlocks << 5)
    {
      case 512 : 
        m_dtsParam.nDtsType = 1;
        break;
      case 1024: 
        m_dtsParam.nDtsType = 2;
        break;
      case 2048: 
        m_dtsParam.nDtsType = 3;
        break;
      default:
        m_dtsParam.nDtsType = 0;
        break;
    }

    //m_dtsParam.nFormat = 1;
    m_dtsParam.nDtsType = 1;

    m_LostSync = false;

    return skip;
  }

  m_LostSync = true;
  return iSize;
}

unsigned int COMXAudio::SyncAC3(BYTE* pData, unsigned int iSize)
{
  unsigned int skip = 0;
  //unsigned int fSize = 0;

  for(skip = 0; iSize - skip > 6; ++skip, ++pData)
  {
    /* search for an ac3 sync word */
    if(pData[0] != 0x0b || pData[1] != 0x77)
      continue;

    uint8_t fscod      = pData[4] >> 6;
    uint8_t frmsizecod = pData[4] & 0x3F;
    uint8_t bsid       = pData[5] >> 3;

    /* sanity checks on the header */
    if (
        fscod      ==   3 ||
        frmsizecod >   37 ||
        bsid       > 0x11
    ) continue;

    /* get the details we need to check crc1 and framesize */
    uint16_t     bitrate   = AC3Bitrates[frmsizecod >> 1];
    unsigned int framesize = 0;
    switch(fscod)
    {
      case 0: framesize = bitrate * 2; break;
      case 1: framesize = (320 * bitrate / 147 + (frmsizecod & 1 ? 1 : 0)); break;
      case 2: framesize = bitrate * 4; break;
    }

    //fSize = framesize * 2;
    m_SampleRate = AC3FSCod[fscod];

    /* dont do extensive testing if we have not lost sync */
    if (!m_LostSync && skip == 0)
      return 0;

    unsigned int crc_size;
    /* if we have enough data, validate the entire packet, else try to validate crc2 (5/8 of the packet) */
    if (framesize <= iSize - skip)
         crc_size = framesize - 1;
    else crc_size = (framesize >> 1) + (framesize >> 3) - 1;

    if (crc_size <= iSize - skip)
      if(m_dllAvUtil.av_crc(m_dllAvUtil.av_crc_get_table(AV_CRC_16_ANSI), 0, &pData[2], crc_size * 2))
        continue;

    /* if we get here, we can sync */
    m_LostSync = false;
    return skip;
  }

  /* if we get here, the entire packet is invalid and we have lost sync */
  m_LostSync = true;
  return iSize;
}

