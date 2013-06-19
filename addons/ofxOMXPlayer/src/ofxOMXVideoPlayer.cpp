#include "ofxOMXVideoPlayer.h"

ofxOMXVideoPlayer::ofxOMXVideoPlayer()
{
	videoWidth			= 0;
	videoHeight			= 0;
	isMPEG				= false;
	hasVideo			= false;
	hasAudio			= false;
	packet				= NULL;
	moviePath			= "moviePath is undefined";
	clock				= NULL;
	bPlaying			= false;
	duration			= 0.0;
	nFrames				= 0;
	doLooping  = true;
	m_buffer_empty = true;
}

void ofxOMXVideoPlayer::loadMovie(string filepath)
{
	moviePath = filepath; 
	rbp.Initialize();
	omxCore.Initialize();
	
	clock = new OMXClock(); 
	bool doDumpFormat = false;

	if(omxReader.Open(moviePath.c_str(), doDumpFormat))
	{
		
		ofLogVerbose() << "omxReader open moviePath PASS: " << moviePath;
		hasVideo     = omxReader.VideoStreamCount();
		int audioStreamCount	 = omxReader.AudioStreamCount();
		if (audioStreamCount>0) 
		{
			hasAudio = true;
			ofLogVerbose() << "HAS AUDIO";
		}else 
		{
			ofLogVerbose() << "NO AUDIO";
		}

		if (hasVideo) 
		{
			ofLogVerbose() << "Video streams detection PASS";
			
			if(clock->OMXInitialize(hasVideo, hasAudio))
			{
				ofLogVerbose() << "clock Init PASS";
				openPlayer();
				
				
			}else 
			{
				ofLogError() << "clock Init FAIL";
			}
		}else 
		{
			ofLogError() << "Video streams detection FAIL";
		}
	}else 
	{
		ofLogError() << "omxReader open moviePath FAIL: "  << moviePath;
	}
	
}
void ofxOMXVideoPlayer::openPlayer()
{
	omxReader.GetHints(OMXSTREAM_VIDEO, streamInfo);
	omxReader.GetHints(OMXSTREAM_AUDIO, audioStreamInfo);
	videoWidth	= streamInfo.width;
	videoHeight	= streamInfo.height;
	ofLogVerbose() << "SET videoWidth: " << videoWidth;
	ofLogVerbose() << "SET videoHeight: " << videoHeight;
	
	bool deinterlace = false;
	bool mpeg = false;
	bool hdmi_clock_sync = true;
	bool use_thread = true;
	float display_aspect = 0.0;

	bPlaying = videoPlayer.Open(streamInfo, clock, deinterlace, mpeg, hdmi_clock_sync, use_thread, display_aspect);
	string deviceString			= "omx:hdmi";
	bool m_passthrough			= false;
	int m_use_hw_audio			= false;
	bool m_boost_on_downmix		= false;
	bool m_thread_player		= true;
	bool didAudioOpen = m_player_audio.Open(audioStreamInfo, clock, &omxReader, deviceString, 
											m_passthrough, m_use_hw_audio,
											m_boost_on_downmix, m_thread_player);
	if (didAudioOpen) 
	{
		ofLogVerbose() << " AUDIO PLAYER OPEN PASS";
	}else
	{
		ofLogVerbose() << " AUDIO PLAYER OPEN FAIL";

	}

	if (isPlaying()) 
	{
		ofLogVerbose() << "streamInfo.nb_frames " << streamInfo.nb_frames;
		ofLogVerbose() << "videoPlayer.GetFPS(): " << videoPlayer.GetFPS();
		
		if(streamInfo.nb_frames>0 && videoPlayer.GetFPS()>0)
		{
			nFrames = streamInfo.nb_frames;
			duration = streamInfo.nb_frames / videoPlayer.GetFPS();
			ofLogVerbose() << "duration SET: " << duration;
		}else 
		{
			//file is weird (like test.h264) and has no reported frames
			//enable File looping hack if looping enabled;
			
			if (doLooping) 
			{
				omxReader.enableFileLoopinghack();
			}
		}
		clock->SetSpeed(DVD_PLAYSPEED_NORMAL);
		clock->OMXStateExecute();
		clock->OMXStart();
		
		ofLogVerbose() << "Opened video PASS";	
		startThread(false, false);
		
	}else 
	{
		ofLogError() << "Opened video FAIL";
	}
}

