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

#include "soloud_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <new>

#if !SOLOUD_OGG_ONLY
#include "dr_mp3.h"
#include "dr_wav.h"
#endif
#include "soloud.h"
#include "soloud_file.h"
#include "stb_vorbis.h"

namespace SoLoud {
WavInstance::WavInstance(Wav *aParent){
    mParent = aParent;
    mOffset = 0;
}

unsigned int WavInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize){
    if(mParent->mData == NULL)
        return 0;

    unsigned int dataleft = mParent->mSampleCount - mOffset;
    unsigned int copylen = dataleft;
    if(copylen > aSamplesToRead)
        copylen = aSamplesToRead;

    unsigned int i;
    for(i = 0; i < mChannels; i++){
        memcpy(aBuffer + i * aBufferSize, mParent->mData + mOffset + i * mParent->mSampleCount, sizeof(float) * copylen);
    }

    mOffset += copylen;
    return copylen;
}

result WavInstance::rewind(){
    mOffset = 0;
    mStreamPosition = 0.0f;
    return 0;
}

bool WavInstance::hasEnded(){
    if(!(mFlags & AudioSourceInstance::LOOPING) && mOffset >= mParent->mSampleCount){
        return 1;
    }
    return 0;
}

Wav::Wav(){
    mData = NULL;
    mSampleCount = 0;
}

Wav::~Wav(){
    stop();
    delete[] mData;
}

#define MAKEDWORD(a, b, c, d) (((d) << 24) | ((c) << 16) | ((b) << 8) | (a))

#if !SOLOUD_OGG_ONLY
result Wav::loadwav(MemoryFile *aReader){
    drwav decoder;

    if(!drwav_init_memory(&decoder, aReader->getMemPtr(), aReader->length(), NULL)){
        return FILE_LOAD_FAILED;
    }

    drwav_uint64 samples = decoder.totalPCMFrameCount;

    if(!samples){
        drwav_uninit(&decoder);
        return FILE_LOAD_FAILED;
    }

    mData = new float[(unsigned int)(samples * decoder.channels)];
    mBaseSamplerate = (float)decoder.sampleRate;
    mSampleCount = (unsigned int)samples;
    mChannels = decoder.channels;

    unsigned int i, j, k;
    for(i = 0; i < mSampleCount; i += 512){
        float tmp[512 * MAX_CHANNELS];
        unsigned int blockSize = (mSampleCount - i) > 512 ? 512 : mSampleCount - i;
        drwav_read_pcm_frames_f32(&decoder, blockSize, tmp);
        for(j = 0; j < blockSize; j++){
            for(k = 0; k < decoder.channels; k++){
                mData[k * mSampleCount + i + j] = tmp[j * decoder.channels + k];
            }
        }
    }
    drwav_uninit(&decoder);

    return SO_NO_ERROR;
}
#endif

// Hard ceiling on the decoded size of one ogg file, in bytes of float PCM (512MB is ~25 minutes of stereo audio at 44.1kHz
// Override with -DSOLOUD_OGG_MAX_DECODE_BYTES=...
#ifndef SOLOUD_OGG_MAX_DECODE_BYTES
#define SOLOUD_OGG_MAX_DECODE_BYTES (512u * 1024u * 1024u)
#endif

