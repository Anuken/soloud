/*
SoLoud audio engine
Copyright (c) 2013 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#include <stdlib.h> // rand
#include <math.h> // sin
#include "soloud.h"

namespace SoLoud
{
	AudioProducer::AudioProducer()
	{
		mPlayIndex = 0;
		mFlags = 0;
		mLVolume = 1.0f / (float)sqrt(2.0);
		mRVolume = 1.0f / (float)sqrt(2.0);
		mVolume = 1.0f;
		mBaseSamplerate = 44100.0f;
		mSamplerate = 44100.0f;
		mRelativePlaySpeed = 1.0f;
		mStreamTime = 0.0f;
	}

	AudioProducer::~AudioProducer()
	{
	}

	void AudioProducer::init(int aPlayIndex, float aBaseSamplerate, int aFactoryFlags)
	{
		mPlayIndex = aPlayIndex;
		mBaseSamplerate = aBaseSamplerate;
		mSamplerate = mBaseSamplerate;
		mStreamTime = 0.0f;
		mFlags = 0;

		if (aFactoryFlags & AudioFactory::SHOULD_LOOP)
		{
			mFlags |= AudioProducer::LOOPING;
		}

		if (aFactoryFlags & AudioFactory::STEREO)
		{
			mFlags |= AudioProducer::STEREO;
		}
	}

	int AudioProducer::rewind()
	{
		return 0;
	}

	void AudioProducer::seek(float aSeconds, float *mScratch, int mScratchSize)
	{
		float offset = aSeconds - mStreamTime;
		if (offset < 0)
		{
			if (rewind() == 0)
			{
				// can't do generic seek backwards unless we can rewind.
				return;
			}
			offset = aSeconds;
		}
		int samples_to_discard = (int)floor(mSamplerate * offset);

		while (samples_to_discard)
		{
			int samples = mScratchSize / 2;
			if (samples > samples_to_discard)
				samples = samples_to_discard;
			getAudio(mScratch, samples);
			samples_to_discard -= samples;
		}
	}


	///////////////////////////////////////////////////////////////////////////

	AudioFactory::AudioFactory() 
	{ 
		mFlags = 0; 
		mBaseSamplerate = 44100; 
	}

	AudioFactory::~AudioFactory() 
	{
	}

	void AudioFactory::setLooping(int aLoop)
	{
		if (aLoop)
		{
			mFlags |= SHOULD_LOOP;
		}
		else
		{
			mFlags &= ~SHOULD_LOOP;
		}
	}

	///////////////////////////////////////////////////////////////////////////

	Soloud::Soloud()
	{
		mScratch = NULL;
		mScratchSize = 0;
		mScratchNeeded = 0;
		mChannel = NULL;
		mChannelCount = 0;
		mSamplerate = 0;
		mBufferSize = 0;
		mFlags = 0;
		mGlobalVolume = 0;
		mPlayIndex = 0;
		mMixerData = NULL;
		lockMutex = NULL;
		unlockMutex = NULL;
	}

	Soloud::~Soloud()
	{
		stopAll();
		delete[] mScratch;
		delete[] mChannel;
	}

	void Soloud::init(int aChannels, int aSamplerate, int aBufferSize, int aFlags)
	{
		mGlobalVolume = 1;
		mChannel = new AudioProducer*[aChannels];
		mChannelCount = aChannels;
		int i;
		for (i = 0; i < aChannels; i++)
		{
			mChannel[i] = 0;
		}
		mSamplerate = aSamplerate;
		mScratchSize = 0;
		mScratchNeeded = 1;
		mBufferSize = aBufferSize;
		mFlags = aFlags;
		mPostClipScaler = 0.5f;
	}

	float Soloud::getPostClipScaler()
	{
		return mPostClipScaler;
	}

	void Soloud::setPostClipScaler(float aScaler)
	{
		mPostClipScaler = aScaler;
	}

	void Soloud::setGlobalVolume(float aVolume)
	{
		mGlobalVolume = aVolume;
	}		

	int Soloud::findFreeChannel()
	{
		int i;
		unsigned int lowest_play_index_value = 0xffffffff;
		int lowest_play_index = -1;
		for (i = 0; i < mChannelCount; i++)
		{
			if (mChannel[i] == NULL)
			{
				return i;
			}
			if (((mChannel[i]->mFlags & AudioProducer::PROTECTED) == 0) && 
				mChannel[i]->mPlayIndex < lowest_play_index_value)
			{
				lowest_play_index_value = mChannel[i]->mPlayIndex;
				lowest_play_index = i;
			}
		}
		stop(lowest_play_index);
		return lowest_play_index;
	}

	int Soloud::play(AudioFactory &aSound, float aVolume, float aPan, int aPaused)
	{
		if (lockMutex) lockMutex();
		int ch = findFreeChannel();
		if (ch < 0) 
		{
			if (unlockMutex) unlockMutex();
			return -1;
		}
		mChannel[ch] = aSound.createProducer();
		int handle = ch | (mPlayIndex << 8);

		mChannel[ch]->init(mPlayIndex, aSound.mBaseSamplerate, aSound.mFlags);

		if (aPaused)
		{
			mChannel[ch]->mFlags |= AudioProducer::PAUSED;
		}

		// TODO: mutex is locked at this point, but the following lock/unlock it too..
		setPan(handle, aPan);
		setVolume(handle, aVolume);
		setRelativePlaySpeed(handle, 1);

		mPlayIndex++;
		int scratchneeded = (int)ceil((mChannel[ch]->mSamplerate / mSamplerate) * mBufferSize);
		if (mScratchNeeded < scratchneeded)
		{
			int pot = 1024;
			while (pot < scratchneeded) pot <<= 1;
			mScratchNeeded = pot;
		}
		if (unlockMutex) unlockMutex();
		return handle;
	}	

	int Soloud::getAbsoluteChannelFromHandle(int aChannel)
	{
		int ch = aChannel & 0xff;
		unsigned int idx = aChannel >> 8;
		if (mChannel[ch] &&
			(mChannel[ch]->mPlayIndex & 0xffffff) == idx)
		{
			return ch;
		}
		return -1;		
	}

	int Soloud::getActiveVoiceCount()
	{
		if (lockMutex) lockMutex();
		int i;
		int c = 0;
		for (i = 0; i < mChannelCount; i++)
		{
			if (mChannel[i]) 
			{
				c++;
			}
		}
		if (unlockMutex) unlockMutex();
		return c;
	}

	int Soloud::isValidChannelHandle(int aChannel)
	{
		if (lockMutex) lockMutex();
		if (getAbsoluteChannelFromHandle(aChannel) != -1) 
		{
			if (unlockMutex) unlockMutex();
			return 1;
		}
		if (unlockMutex) unlockMutex();
		return 0;
	}


	float Soloud::getVolume(int aChannel)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return 0;
		}
		float v = mChannel[ch]->mVolume;
		if (unlockMutex) unlockMutex();
		return v;
	}

	float Soloud::getStreamTime(int aChannel)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return 0;
		}
		float v = mChannel[ch]->mStreamTime;
		if (unlockMutex) unlockMutex();
		return v;
	}

	float Soloud::getRelativePlaySpeed(int aChannel)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return 1;
		}
		float v = mChannel[ch]->mRelativePlaySpeed;
		if (unlockMutex) unlockMutex();
		return v;
	}

	void Soloud::setRelativePlaySpeed(int aChannel, float aSpeed)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return;
		}
		mChannel[ch]->mRelativePlaySpeed = aSpeed;
		mChannel[ch]->mSamplerate = mChannel[ch]->mBaseSamplerate * mChannel[ch]->mRelativePlaySpeed;
		int scratchneeded = (int)ceil((mChannel[ch]->mSamplerate / mSamplerate) * mBufferSize);
		if (mScratchNeeded < scratchneeded)
		{
			int pot = 1024;
			while (pot < scratchneeded) pot <<= 1;
			mScratchNeeded = pot;
		}
		if (unlockMutex) unlockMutex();
	}

	float Soloud::getSamplerate(int aChannel)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return 0;
		}
		float v = mChannel[ch]->mBaseSamplerate;
		if (unlockMutex) unlockMutex();
		return v;
	}

	void Soloud::setSamplerate(int aChannel, float aSamplerate)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return;
		}
		mChannel[ch]->mBaseSamplerate = aSamplerate;
		mChannel[ch]->mSamplerate = mChannel[ch]->mBaseSamplerate * mChannel[ch]->mRelativePlaySpeed;
		if (unlockMutex) unlockMutex();
	}

	void Soloud::seek(int aChannel, float aSeconds)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return;
		}
		mChannel[ch]->seek(aSeconds, mScratch, mScratchSize);
		if (unlockMutex) unlockMutex();
	}

	void Soloud::setPause(int aChannel, int aPause)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return;
		}
		if (aPause)
		{
			mChannel[ch]->mFlags |= AudioProducer::PAUSED;
		}
		else
		{
			mChannel[ch]->mFlags &= ~AudioProducer::PAUSED;
		}
		if (unlockMutex) unlockMutex();
	}

	void Soloud::setPauseAll(int aPause)
	{
		if (lockMutex) lockMutex();
		int ch;
		for (ch = 0; ch < mChannelCount; ch++)
		{
			if (mChannel[ch])
			{
				if (aPause)
				{
					mChannel[ch]->mFlags |= AudioProducer::PAUSED;
				}
				else
				{
					mChannel[ch]->mFlags &= ~AudioProducer::PAUSED;
				}
			}
		}
		if (unlockMutex) unlockMutex();
	}


	int Soloud::getPause(int aChannel)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return 0;
		}
		int v = !!(mChannel[ch]->mFlags & AudioProducer::PAUSED);
		if (unlockMutex) unlockMutex();
		return v;
	}

	int Soloud::getProtectChannel(int aChannel)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return 0;
		}
		int v = !!(mChannel[ch]->mFlags & AudioProducer::PROTECTED);
		if (unlockMutex) unlockMutex();
		return v;
	}

	void Soloud::setProtectChannel(int aChannel, int aProtect)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return;
		}
		if (aProtect)
		{
			mChannel[ch]->mFlags |= AudioProducer::PROTECTED;
		}
		else
		{
			mChannel[ch]->mFlags &= ~AudioProducer::PROTECTED;
		}
		if (unlockMutex) unlockMutex();
	}

	void Soloud::setPan(int aChannel, float aPan)
	{
		setPanAbsolute(
			aChannel,
			(float)cos((aPan + 1) * M_PI / 4),
			(float)sin((aPan + 1) * M_PI / 4));
	}

	void Soloud::setPanAbsolute(int aChannel, float aLVolume, float aRVolume)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return;
		}
		mChannel[ch]->mLVolume = aLVolume;
		mChannel[ch]->mRVolume = aRVolume;
		if (unlockMutex) unlockMutex();
	}

	void Soloud::setVolume(int aChannel, float aVolume)
	{
		if (lockMutex) lockMutex();
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			if (unlockMutex) unlockMutex();
			return;
		}
		mChannel[ch]->mVolume = aVolume;
		if (unlockMutex) unlockMutex();
	}

	void Soloud::stop(int aChannel)
	{
		int ch = getAbsoluteChannelFromHandle(aChannel);
		if (ch == -1) 
		{
			return;
		}
		if (lockMutex) lockMutex();
		stopAbsolute(ch);
		if (unlockMutex) unlockMutex();
	}

	void Soloud::stopAbsolute(int aAbsoluteChannel)
	{
		if (mChannel[aAbsoluteChannel])
		{
			delete mChannel[aAbsoluteChannel];
			mChannel[aAbsoluteChannel] = 0;			
		}
	}		

	void Soloud::stopAll()
	{
		int i;
		if (lockMutex) lockMutex();
		for (i = 0; i < mChannelCount; i++)
		{
			stopAbsolute(i);
		}
		if (unlockMutex) unlockMutex();
	}

	void Soloud::mix(float *aBuffer, int aSamples)
	{
		float buffertime = aSamples / (float)mSamplerate;

		if (lockMutex) lockMutex();
		if (mScratchSize < mScratchNeeded)
		{
			mScratchSize = mScratchNeeded;
			delete[] mScratch;
			mScratch = new float[mScratchSize];
		}

		int i;

		// Clear accumulation buffer
		for (i = 0; i < aSamples*2; i++)
		{
			aBuffer[i] = 0;
		}

		// Accumulate sound sources
		for (i = 0; i < mChannelCount; i++)
		{
			if (mChannel[i] && !(mChannel[i]->mFlags & AudioProducer::PAUSED))
			{
				float lpan = mChannel[i]->mLVolume * mChannel[i]->mVolume * mGlobalVolume;
				float rpan = mChannel[i]->mRVolume * mChannel[i]->mVolume * mGlobalVolume;

				float stepratio = mChannel[i]->mSamplerate / mSamplerate;
				mChannel[i]->getAudio(mScratch, (int)ceil(aSamples * stepratio));		
				mChannel[i]->mStreamTime += buffertime;

				int j;
				float step = 0;
				if (mChannel[i]->mFlags & AudioProducer::STEREO)
				{
					for (j = 0; j < aSamples; j++, step += stepratio)
					{
						float s1 = mScratch[(int)floor(step)*2];
						float s2 = mScratch[(int)floor(step)*2+1];
						aBuffer[j * 2 + 0] += s1 * lpan;
						aBuffer[j * 2 + 1] += s2 * rpan;
					}
				}
				else
				{
					for (j = 0; j < aSamples; j++, step += stepratio)
					{
						float s = mScratch[(int)floor(step)];
						aBuffer[j * 2 + 0] += s * lpan;
						aBuffer[j * 2 + 1] += s * rpan;
					}
				}

				// chear channel if the sound is over
				if (!(mChannel[i]->mFlags & AudioProducer::LOOPING) && mChannel[i]->hasEnded())
				{
					stopAbsolute(i);
				}
			}
		}

		if (unlockMutex) unlockMutex();

		// Clip
		if (mFlags & CLIP_ROUNDOFF)
		{
			for (i = 0; i < aSamples*2; i++)
			{
				float f = aBuffer[i];
				if (f <= -1.65f)
				{
					f = -0.9862875f;
				}
				else
				if (f >= 1.65f)
				{
					f = 0.9862875f;
				}
				else
				{
					f =  0.87f * f - 0.1f * f * f * f;
				}
				aBuffer[i] = f * mPostClipScaler;
			}
		}
		else
		{
			for (i = 0; i < aSamples; i++)
			{
				float f = aBuffer[i];
				if (f < -1.0f)
				{
					f = -1.0f;
				}
				else
				if (f > 1.0f)
				{
					f = 1.0f;
				}
				aBuffer[i] = f * mPostClipScaler;
			}
		}
	}

};