/*
SoLoud audio engine
Copyright (c) 2013-2018 Jari Komppa

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

//ogg-only version of soloud_wavstream
#if SOLOUD_OGG_ONLY

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "soloud.h"
#include "soloud_wavstream.h"
#include "soloud_file.h"
#include "stb_vorbis.h"

/*
//on Android, I need access to the asset manager, but that is only provided through glfm
#ifdef __ANDROID__
#ifndef SOLOUD_NO_GLFM

#define SOLOUD_USE_ANDROID_ASSETS
#include <android/asset_manager.h>
#include <android/log.h>
#include <glfm.h>

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, "soloud-android", __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, "soloud-android", __VA_ARGS__))

#endif
#endif
*/

namespace SoLoud
{

	WavStreamInstance::WavStreamInstance(WavStream *aParent)
	{
		mParent = aParent;
		mOffset = 0;
		mCodec.mOgg = 0;
		mCodec.mFlac = 0;
		mFile = 0;
		if (aParent->mMemFile)
		{
			MemoryFile *mf = new MemoryFile();
			if (mf->openMem(aParent->mMemFile->getMemPtr(), aParent->mMemFile->length(), false, false) == SO_NO_ERROR) {
				mFile = mf;
			}
		}
		else
		if (aParent->mFilename)
		{
			DiskFile *df = new DiskFile;

			if (df->open(aParent->mFilename) == SO_NO_ERROR) {
				mFile = df;
			}
		}
		else
		if (aParent->mStreamFile)
		{
			mFile = aParent->mStreamFile;
			mFile->seek(0); // stb_vorbis assumes file offset to be at start of ogg
		}
		else
		{
			return;
		}
		
		if (mFile)
		{
			if (mParent->mFiletype == WAVSTREAM_OGG)
			{
				int e;

				mCodec.mOgg = stb_vorbis_open_file((Soloud_Filehack *)mFile, 0, &e, 0);

				if (!mCodec.mOgg)
				{
					if (mFile != mParent->mStreamFile)
						delete mFile;
					mFile = 0;
				}
				mOggFrameSize = 0;
				mOggFrameOffset = 0;
				mOggOutputs = 0;
			}
			else
			{
				if (mFile != mParent->mStreamFile)
					delete mFile;
				mFile = NULL;
				return;
			}
		}
	}

	WavStreamInstance::~WavStreamInstance()
	{
		if (mCodec.mOgg)
		{
			stb_vorbis_close(mCodec.mOgg);
		}
		if (mFile != mParent->mStreamFile)
		{
			delete mFile;
		}
	}

	static int getOggData(float **aOggOutputs, float *aBuffer, int aSamples, int aPitch, int aFrameSize, int aFrameOffset, int aChannels)
	{			
		if (aFrameSize <= 0)
			return 0;

		int samples = aSamples;
		if (aFrameSize - aFrameOffset < samples)
		{
			samples = aFrameSize - aFrameOffset;
		}

		int i;
		for (i = 0; i < aChannels; i++)
		{
			memcpy(aBuffer + aPitch * i, aOggOutputs[i] + aFrameOffset, sizeof(float) * samples);
		}
		return samples;
	}

	

	unsigned int WavStreamInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{			
		unsigned int offset = 0;
		if (mFile == NULL)
			return 0;
			
		if (mOggFrameOffset < mOggFrameSize)
		{
			int b = getOggData(mOggOutputs, aBuffer, aSamplesToRead, aBufferSize, mOggFrameSize, mOggFrameOffset, mChannels);
			mOffset += b;
			offset += b;
			mOggFrameOffset += b;
		}

		while (offset < aSamplesToRead)
		{
			mOggFrameSize = stb_vorbis_get_frame_float(mCodec.mOgg, NULL, &mOggOutputs);
			mOggFrameOffset = 0;
			int b = getOggData(mOggOutputs, aBuffer + offset, aSamplesToRead - offset, aBufferSize, mOggFrameSize, mOggFrameOffset, mChannels);
			mOffset += b;
			offset += b;
			mOggFrameOffset += b;

			if (mOffset >= mParent->mSampleCount || b == 0)
			{
				mOffset += offset;
				return offset;
			}
		}
			
		
		return aSamplesToRead;
	}

	result WavStreamInstance::seek(double aSeconds, float* mScratch, unsigned int mScratchSize)
	{
		int pos = (int)floor(mBaseSamplerate * aSeconds);
		stb_vorbis_seek(mCodec.mOgg, pos);
		// Since the position that we just sought to might not be *exactly*
		// the position we asked for, we're re-calculating the position just
		// for the sake of correctness.
		mOffset = stb_vorbis_get_sample_offset(mCodec.mOgg);
		double newPosition = float(mOffset / mBaseSamplerate);
		mStreamPosition = newPosition;
		return 0;
	}