void ofxOMXVideoPlayer::threadedFunction()
{
	while (isThreadRunning()) 
	{
		struct timespec starttime, endtime;
		while(true)
		{
			if(m_player_audio.Error())
			{
				printf("audio player error. emergency exit!!!\n");
			}
			
			printf("V : %8.02f %8d %8d A : %8.02f %8.02f Cv : %8d Ca : %8d                            \r",
				   clock->OMXMediaTime(), videoPlayer.GetDecoderBufferSize(),
				   videoPlayer.GetDecoderFreeSpace(), m_player_audio.GetCurrentPTS() / DVD_TIME_BASE, 
				   m_player_audio.GetDelay(), videoPlayer.GetCached(), m_player_audio.GetCached());
			
			if(omxReader.IsEof() && !packet)
			{
				if (!m_player_audio.GetCached() && !videoPlayer.GetCached())
					break;
				
				// Abort audio buffering, now we're on our own
				if (m_buffer_empty)
					clock->OMXResume();
				
				OMXClock::OMXSleep(10);
				continue;
			}
			
			/* when the audio buffer runs under 0.1 seconds we buffer up */
			if(hasAudio)
			{
				if(m_player_audio.GetDelay() < 0.1f && !m_buffer_empty)
				{
					if(!clock->OMXIsPaused())
					{
						clock->OMXPause();
						//printf("buffering start\n");
						m_buffer_empty = true;
						clock_gettime(CLOCK_REALTIME, &starttime);
					}
				}
				if(m_player_audio.GetDelay() > (AUDIO_BUFFER_SECONDS * 0.75f) && m_buffer_empty)
				{
					if(clock->OMXIsPaused())
					{
						clock->OMXResume();
						//printf("buffering end\n");
						m_buffer_empty = false;
					}
				}
				if(m_buffer_empty)
				{
					clock_gettime(CLOCK_REALTIME, &endtime);
					if((endtime.tv_sec - starttime.tv_sec) > 1)
					{
						m_buffer_empty = false;
						clock->OMXResume();
						//printf("buffering timed out\n");
					}
				}
			}
			
			if(!packet)
				packet = omxReader.Read(false);
			
			if(hasVideo && packet && omxReader.IsActive(OMXSTREAM_VIDEO, packet->stream_index))
			{
				if(videoPlayer.AddPacket(packet))
					packet = NULL;
				else
					OMXClock::OMXSleep(10);
			}
			else if(hasAudio && packet && packet->codec_type == AVMEDIA_TYPE_AUDIO)
			{
				if(m_player_audio.AddPacket(packet))
					packet = NULL;
				else
					OMXClock::OMXSleep(10);
			}
			else
			{
				if(packet)
				{
					omxReader.FreePacket(packet);
					packet = NULL;
				}
			}
		}
		
	}
}
bool hasPrintedEOF = false;

