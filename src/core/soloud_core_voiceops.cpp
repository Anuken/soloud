/*
SoLoud audio engine
Copyright (c) 2013-2015 Jari Komppa

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

#include <string.h>

#include "soloud.h"

// Direct voice operations (no mutexes - called from other functions)

namespace SoLoud {
result Soloud::setVoiceRelativePlaySpeed_internal(unsigned int aVoice, float aSpeed){
    SOLOUD_ASSERT(aVoice < VOICE_COUNT);
    SOLOUD_ASSERT(mInsideAudioThreadMutex);
    if(aSpeed <= 0.0f){
        return INVALID_PARAMETER;
    }

    if(mVoice[aVoice]){
        mVoice[aVoice]->mSetRelativePlaySpeed = aSpeed;
        updateVoiceRelativePlaySpeed_internal(aVoice);
    }

    return 0;
}

void Soloud::setVoicePause_internal(unsigned int aVoice, int aPause){
    SOLOUD_ASSERT(aVoice < VOICE_COUNT);
    SOLOUD_ASSERT(mInsideAudioThreadMutex);
    mActiveVoiceDirty = true;
    if(mVoice[aVoice]){
        mVoice[aVoice]->mPauseScheduler.mActive = 0;

        if(aPause){
            mVoice[aVoice]->mFlags |= AudioSourceInstance::PAUSED;
        }else{
            mVoice[aVoice]->mFlags &= ~AudioSourceInstance::PAUSED;
        }
    }
}

void Soloud::setVoicePan_internal(unsigned int aVoice, float aPan){
    SOLOUD_ASSERT(aVoice < VOICE_COUNT);
    SOLOUD_ASSERT(mInsideAudioThreadMutex);
    if(mVoice[aVoice]){
        mVoice[aVoice]->mPan = aPan;
        float l = (float)cos((aPan + 1) * M_PI / 4);
        float r = (float)sin((aPan + 1) * M_PI / 4);
        mVoice[aVoice]->mChannelVolume[0] = l;
        mVoice[aVoice]->mChannelVolume[1] = r;
        if(mVoice[aVoice]->mChannels == 4){
            mVoice[aVoice]->mChannelVolume[2] = l;
            mVoice[aVoice]->mChannelVolume[3] = r;
        }
        if(mVoice[aVoice]->mChannels == 6){
            mVoice[aVoice]->mChannelVolume[2] = 1.0f / (float)sqrt(2.0f);
            mVoice[aVoice]->mChannelVolume[3] = 1;
            mVoice[aVoice]->mChannelVolume[4] = l;
            mVoice[aVoice]->mChannelVolume[5] = r;
        }
        if(mVoice[aVoice]->mChannels == 8){
            mVoice[aVoice]->mChannelVolume[2] = 1.0f / (float)sqrt(2.0f);
            mVoice[aVoice]->mChannelVolume[3] = 1;
            mVoice[aVoice]->mChannelVolume[4] = l;
            mVoice[aVoice]->mChannelVolume[5] = r;
            mVoice[aVoice]->mChannelVolume[6] = l;
            mVoice[aVoice]->mChannelVolume[7] = r;
        }
    }
}

void Soloud::setVoiceVolume_internal(unsigned int aVoice, float aVolume){
    SOLOUD_ASSERT(aVoice < VOICE_COUNT);
    SOLOUD_ASSERT(mInsideAudioThreadMutex);
    mActiveVoiceDirty = true;
    if(mVoice[aVoice]){
        mVoice[aVoice]->mSetVolume = aVolume;
        updateVoiceVolume_internal(aVoice);
    }
}

void Soloud::stopVoice_internal(unsigned int aVoice){
    // just ignore invalid voices
    if(aVoice >= VOICE_COUNT)
        return;
    SOLOUD_ASSERT(mInsideAudioThreadMutex);
    mActiveVoiceDirty = true;
    if(mVoice[aVoice]){
        // Delete via temporary variable to avoid recursion
        AudioSourceInstance* v = mVoice[aVoice];
        mVoice[aVoice] = 0;

        unsigned int i;
        for(i = 0; i < mMaxActiveVoices; i++){
            if(mResampleDataOwner[i] == v){
                mResampleDataOwner[i] = NULL;
            }
        }

        delete v;
    }
}

void Soloud::fadeStopVoice_internal(unsigned int aVoice){
    if(aVoice >= VOICE_COUNT)
        return;
    SOLOUD_ASSERT(mInsideAudioThreadMutex);
    AudioSourceInstance *v = mVoice[aVoice];
    if(!v)
        return;

    int slot = -1;
    // voices that have not been mixed yet or are silent can be cut off without a click
    if(v->mResampleData[0] && mFadeResampleBuffer.mData && !(v->mFlags & (AudioSourceInstance::PAUSED | AudioSourceInstance::INAUDIBLE))){
        for(int i = 0; i < FADE_VOICE_COUNT; i++){
            if(mFadeVoice[i] == NULL){
                slot = i;
                break;
            }
        }
    }

    if(slot == -1){
        stopVoice_internal(aVoice);
        return;
    }

    mActiveVoiceDirty = true;
    mVoice[aVoice] = 0;

    // the resample buffers belong to the active voice pool and will be reassigned, so move the data into the fade pool
    float *data = mFadeResampleBuffer.mData + slot * 2 * SAMPLE_GRANULARITY * MAX_CHANNELS;
    for(int k = 0; k < 2; k++){
        memcpy(data + k * SAMPLE_GRANULARITY * MAX_CHANNELS, v->mResampleData[k], sizeof(float) * SAMPLE_GRANULARITY * v->mChannels);
        v->mResampleData[k] = data + k * SAMPLE_GRANULARITY * MAX_CHANNELS;
    }

    for(unsigned int i = 0; i < mMaxActiveVoices; i++){
        if(mResampleDataOwner[i] == v){
            mResampleDataOwner[i] = NULL;
        }
    }

    // panAndExpand ramps from the current volume to this over the next block
    v->mOverallVolume = 0;
    mFadeVoice[slot] = v;
}

void Soloud::stopFadeVoices_internal(unsigned int aAudioSourceID, unsigned int aBusHandle){
    SOLOUD_ASSERT(mInsideAudioThreadMutex);
    for(int i = 0; i < FADE_VOICE_COUNT; i++){
        AudioSourceInstance *v = mFadeVoice[i];
        if(v && ((aAudioSourceID != 0 && v->mAudioSourceID == aAudioSourceID) || (aBusHandle != 0 && v->mBusHandle == aBusHandle))){
            mFadeVoice[i] = NULL;
            delete v;
        }
    }
}

void Soloud::updateVoiceRelativePlaySpeed_internal(unsigned int aVoice){
    SOLOUD_ASSERT(aVoice < VOICE_COUNT);
    SOLOUD_ASSERT(mInsideAudioThreadMutex);
    mVoice[aVoice]->mOverallRelativePlaySpeed = m3dData[aVoice].mDopplerValue * mVoice[aVoice]->mSetRelativePlaySpeed;
    mVoice[aVoice]->mSamplerate = mVoice[aVoice]->mBaseSamplerate * mVoice[aVoice]->mOverallRelativePlaySpeed;
}

void Soloud::updateVoiceVolume_internal(unsigned int aVoice){
    SOLOUD_ASSERT(aVoice < VOICE_COUNT);
    SOLOUD_ASSERT(mInsideAudioThreadMutex);
    mVoice[aVoice]->mOverallVolume = mVoice[aVoice]->mSetVolume * m3dData[aVoice].m3dVolume;
    if(mVoice[aVoice]->mFlags & AudioSourceInstance::PAUSED){
        int i;
        for(i = 0; i < MAX_CHANNELS; i++){
            mVoice[aVoice]->mCurrentChannelVolume[i] = mVoice[aVoice]->mChannelVolume[i] * mVoice[aVoice]->mOverallVolume;
        }
    }
}
} // namespace SoLoud