	result WavStreamInstance::rewind()
	{
		if (mCodec.mOgg)
		{
			stb_vorbis_seek_start(mCodec.mOgg);
		}
		mOffset = 0;
		mStreamPosition = 0.0f;
		return 0;
	}

	bool WavStreamInstance::hasEnded()
	{
		if (mOffset >= mParent->mSampleCount)
		{
			return 1;
		}
		return 0;
	}

	WavStream::WavStream()
	{
		mFilename = 0;
		mSampleCount = 0;
		mFiletype = WAVSTREAM_WAV;
		mMemFile = 0;
		mStreamFile = 0;
	}
	
	WavStream::~WavStream()
	{
		stop();
		delete[] mFilename;
		delete mMemFile;
	}
	
#define MAKEDWORD(a,b,c,d) (((d) << 24) | ((c) << 16) | ((b) << 8) | (a))

	result WavStream::loadogg(File * fp)
	{
		fp->seek(0);
		int e;
		stb_vorbis *v;
		v = stb_vorbis_open_file((Soloud_Filehack *)fp, 0, &e, 0);
		if (v == NULL)
			return FILE_LOAD_FAILED;
		stb_vorbis_info info = stb_vorbis_get_info(v);
		mChannels = info.channels;
		if (info.channels > MAX_CHANNELS)
		{
			mChannels = MAX_CHANNELS;
		}
		mBaseSamplerate = (float)info.sample_rate;
		int samples = stb_vorbis_stream_length_in_samples(v);
		stb_vorbis_close(v);
		mFiletype = WAVSTREAM_OGG;

		mSampleCount = samples;

		return 0;
	}

	result WavStream::load(const char *aFilename)
	{
		delete[] mFilename;
		delete mMemFile;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;

		int res;

		//it would be nice if this worked, but it doesn't. crash: ftello: FILE* is NULL.
		/*
		#ifdef SOLOUD_USE_ANDROID_ASSETS

		AndroidFile fp(glfmAndroidGetActivity()->assetManager, aFilename);

		//can be null when not found for some ungodly reason.
		if(!fp.Asset_)
			return FILE_NOT_FOUND;

		#else*/

		DiskFile fp;
		res = fp.open(aFilename);
		if (res != SO_NO_ERROR)
			return res;

		//#endif

		int len = (int)strlen(aFilename);
		mFilename = new char[len+1];		
		memcpy(mFilename, aFilename, len);
		mFilename[len] = 0;
		
		res = parse(&fp);

		if (res != SO_NO_ERROR)
		{
			delete[] mFilename;
			mFilename = 0;
			return res;
		}

		return 0;
	}

	result WavStream::loadMem(const unsigned char *aData, unsigned int aDataLen, bool aCopy, bool aTakeOwnership)
	{
		delete[] mFilename;
		delete mMemFile;
		mStreamFile = 0;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;

		if (aData == NULL || aDataLen == 0)
			return INVALID_PARAMETER;

		MemoryFile *mf = new MemoryFile();
		int res = mf->openMem(aData, aDataLen, aCopy, aTakeOwnership);
		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		res = parse(mf);

		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		mMemFile = mf;

		return 0;
	}

	result WavStream::loadToMem(const char *aFilename)
	{
		DiskFile df;
		int res = df.open(aFilename);
		if (res == SO_NO_ERROR)
		{
			res = loadFileToMem(&df);
		}
		return res;
	}

	result WavStream::loadFile(File *aFile)
	{
		delete[] mFilename;
		delete mMemFile;
		mStreamFile = 0;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;

		int res = parse(aFile);

		if (res != SO_NO_ERROR)
		{
			return res;
		}

		mStreamFile = aFile;

		return 0;
	}

	result WavStream::loadFileToMem(File *aFile)
	{
		delete[] mFilename;
		delete mMemFile;
		mStreamFile = 0;
		mMemFile = 0;
		mFilename = 0;
		mSampleCount = 0;

		MemoryFile *mf = new MemoryFile();
		int res = mf->openFileToMem(aFile);
		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		res = parse(mf);

		if (res != SO_NO_ERROR)
		{
			delete mf;
			return res;
		}

		mMemFile = mf;

		return res;
	}


	result WavStream::parse(File *aFile)
	{
		int tag = aFile->read32();
		int res = SO_NO_ERROR;
		if (tag == MAKEDWORD('O', 'g', 'g', 'S'))
		{
			res = loadogg(aFile);
		}
		else
		{
			res = FILE_LOAD_FAILED;
		}
		return res;
	}

	AudioSourceInstance *WavStream::createInstance()
	{
		return new WavStreamInstance(this);
	}

	double WavStream::getLength()
	{
		if (mBaseSamplerate == 0)
			return 0;
		return mSampleCount / mBaseSamplerate;
	}
};

#endif