void ofxOMXVideoPlayer::update()
{
	
	if(!isPlaying())
	{
		return;
	}
	if (doLooping && !packet) 
	{
		if (!omxReader.IsEof())
		{
			packet = omxReader.Read(false);
		}else 
		{
			if (!hasPrintedEOF) {
				hasPrintedEOF = true;
				ofLogVerbose() << "READER IS EOF";
			}

			ofLogVerbose() << "Audio Cache size: " << m_player_audio.GetCached();
			ofLogVerbose() << "Video Cache size: " << videoPlayer.GetCached();
			if (!m_player_audio.GetCached() && !videoPlayer.GetCached())
			{
				double startpts;

				
				ofLogVerbose() << "looping via doLooping option";
				if (hasAudio) 
				{
					m_player_audio.WaitCompletion();
				}
				videoPlayer.WaitCompletion();
				if(omxReader.SeekTime(0 * 1000.0f, 0, &startpts))
				{
					videoPlayer.Flush();
					if(hasAudio)
					{
						m_player_audio.Flush();
					}
					if(packet)
					{
						omxReader.FreePacket(packet);
						packet = NULL;
					}
					
					if(startpts != DVD_NOPTS_VALUE)
					{
						clock->OMXUpdateClock(startpts);
						
					}
					videoPlayer.UnFlush();	
				}
				
				
				//packet = omxReader.Read(true);
			}
		}
	}else 
	{
		if(!packet)
		{
			packet = omxReader.Read(false);
		}
	}

	if(packet && omxReader.IsActive(OMXSTREAM_VIDEO, packet->stream_index))
    {
		videoPlayer.AddPacket(packet);
		packet = NULL;
		if(packet)
		{
			omxReader.FreePacket(packet);
			packet = NULL;
		}
	}else 
	{
		if(hasAudio && packet && packet->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			m_player_audio.AddPacket(packet);
			packet = NULL;
		}
		if(packet)
		{
			omxReader.FreePacket(packet);
			packet = NULL;
		}
	}
	//struct timespec starttime, endtime;

	/* when the audio buffer runs under 0.1 seconds we buffer up */
   /* if(hasAudio)
    {
		if(m_player_audio.GetDelay() < 0.1f && !m_buffer_empty)
		{
			if(!clock->OMXIsPaused())
			{
				clock->OMXPause();
				ofLogVerbose() <<  "buffering start";
				m_buffer_empty = true;
				clock_gettime(CLOCK_REALTIME, &starttime);
			}
		}
		if(m_player_audio.GetDelay() > (AUDIO_BUFFER_SECONDS * 0.75f) && m_buffer_empty)
		{
			if(clock->OMXIsPaused())
			{
				clock->OMXResume();
				ofLogVerbose() << "buffering end";
				m_buffer_empty = false;
			}
		}
		if(m_buffer_empty)
		{
			clock_gettime(CLOCK_REALTIME, &endtime);
			if((endtime.tv_sec - starttime.tv_sec) > 1)
			{
				m_buffer_empty = false;
				clock->OMXResume();
				ofLogVerbose() << "buffering timed out";

			}
		}
    }*/
	  	
}
//--------------------------------------------------------
void ofxOMXVideoPlayer::play()
{
	ofLogVerbose() << "TODO: not sure what to do with this - reopen the player?";
	/*clock->SetSpeed(OMX_PLAYSPEED_NORMAL);
	clock->OMXStateExecute();
	clock->OMXStart();
	bPlaying = openPlayer();*/
}

void ofxOMXVideoPlayer::stop()
{
	clock->OMXStop();
	clock->OMXStateIdle();
	videoPlayer.Close();
	m_player_audio.Close();
	bPlaying = false;
	ofLogVerbose() << "ofxOMXVideoPlayer::stop called";
}

void ofxOMXVideoPlayer::setPaused(bool doPause)
{
	if(doPause)
	{
		videoPlayer.SetSpeed(OMX_PLAYSPEED_PAUSE);
		clock->OMXPause();
	}else 
	{
		videoPlayer.SetSpeed(OMX_PLAYSPEED_NORMAL);
		clock->OMXResume();
	}

			
}

float ofxOMXVideoPlayer::getDuration()
{
	return duration;
}
//inaccurate, I believe as it is referring to decoded frames which can be 
//ahead of rendered frames
int ofxOMXVideoPlayer::getCurrentFrame()
{
	return (int)(videoPlayer.GetCurrentPTS()/ DVD_TIME_BASE)*videoPlayer.GetFPS();
}

int ofxOMXVideoPlayer::getTotalNumFrames()
{
	return nFrames;
}

float ofxOMXVideoPlayer::getWidth()
{
	return videoWidth;
}

float ofxOMXVideoPlayer::getHeight()
{
	return videoHeight;
}

double ofxOMXVideoPlayer::getMediaTime()
{
	double mediaTime = 0.0;
	if(isPlaying()) 
	{
		mediaTime =  clock->OMXMediaTime();
	}
	
	return mediaTime;
}

bool ofxOMXVideoPlayer::isPaused()
{
	if (!clock) 
	{
		ofLogVerbose() << "No clock for pause state inquiry";
		return false;
	}
	return clock->OMXIsPaused();
}

bool ofxOMXVideoPlayer::isPlaying()
{
	return bPlaying;
}

void ofxOMXVideoPlayer::close()
{
	stopThread();
	if(isPlaying()) 
	{
		stop();
	}
	//OMXReader::g_abort = true;
	
	if(packet)
	{
		omxReader.FreePacket(packet);
		packet = NULL;
	}	
	omxReader.Close();
	

	omxCore.Deinitialize();
	rbp.Deinitialize();
	ofLogVerbose() << "reached end of ofxOMXVideoPlayer::close";
}