result Wav::loadogg(MemoryFile *aReader){
    // stb_vorbis takes an int length
    if(aReader->length() > (unsigned int)INT_MAX){
        return FILE_LOAD_FAILED;
    }

    int e = 0;
    stb_vorbis *vorbis = stb_vorbis_open_memory(aReader->getMemPtr(), (int)aReader->length(), &e, 0);

    if(0 == vorbis){
        return FILE_LOAD_FAILED;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    const float samplerate = (float)info.sample_rate;
    const unsigned int channels = (unsigned int)(info.channels > MAX_CHANNELS ? MAX_CHANNELS : info.channels);
    if(channels == 0){
        stb_vorbis_close(vorbis);
        return FILE_LOAD_FAILED;
    }

    unsigned long long maxFrames = (unsigned long long)SOLOUD_OGG_MAX_DECODE_BYTES / (sizeof(float) * channels);
    if(maxFrames > 0xFFFFFFFFull) // mSampleCount is an unsigned int
        maxFrames = 0xFFFFFFFFull;
    if(maxFrames * channels * sizeof(float) > (unsigned long long)(size_t)-1) // keep the byte count representable (32-bit builds)
        maxFrames = (unsigned long long)(size_t)-1 / (channels * sizeof(float));

    // Start with the length from the header so ordinary files decode with one allocation and no copying, but never trust it beyond the ceiling. 
    // If it turns out to be too small we grow below, and if it is too large we repack to the exact decoded size at the end.
    unsigned long long capacity = stb_vorbis_stream_length_in_samples(vorbis);
    if(capacity > maxFrames)
        capacity = maxFrames;
    if(capacity == 0)
        capacity = 4096;

    float *data = new(std::nothrow) float[(size_t)(capacity * channels)];
    result error = SO_NO_ERROR;
    if(data == NULL){
        error = OUT_OF_MEMORY;
    }

    unsigned long long frames = 0;
    while(error == SO_NO_ERROR){
        float **outputs = NULL;
        int n = stb_vorbis_get_frame_float(vorbis, NULL, &outputs);
        if(n <= 0 || outputs == NULL){
            break;
        }

        if(frames + (unsigned long long)n > capacity){
            // The decoded length exceeds what the header promised; grow, with headroom.
            unsigned long long newCapacity = capacity * 2;
            if(newCapacity < frames + (unsigned long long)n)
                newCapacity = frames + (unsigned long long)n;
            if(newCapacity > maxFrames)
                newCapacity = maxFrames;
            if(frames + (unsigned long long)n > newCapacity){
                error = FILE_LOAD_FAILED; // over the ceiling
                break;
            }
            float *grown = new(std::nothrow) float[(size_t)(newCapacity * channels)];
            if(grown == NULL){
                error = OUT_OF_MEMORY;
                break;
            }
            unsigned int ch;
            for(ch = 0; ch < channels; ch++)
                memcpy(grown + (size_t)(ch * newCapacity), data + (size_t)(ch * capacity), sizeof(float) * (size_t)frames);
            delete[] data;
            data = grown;
            capacity = newCapacity;
        }

        unsigned int ch;
        for(ch = 0; ch < channels; ch++)
            memcpy(data + (size_t)(ch * capacity + frames), outputs[ch], sizeof(float) * (size_t)n);
        frames += (unsigned long long)n;
    }

    stb_vorbis_close(vorbis);

    if(error == SO_NO_ERROR && frames == 0)
        error = FILE_LOAD_FAILED;
    if(error != SO_NO_ERROR){
        delete[] data;
        return error;
    }

    // WavInstance::getAudio assumes the per-channel stride equals mSampleCount, and the buffer must not expose unwritten memory, so repack to the exact decoded size if capacity differs.
    if(frames != capacity){
        float *exact = new(std::nothrow) float[(size_t)(frames * channels)];
        if(exact == NULL){
            delete[] data;
            return OUT_OF_MEMORY;
        }
        unsigned int ch;
        for(ch = 0; ch < channels; ch++)
            memcpy(exact + (size_t)(ch * frames), data + (size_t)(ch * capacity), sizeof(float) * (size_t)frames);
        delete[] data;
        data = exact;
    }

    delete[] mData;
    mData = data;
    mSampleCount = (unsigned int)frames;
    mChannels = channels;
    mBaseSamplerate = samplerate;

    return SO_NO_ERROR;
}

#if !SOLOUD_OGG_ONLY
result Wav::loadmp3(MemoryFile *aReader){
    drmp3 decoder;

    if(!drmp3_init_memory(&decoder, aReader->getMemPtr(), aReader->length(), NULL)){
        return FILE_LOAD_FAILED;
    }

    drmp3_uint64 samples = drmp3_get_pcm_frame_count(&decoder);

    if(!samples){
        drmp3_uninit(&decoder);
        return FILE_LOAD_FAILED;
    }

    mData = new float[(unsigned int)(samples * decoder.channels)];
    mBaseSamplerate = (float)decoder.sampleRate;
    mSampleCount = (unsigned int)samples;
    mChannels = decoder.channels;
    drmp3_seek_to_pcm_frame(&decoder, 0);

    unsigned int i, j, k;
    for(i = 0; i < mSampleCount; i += 512){
        float tmp[512 * MAX_CHANNELS];
        unsigned int blockSize = (mSampleCount - i) > 512 ? 512 : mSampleCount - i;
        drmp3_read_pcm_frames_f32(&decoder, blockSize, tmp);
        for(j = 0; j < blockSize; j++){
            for(k = 0; k < decoder.channels; k++){
                mData[k * mSampleCount + i + j] = tmp[j * decoder.channels + k];
            }
        }
    }
    drmp3_uninit(&decoder);

    return SO_NO_ERROR;
}
#endif

result Wav::testAndLoadFile(MemoryFile *aReader){
    delete[] mData;
    mData = 0;
    mSampleCount = 0;
    mChannels = 1;
    int tag = aReader->read32();
    if(tag == MAKEDWORD('O', 'g', 'g', 'S')){
        return loadogg(aReader);
#if !SOLOUD_OGG_ONLY
    }else if(tag == MAKEDWORD('R', 'I', 'F', 'F')){
        return loadwav(aReader);
    }else if(loadmp3(aReader) == SO_NO_ERROR){
        return SO_NO_ERROR;
#endif
    }

    return FILE_LOAD_FAILED;
}

result Wav::load(const char *aFilename){
    if(aFilename == 0)
        return INVALID_PARAMETER;
    stop();
    DiskFile dr;
    int res = dr.open(aFilename);
    if(res == SO_NO_ERROR)
        return loadFile(&dr);
    return res;
}

result Wav::loadMem(const unsigned char *aMem, unsigned int aLength, bool aCopy, bool aTakeOwnership){
    if(aMem == NULL || aLength == 0)
        return INVALID_PARAMETER;
    stop();

    MemoryFile dr;
    dr.openMem(aMem, aLength, aCopy, aTakeOwnership);
    return testAndLoadFile(&dr);
}

result Wav::loadFile(File *aFile){
    if(!aFile)
        return INVALID_PARAMETER;
    stop();

    MemoryFile mr;
    result res = mr.openFileToMem(aFile);

    if(res != SO_NO_ERROR){
        return res;
    }
    return testAndLoadFile(&mr);
}

AudioSourceInstance *Wav::createInstance(){
    return new WavInstance(this);
}

double Wav::getLength(){
    if(mBaseSamplerate == 0)
        return 0;
    return mSampleCount / mBaseSamplerate;
}

result Wav::loadRawWave8(unsigned char *aMem, unsigned int aLength, float aSamplerate, unsigned int aChannels){
    if(aMem == 0 || aLength == 0 || aSamplerate <= 0 || aChannels < 1)
        return INVALID_PARAMETER;
    stop();
    delete[] mData;
    mData = new float[aLength];
    mSampleCount = aLength / aChannels;
    mChannels = aChannels;
    mBaseSamplerate = aSamplerate;
    unsigned int i;
    for(i = 0; i < aLength; i++)
        mData[i] = ((signed)aMem[i] - 128) / (float)0x80;
    return SO_NO_ERROR;
}

result Wav::loadRawWave16(short *aMem, unsigned int aLength, float aSamplerate, unsigned int aChannels){
    if(aMem == 0 || aLength == 0 || aSamplerate <= 0 || aChannels < 1)
        return INVALID_PARAMETER;
    stop();
    delete[] mData;
    mData = new float[aLength];
    mSampleCount = aLength / aChannels;
    mChannels = aChannels;
    mBaseSamplerate = aSamplerate;
    unsigned int i;
    for(i = 0; i < aLength; i++)
        mData[i] = ((signed short)aMem[i]) / (float)0x8000;
    return SO_NO_ERROR;
}

result Wav::loadRawWave(float *aMem, unsigned int aLength, float aSamplerate, unsigned int aChannels, bool aCopy, bool aTakeOwndership){
    if(aMem == 0 || aLength == 0 || aSamplerate <= 0 || aChannels < 1)
        return INVALID_PARAMETER;
    stop();
    delete[] mData;
    if(aCopy == true || aTakeOwndership == false){
        mData = new float[aLength];
        memcpy(mData, aMem, sizeof(float) * aLength);
    }else{
        mData = aMem;
    }
    mSampleCount = aLength / aChannels;
    mChannels = aChannels;
    mBaseSamplerate = aSamplerate;
    return SO_NO_ERROR;
}
}; // namespace SoLoud