/* AudioSessionOutALSA.cpp
 **
 ** Copyright 2008-2009 Wind River Systems
 ** Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#define LOG_TAG "AudioSessionOutALSA"
#define LOG_NDEBUG 0
#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>

#include <linux/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sys/poll.h>
#include <sys/eventfd.h>
#include <linux/unistd.h>

#include "AudioHardwareALSA.h"

namespace sys_write {
    ssize_t lib_write(int fd, const void *buf, size_t count) {
        return write(fd, buf, count);
    }
};
namespace android_audio_legacy
{
// ----------------------------------------------------------------------------

AudioSessionOutALSA::AudioSessionOutALSA(AudioHardwareALSA *parent,
                                         uint32_t   devices,
                                         int        format,
                                         uint32_t   channels,
                                         uint32_t   samplingRate,
                                         int        sessionId,
                                         status_t   *status)
{
    devices |= AudioSystem::DEVICE_OUT_SPDIF;
    alsa_handle_t alsa_handle;
    char *use_case;
    bool bIsUseCaseSet = false;

    Mutex::Autolock autoLock(mLock);
    // Default initilization
    mParent         = parent;
    mALSADevice     = mParent->mALSADevice;
    mUcMgr          = mParent->mUcMgr;
    mFrameCount     = 0;
    mFormat         = format;
    if(mFormat == AUDIO_FORMAT_AC3 || format == AUDIO_FORMAT_AC3_PLUS) {
        mSampleRate     = 48000;
        mChannels       = 2;
    } else {
        if(samplingRate > 48000) {
            LOGE("Sample rate >48000, opening the driver with 48000Hz");
            mSampleRate     = 48000;
        } else {
            mSampleRate     = samplingRate;
        }
        mChannels       = channels;
    }
    mDevices        = devices;
    mBufferSize     = 0;
    *status         = BAD_VALUE;
    mSessionId      = sessionId;


    mUseTunnelDecode    = false;
    mCaptureFromProxy   = false;
    mRoutePcmAudio      = false;
    mRoutePcmToSpdif    = false;
    mRouteCompreToSpdif = false;
    mRouteAudioToA2dp   = false;
    mA2dpOutputStarted  = false;
    mOpenMS11Decoder    = false;
    mChannelStatusSet   = false;
    mTunnelPaused       = false;
    mTunnelSeeking      = false;
    mReachedExtractorEOS= false;
    mSkipWrite          = false;

    mPcmRxHandle        = NULL;
    mPcmTxHandle        = NULL;
    mSpdifRxHandle      = NULL;
    mCompreRxHandle     = NULL;
    mProxyPcmHandle     = NULL;
    mMS11Decoder        = NULL;
    mBitstreamSM        = NULL;

    mInputBufferSize    = DEFAULT_BUFFER_SIZE;
    mInputBufferCount   = TUNNEL_DECODER_BUFFER_COUNT;
    mEfd = -1;

    mWMAConfigDataSet    = false;
    mAacConfigDataSet    = false; // flags if AAC config to be set(which is sent in first buffer)
    mEventThread         = NULL;
    mEventThreadAlive    = false;
    mKillEventThread     = false;
    mMinBytesReqToDecode = 0;
    mObserver            = NULL;

    if(devices == 0) {
        LOGE("No output device specified");
        return;
    }
    if((format == AUDIO_FORMAT_PCM_16_BIT) && (channels == 0 || channels > 6)) {
        LOGE("Invalid number of channels %d", channels);
        return;
    }

    if(mDevices & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        mCaptureFromProxy = true;
        mRouteAudioToA2dp = true;
        mDevices &= ~AudioSystem::DEVICE_OUT_ALL_A2DP;
        // ToDo: Handle A2DP+Speaker
        //mDevices |= AudioSystem::DEVICE_OUT_PROXY;
        mDevices = AudioSystem::DEVICE_OUT_PROXY;
    }

    if(format == AUDIO_FORMAT_AAC || format == AUDIO_FORMAT_HE_AAC_V1 ||
       format == AUDIO_FORMAT_HE_AAC_V2 || format == AUDIO_FORMAT_AAC_ADIF ||
       format == AUDIO_FORMAT_AC3 || format == AUDIO_FORMAT_AC3_PLUS) {
        // Instantiate MS11 decoder for single decode use case
        int32_t format_ms11;
        mMS11Decoder = new SoftMS11;
        if(mMS11Decoder->initializeMS11FunctionPointers() == false) {
            LOGE("Could not resolve all symbols Required for MS11");
            delete mMS11Decoder;
            return;
        }
        mBitstreamSM = new AudioBitstreamSM;
        if(false == mBitstreamSM->initBitstreamPtr()) {
            LOGE("Unable to allocate Memory for i/p and o/p buffering for MS11");
            delete mMS11Decoder;
            delete mBitstreamSM;
            return;
        }
        if(format == AUDIO_FORMAT_AAC || format == AUDIO_FORMAT_HE_AAC_V1 ||
           format == AUDIO_FORMAT_HE_AAC_V2 || format == AUDIO_FORMAT_AAC_ADIF)
        {
            if(format == AUDIO_FORMAT_AAC_ADIF)
                mMinBytesReqToDecode = AAC_BLOCK_PER_CHANNEL_MS11*mChannels-1;
            else
                mMinBytesReqToDecode = 0;
            format_ms11 = FORMAT_DOLBY_PULSE_MAIN;
        } else {
            format_ms11 = FORMAT_DOLBY_DIGITAL_PLUS_MAIN;
            mMinBytesReqToDecode = 0;
        }
        if(mMS11Decoder->setUseCaseAndOpenStream(format_ms11,channels,samplingRate)) {
            LOGE("SetUseCaseAndOpen MS11 failed");
            delete mMS11Decoder;
            delete mBitstreamSM;
            return;
        }
        mOpenMS11Decoder= true; // indicates if MS11 decoder is instantiated
        mAacConfigDataSet = false; // flags if AAC config to be set(which is sent in first buffer)

        if(mDevices & ~AudioSystem::DEVICE_OUT_SPDIF) {
            mRoutePcmAudio = true;
        }
        if(devices & AudioSystem::DEVICE_OUT_SPDIF) {
//            mRouteCompreToSpdif = true;
//NOTE: The compressed to SPDIF will be enabled when the driver is integrated in HAL.
//      Till then the PCM out of MS11 is routed to both SPDIF and SPEAKER
            mRoutePcmToSpdif = true;
        }
    } else if(format == AUDIO_FORMAT_WMA || format == AUDIO_FORMAT_DTS ||
              format == AUDIO_FORMAT_MP3){
        // In this case, DSP will decode and route the PCM data to output devices
        mUseTunnelDecode = true;
        if(devices & AudioSystem::DEVICE_OUT_PROXY) {
            mCaptureFromProxy = true;
        }
        mWMAConfigDataSet = false;
        createThreadsForTunnelDecode();
    } else if(format == AUDIO_FORMAT_PCM_16_BIT) {
        if(mDevices & ~AudioSystem::DEVICE_OUT_SPDIF) {
            mRoutePcmAudio = true;
        }
        mMinBytesReqToDecode = PCM_BLOCK_PER_CHANNEL_MS11*mChannels-1;
        if(channels > 2 && channels <= 6) {
            // Instantiate MS11 decoder for downmix and re-encode
            mMS11Decoder = new SoftMS11;
            if(mMS11Decoder->initializeMS11FunctionPointers() == false) {
                LOGE("Could not resolve all symbols Required for MS11");
                delete mMS11Decoder;
                return;
            }
            mBitstreamSM = new AudioBitstreamSM;
            if(false == mBitstreamSM->initBitstreamPtr()) {
                LOGE("Unable to allocate Memory for i/p and o/p buffering for MS11");
                delete mBitstreamSM;
                delete mMS11Decoder;
                return;
            }
            if(mMS11Decoder->setUseCaseAndOpenStream(FORMAT_EXTERNAL_PCM,channels,samplingRate)) {
                LOGE("SetUseCaseAndOpen MS11 failed");
                delete mBitstreamSM;
                delete mMS11Decoder;
                return;
            }
            mOpenMS11Decoder=true;

            if(devices & AudioSystem::DEVICE_OUT_SPDIF) {
                mRouteCompreToSpdif = true;
            }
        } else {
            if(devices & AudioSystem::DEVICE_OUT_SPDIF) {
                mRoutePcmToSpdif = true;
            }
        }
    } else {
        LOGE("Unsupported format %d", format);
        return;
    }

    if(mRoutePcmAudio) {
        // If the same audio PCM is to be routed to SPDIF also, do not remove from 
        // device list
        if(!mRoutePcmToSpdif) {
            devices = mDevices & ~AudioSystem::DEVICE_OUT_SPDIF;
        }
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            *status = openDevice(SND_USE_CASE_VERB_HIFI2, true, devices);
        } else {
            *status = openDevice(SND_USE_CASE_MOD_PLAY_MUSIC2, false, devices);
        }
        free(use_case);
        if(*status != NO_ERROR) {
            return;
        }
        ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
        mPcmRxHandle = &(*it);
        mBufferSize = mPcmRxHandle->periodSize;
        if(mRoutePcmToSpdif) {
            mChannelStatusSet = true;
            if(mALSADevice->get_linearpcm_channel_status(samplingRate,mChannelStatus)) {
                LOGE("channel status set error ");
                return;
            }
            mALSADevice->setChannelStatus(mChannelStatus);
        }
    }
    if (mUseTunnelDecode) {
        if (format != AUDIO_FORMAT_WMA)
            *status = openTunnelDevice();
        else
            *status = NO_ERROR;
    } else if (mRouteCompreToSpdif) {
        devices = AudioSystem::DEVICE_OUT_SPDIF;
        if(mCaptureFromProxy) {
            devices |= AudioSystem::DEVICE_OUT_PROXY;
        }
#if 0
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            *status = openDevice(SND_USE_CASE_VERB_HIFI_COMPRESSED, true, devices);
        } else {
        *status = openDevice(SND_USE_CASE_MOD_PLAY_MUSIC_COMPRESSED, false, devices);
        }
        free(use_case);
#endif
        if(*status != NO_ERROR) {
            return;
        }
        ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
        mCompreRxHandle = &(*it);
    }
    if (mCaptureFromProxy) {
        status_t err = openProxyDevice();
        if(!err && mRouteAudioToA2dp) {
            err = openA2dpOutput();
        }
        *status = err;
    }
}

AudioSessionOutALSA::~AudioSessionOutALSA()
{
    Mutex::Autolock autoLock(mLock);
    LOGV("~AudioSessionOutALSA");
    if(mProxyPcmHandle) {
        LOGV("Closing the Proxy device: mProxyPcmHandle %p", mProxyPcmHandle);
        pcm_close(mProxyPcmHandle);
        mProxyPcmHandle = NULL;
    }
    if(mA2dpOutputStarted) {
        stopA2dpOutput();
        closeA2dpOutput();
    }
    if(mOpenMS11Decoder == true) {
        delete mMS11Decoder;
        delete mBitstreamSM;
    }
    if (mUseTunnelDecode)
        requestAndWaitForEventThreadExit();
    if (mCompreRxHandle) {
        LOGV("Closing the Tunnel device: mCompreRxHandle %p", mCompreRxHandle);
        pcm_close(mCompreRxHandle->handle);
        mCompreRxHandle = NULL;
    }

    mSessionId = -1;
}

status_t AudioSessionOutALSA::setParameters(const String8& keyValuePairs)
{
    Mutex::Autolock autoLock(mLock);
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key = String8(AudioParameter::keyRouting);
    int device;
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore routing if device is 0.
        LOGD("setParameters(): keyRouting with device %d", device);
        mDevices = device;
        if(device) {
            //ToDo: Call device setting UCM API here
            doRouting(device);
        }
        param.remove(key);
    }

    return NO_ERROR;
}

String8 AudioSessionOutALSA::getParameters(const String8& keys)
{
    Mutex::Autolock autoLock(mLock);
    AudioParameter param = AudioParameter(keys);
    String8 value;
    String8 key = String8(AudioParameter::keyRouting);

    if (param.get(key, value) == NO_ERROR) {
        param.addInt(key, (int)mDevices);
    }

    LOGV("getParameters() %s", param.toString().string());
    return param.toString();
}

status_t AudioSessionOutALSA::setVolume(float left, float right)
{
    Mutex::Autolock autoLock(mLock);
    float volume;
    status_t status = NO_ERROR;

    volume = (left + right) / 2;
    if (volume < 0.0) {
        LOGW("AudioSessionOutALSA::setVolume(%f) under 0.0, assuming 0.0\n", volume);
        volume = 0.0;
    } else if (volume > 1.0) {
        LOGW("AudioSessionOutALSA::setVolume(%f) over 1.0, assuming 1.0\n", volume);
        volume = 1.0;
    }
   mStreamVol = lrint((volume * 0x2000)+0.5);

    LOGD("Setting stream volume to %d (available range is 0 to 0x2000)\n", mStreamVol);
    LOGE("ToDo: Implement volume setting for broadcast stream");
    if(mPcmRxHandle) {
        if(!strcmp(mPcmRxHandle->useCase, SND_USE_CASE_VERB_HIFI2) ||
                !strcmp(mPcmRxHandle->useCase, SND_USE_CASE_MOD_PLAY_MUSIC2)) {
            LOGD("setPCM 1 Volume(%f)\n", volume);
            LOGD("Setting PCM volume to %d (available range is 0 to 0x2000)\n", mStreamVol);
            status = mPcmRxHandle->module->setPcmVolume(mStreamVol);
        }
        return status;
    }
    else if(mCompreRxHandle) {
        if(!strcmp(mCompreRxHandle->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL) ||
                !strcmp(mCompreRxHandle->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL)) {
            LOGD("set compressed Volume(%f)\n", volume);
            LOGD("Setting Compressed volume to %d (available range is 0 to 0x2000)\n", mStreamVol);
            status = mCompreRxHandle->module->setCompressedVolume(mStreamVol);
        }
        return status;
    }
    return INVALID_OPERATION;
}


status_t AudioSessionOutALSA::openTunnelDevice()
{
    // If audio is to be capture back from proxy device, then route
    // audio to SPDIF and Proxy devices only
    int devices = mDevices;
    char *use_case;
    status_t status = NO_ERROR;;
    if(mCaptureFromProxy) {
        devices = (mDevices & AudioSystem::DEVICE_OUT_SPDIF);
        devices |= AudioSystem::DEVICE_OUT_PROXY;
    }
    mInputBufferSize    = 4800;
    mInputBufferCount   = 512;
    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
        status = openDevice(SND_USE_CASE_VERB_HIFI_TUNNEL, true, devices);
    } else {
        status = openDevice(SND_USE_CASE_MOD_PLAY_TUNNEL, false, devices);
    }
    free(use_case);
    if(status != NO_ERROR) {
        return status;
    }
    ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
    mCompreRxHandle = &(*it);

    //mmap the buffers for playback
    status_t err = mmap_buffer(mCompreRxHandle->handle);
    if(err) {
        LOGE("MMAP buffer failed - playback err = %d", err);
        return err;
    }
    //prepare the driver for playback
    status = pcm_prepare(mCompreRxHandle->handle);
    if (status) {
        LOGE("PCM Prepare failed - playback err = %d", err);
        return status;
    }
    bufferAlloc(mCompreRxHandle);
    mBufferSize = mCompreRxHandle->periodSize;
    if(mRoutePcmToSpdif) {
        status = mALSADevice->setPlaybackFormat("LPCM");
        if(status != NO_ERROR) {
            return status;
        }
        mChannelStatusSet = true;
        if(mALSADevice->get_linearpcm_channel_status(mSampleRate, mChannelStatus)) {
            LOGE("channel status set error ");
            return status;
        }
        mALSADevice->setChannelStatus(mChannelStatus);
    }
    return status;
}

ssize_t AudioSessionOutALSA::write(const void *buffer, size_t bytes)
{
    int period_size;
    char *use_case;

    Mutex::Autolock autoLock(mLock);
    LOGV("write:: buffer %p, bytes %d", buffer, bytes);
    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioSessionOutLock");
        mPowerLock = true;
    }
    snd_pcm_sframes_t n;
    size_t            sent = 0;
    status_t          err;
    if (mUseTunnelDecode && mWMAConfigDataSet == false && mFormat == AUDIO_FORMAT_WMA) {
        LOGV("Configuring the WMA params");
        status_t err = mALSADevice->setWMAParams(mCompreRxHandle, (int *)buffer, bytes/sizeof(int));
        if (err) {
            LOGE("WMA param config failed");
            return -1;
        }
        err = openTunnelDevice();
        if (err) {
            LOGE("opening of tunnel device failed");
            return -1;
        }
        mWMAConfigDataSet = true;
        return bytes;
    }
    if (mUseTunnelDecode && mCompreRxHandle) {
        LOGV("Signal Event thread\n");
        mEventCv.signal();
        mInputMemRequestMutex.lock();
        LOGV("write Empty Queue size() = %d,\
        Filled Queue size() = %d ",\
        mInputMemEmptyQueue.size(),\
        mInputMemFilledQueue.size());
        if (mInputMemEmptyQueue.empty()) {
            LOGV("Write: waiting on mWriteCv");
            mLock.unlock();
            mWriteCv.wait(mInputMemRequestMutex);
            mLock.lock();
            if (mSkipWrite) {
                LOGV("Write: Flushing the previous write buffer");
                mSkipWrite = false;
                mInputMemRequestMutex.unlock();
                return 0;
            }
            LOGV("Write: received a signal to wake up");
        }

        List<BuffersAllocated>::iterator it = mInputMemEmptyQueue.begin();
        BuffersAllocated buf = *it;
        mInputMemEmptyQueue.erase(it);
        mInputMemRequestMutex.unlock();
        memcpy(buf.memBuf, buffer, bytes);
        buf.bytesToWrite = bytes;
        mInputMemResponseMutex.lock();
        mInputMemFilledQueue.push_back(buf);
        mInputMemResponseMutex.unlock();

        LOGV("PCM write start");
        pcm * local_handle = (struct pcm *)mCompreRxHandle->handle;
        pcm_write(local_handle, buf.memBuf, local_handle->period_size);
        if (bytes < local_handle->period_size) {
        LOGD("Last buffer case");
        uint64_t writeValue = SIGNAL_EVENT_THREAD;
        sys_write::lib_write(mEfd, &writeValue, sizeof(uint64_t));

            //TODO : Is this code reqd - start seems to fail?
            if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_START) < 0)
                LOGE("AUDIO Start failed");
            else
                local_handle->start = 1;
        }
        LOGV("PCM write complete");
    } else if(mMS11Decoder != NULL) {
    // 1. Check if MS11 decoder instance is present and if present we need to
    //    preserve the data and supply it to MS 11 decoder.
        // If MS11, the first buffer in AAC format has the AAC config data.

        if(mFormat == AUDIO_FORMAT_AAC || mFormat == AUDIO_FORMAT_HE_AAC_V1 ||
           mFormat == AUDIO_FORMAT_AAC_ADIF || mFormat == AUDIO_FORMAT_HE_AAC_V2) {
            if(mAacConfigDataSet == false) {
                if(mMS11Decoder->setAACConfig((unsigned char *)buffer, bytes) == true);
                    mAacConfigDataSet = true;
                return bytes;
            }
        }
        if(bytes == 0) {
            if(mFormat == AUDIO_FORMAT_AAC_ADIF)
                mBitstreamSM->appendSilenceToBitstreamInternalBuffer(mMinBytesReqToDecode,0);
            else
                return bytes;
        }

        bool    continueDecode=false;
        size_t  bytesConsumedInDecode;
        size_t  copyBytesMS11;
        char    *bufPtr;
        uint32_t outSampleRate=mSampleRate,outChannels=mChannels;
        mBitstreamSM->copyBitsreamToInternalBuffer((char *)buffer, bytes);

        do
        {
            // flag indicating if the decoding has to be continued so as to
            // get all the output held up with MS11. Examples, AAC can have an
            // output frame of 4096 bytes. While the output of MS11 is 1536, the
            // decoder has to be called more than twice to get the reside samples.
            continueDecode=false;
            if(mBitstreamSM->sufficientBitstreamToDecode(mMinBytesReqToDecode) == true)
            {
                bufPtr = mBitstreamSM->getInputBufferPtr();
                copyBytesMS11 = mBitstreamSM->bitStreamBufSize();

                mMS11Decoder->copyBitstreamToMS11InpBuf(bufPtr,copyBytesMS11);
                bytesConsumedInDecode = mMS11Decoder->streamDecode(&outSampleRate,&outChannels);
                bufPtr=mBitstreamSM->getOutputBufferWritePtr(PCM_2CH_OUT);
                copyBytesMS11 = mMS11Decoder->copyOutputFromMS11Buf(PCM_2CH_OUT,bufPtr);
                mBitstreamSM->copyResidueBitstreamToStart(bytesConsumedInDecode);
                // Note: Set the output Buffer to start for for changein sample rate and channel
                // This has to be done.

                mBitstreamSM->setOutputBufferWritePtr(PCM_2CH_OUT,copyBytesMS11);
                // If output samples size is zero, donot continue and wait for next
                // write for decode
                if(copyBytesMS11)
                    continueDecode = true;
            }
            // Close and open the driver again if the output sample rate change is observed
            // in decode.
            if(mPcmRxHandle && mRoutePcmAudio) {
                if( (mSampleRate != outSampleRate) || (mChannels != outChannels)) {
                    uint32_t devices = mDevices;
                    status_t status = closeDevice(mPcmRxHandle);
                    mSampleRate = outSampleRate;
                    mChannels = outChannels;
                    if(!mRoutePcmToSpdif) {
                        devices = mDevices & ~AudioSystem::DEVICE_OUT_SPDIF;
                    }
                    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
                    if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
                        status = openDevice(SND_USE_CASE_VERB_HIFI2, true, devices);
                    } else {
                        status = openDevice(SND_USE_CASE_MOD_PLAY_MUSIC2, false, devices);
                    }
                    free(use_case);
                    if(status != NO_ERROR) {
                        LOGE("Error opening the driver");
                        break;
                    }
                    ALSAHandleList::iterator it = mParent->mDeviceList.end(); it--;
                    mPcmRxHandle = &(*it);
                    mBufferSize = mPcmRxHandle->periodSize;
                    if(mRoutePcmToSpdif) {
                        mChannelStatusSet = true;
                        if(mALSADevice->get_linearpcm_channel_status(mSampleRate,mChannelStatus)) {
                            LOGE("channel status set error ");
                            return -1;
                        }
                        mALSADevice->setChannelStatus(mChannelStatus);
                    }
                }
            }
            if(mPcmRxHandle && mRoutePcmAudio) {
                period_size = mPcmRxHandle->periodSize;
                while(mBitstreamSM->sufficientSamplesToRender(PCM_2CH_OUT,period_size) == true) {
                    n = pcm_write(mPcmRxHandle->handle,
                             mBitstreamSM->getOutputBufferPtr(PCM_2CH_OUT),
                              period_size);
                    LOGE("pcm_write returned with %d", n);
                    if (n == -EBADFD) {
                        // Somehow the stream is in a bad state. The driver probably
                        // has a bug and snd_pcm_recover() doesn't seem to handle this.
                        mPcmRxHandle->module->open(mPcmRxHandle);
                    } else if (n < 0) {
                        // Recovery is part of pcm_write. TODO split is later.
                        LOGE("pcm_write returned n < 0");
                        return static_cast<ssize_t>(n);
                    } else {
                        mFrameCount++;
                        sent += static_cast<ssize_t>((period_size));
                        mBitstreamSM->copyResidueOutputToStart(PCM_2CH_OUT,period_size);
                    }
                }
            }
	} while( (continueDecode == true) && (mBitstreamSM->sufficientBitstreamToDecode(mMinBytesReqToDecode) == true));
    } else {
        // 2. Get the output data from Software decoder and write to PCM driver
        if(bytes == 0)
            return bytes;
        if(mPcmRxHandle && mRoutePcmAudio) {
            int write_pending = bytes;
            period_size = mPcmRxHandle->periodSize;
            do {
                if (write_pending < period_size) {
                    LOGE("write:: We should not be here !!!");
                    write_pending = period_size;
                }
                LOGE("Calling pcm_write");
                n = pcm_write(mPcmRxHandle->handle,
                         (char *)buffer + sent,
                          period_size);
                LOGE("pcm_write returned with %d", n);
                if (n == -EBADFD) {
                    // Somehow the stream is in a bad state. The driver probably
                    // has a bug and snd_pcm_recover() doesn't seem to handle this.
                    mPcmRxHandle->module->open(mPcmRxHandle);
                }
                else if (n < 0) {
                    // Recovery is part of pcm_write. TODO split is later.
                    LOGE("pcm_write returned n < 0");
                    return static_cast<ssize_t>(n);
                }
                else {
                    mFrameCount++;
                    sent += static_cast<ssize_t>((period_size));
                    write_pending -= period_size;
                }
            } while ((mPcmRxHandle->handle) && (sent < bytes));
            if(mRouteAudioToA2dp && !mA2dpOutputStarted) {
                startA2dpOutput();
                mA2dpOutputStarted = true;
            }
        }
    }
    return sent;
}

void AudioSessionOutALSA::bufferAlloc(alsa_handle_t *handle) {
    void  *mem_buf = NULL;
    int i = 0;

    struct pcm * local_handle = (struct pcm *)handle->handle;
    int32_t nSize = local_handle->period_size;
    LOGV("number of input buffers = %d", mInputBufferCount);
    LOGV("memBufferAlloc calling with required size %d", nSize);
    for (i = 0; i < mInputBufferCount; i++) {
        mem_buf = (int32_t *)local_handle->addr + (nSize * i/sizeof(int));
        BuffersAllocated buf(mem_buf, nSize);
        memset(buf.memBuf, 0x0, nSize);
        mInputMemEmptyQueue.push_back(buf);
        mInputBufPool.push_back(buf);
        LOGD("The MEM that is allocated - buffer is %x",\
            (unsigned int)mem_buf);
    }
}

void AudioSessionOutALSA::bufferDeAlloc() {
    //Remove all the buffers from request queue
    while (!mInputBufPool.empty()) {
        List<BuffersAllocated>::iterator it = mInputBufPool.begin();
        BuffersAllocated &memBuffer = *it;
        LOGD("Removing input buffer from Buffer Pool ");
        mInputBufPool.erase(it);
   }
}

void AudioSessionOutALSA::requestAndWaitForEventThreadExit() {

    if (!mEventThreadAlive)
        return;
    mKillEventThread = true;
    if(mEfd != -1) {
        LOGE("Writing to mEfd %d",mEfd);
        uint64_t writeValue = KILL_EVENT_THREAD;
        sys_write::lib_write(mEfd, &writeValue, sizeof(uint64_t));
    }
    mEventCv.signal();
    pthread_join(mEventThread,NULL);
    LOGD("event thread killed");
}

void * AudioSessionOutALSA::eventThreadWrapper(void *me) {
    static_cast<AudioSessionOutALSA *>(me)->eventThreadEntry();
    return NULL;
}

void  AudioSessionOutALSA::eventThreadEntry() {

    int rc = 0;
    int err_poll = 0;
    int avail = 0;
    int i = 0;
    struct pollfd pfd[NUM_FDS];
    struct pcm * local_handle = NULL;
    mEventMutex.lock();
    int timeout = -1;
    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"HAL Audio EventThread", 0, 0, 0);

    LOGV("eventThreadEntry wait for signal \n");
    mEventCv.wait(mEventMutex);
    LOGV("eventThreadEntry ready to work \n");
    mEventMutex.unlock();

    LOGV("Allocating poll fd");
    if(!mKillEventThread && mUseTunnelDecode) {
        LOGV("Allocating poll fd");
        local_handle = (struct pcm *)mCompreRxHandle->handle;
        pfd[0].fd = local_handle->timer_fd;
        pfd[0].events = (POLLIN | POLLERR | POLLNVAL);
        LOGV("Allocated poll fd");
        mEfd = eventfd(0,0);
        pfd[1].fd = mEfd;
        pfd[1].events = (POLLIN | POLLERR | POLLNVAL);
    }
    while(mUseTunnelDecode && !mKillEventThread && ((err_poll = poll(pfd, NUM_FDS, timeout)) >=0)) {
        LOGV("pfd[0].revents =%d ", pfd[0].revents);
        LOGV("pfd[1].revents =%d ", pfd[1].revents);
        if (err_poll == EINTR)
            LOGE("Timer is intrrupted");
        if (pfd[1].revents & POLLIN) {
            uint64_t u;
            read(mEfd, &u, sizeof(uint64_t));
            LOGV("POLLIN event occured on the event fd, value written to %llu",\
                    (unsigned long long)u);
            pfd[1].revents = 0;
            if (u == SIGNAL_EVENT_THREAD) {
                LOGV("### Setting mReachedExtractorEOS");
                mReachedExtractorEOS = true;
                continue;
            }
        }
        if ((pfd[1].revents & POLLERR) || (pfd[1].revents & POLLNVAL)) {
            LOGE("POLLERR or INVALID POLL");
        }
        struct snd_timer_tread rbuf[4];
        read(local_handle->timer_fd, rbuf, sizeof(struct snd_timer_tread) * 4 );
        if((pfd[0].revents & POLLERR) || (pfd[0].revents & POLLNVAL))
            continue;

        if (pfd[0].revents & POLLIN && !mKillEventThread) {
            pfd[0].revents = 0;
            if (mTunnelPaused)
                continue;
            LOGV("After an event occurs");
            {
                if (mInputMemFilledQueue.empty()) {
                    LOGV("Filled queue is empty");
                    continue;
                }
                mInputMemResponseMutex.lock();
                BuffersAllocated buf = *(mInputMemFilledQueue.begin());
                mInputMemFilledQueue.erase(mInputMemFilledQueue.begin());
                LOGV("mInputMemFilledQueue %d", mInputMemFilledQueue.size());
                if (mInputMemFilledQueue.empty() && mReachedExtractorEOS) {
                    LOGV("Posting the EOS to the MPQ player");
                    //post the EOS To MPQ player
                    if (mObserver)
                        mObserver->postEOS(0);
                }
                mInputMemResponseMutex.unlock();

                mInputMemRequestMutex.lock();

                mInputMemEmptyQueue.push_back(buf);

                mInputMemRequestMutex.unlock();
                mWriteCv.signal();
            }
        }
    }
    mEventThreadAlive = false;
    if (mEfd != -1) {
        close(mEfd);
        mEfd = -1;
    }

    LOGD("Event Thread is dying.");
    return;

}

void AudioSessionOutALSA::createThreadsForTunnelDecode() {
    mKillEventThread = false;
    mEventThreadAlive = true;
    LOGD("Creating Event Thread");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&mEventThread, &attr, eventThreadWrapper, this);
    LOGD("Event Thread created");
}

status_t AudioSessionOutALSA::start(int64_t startTime)
{
    Mutex::Autolock autoLock(mLock);
    status_t err = NO_ERROR;
    // 1. Set the absolute time stamp
    // ToDo: We need the ioctl from driver to set the time stamp

    // 2. Signal the driver to start rendering data
    if (ioctl(mPcmRxHandle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        LOGE("start:SNDRV_PCM_IOCTL_START failed\n");
        err = UNKNOWN_ERROR;
    }
    return err;
}

status_t AudioSessionOutALSA::pause()
{
    Mutex::Autolock autoLock(mLock);
    if(mPcmRxHandle) {
        if (ioctl(mPcmRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
            LOGE("PAUSE failed for use case %s", mPcmRxHandle->useCase);
        }
    }
    if(mCompreRxHandle) {
        if (ioctl(mCompreRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
            LOGE("PAUSE failed on use case %s", mCompreRxHandle->useCase);
        }
        mTunnelPaused = true;
    }
    return NO_ERROR;
}

status_t AudioSessionOutALSA::drainTunnel()
{
    status_t err = OK;
    if (!(mCompreRxHandle && mUseTunnelDecode))
        return -1;
    mCompreRxHandle->handle->start = 0;
    err = pcm_prepare(mCompreRxHandle->handle);
    if(err != OK) {
        LOGE("pcm_prepare -seek = %d",err);
        //Posting EOS
        if (mObserver)
            mObserver->postEOS(0);
    }
    LOGV("drainTunnel Empty Queue size() = %d,"
       " Filled Queue size() = %d ",\
        mInputMemEmptyQueue.size(),\
        mInputMemFilledQueue.size());
    LOGV("Reset, drain and prepare completed");
    mCompreRxHandle->handle->sync_ptr->flags =
        SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
    sync_ptr(mCompreRxHandle->handle);
    LOGV("appl_ptr= %d",\
        (int)mCompreRxHandle->handle->sync_ptr->c.control.appl_ptr);
    return err;
}

status_t AudioSessionOutALSA::flush()
{
    Mutex::Autolock autoLock(mLock);
    LOGD("AudioSessionOutALSA::flush E");
    int err = 0;
    if (mUseTunnelDecode && mCompreRxHandle) {
        struct pcm * local_handle = mCompreRxHandle->handle;
        LOGV("Paused case, %d",mTunnelPaused);

        mInputMemResponseMutex.lock();
        mInputMemRequestMutex.lock();
        mInputMemFilledQueue.clear();
        mInputMemEmptyQueue.clear();
        List<BuffersAllocated>::iterator it = mInputBufPool.begin();
        for (;it!=mInputBufPool.end();++it) {
            memset((*it).memBuf, 0x0, (*it).memBufsize);
            mInputMemEmptyQueue.push_back(*it);
        }

        mInputMemRequestMutex.unlock();
        mInputMemResponseMutex.unlock();
        LOGV("Transferred all the buffers from response queue to\
            request queue to handle seek");
        if (!mTunnelPaused) {
            if ((err = ioctl(local_handle->fd, SNDRV_PCM_IOCTL_PAUSE,1)) < 0) {
                LOGE("Audio Pause failed");
                return err;
            }
            mReachedExtractorEOS = false;
            if ((err = drainTunnel()) != OK)
                return err;
        } else {
            mTunnelSeeking = true;
        }
        mSkipWrite = true;
        mWriteCv.signal();
    }
    /*if(mPcmRxHandle) {
        if (ioctl(mPcmRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
            LOGE("flush(): Audio Pause failed");
        }
        mPcmRxHandle->handle->start = 0;
        pcm_prepare(mPcmRxHandle->handle);
        LOGV("flush(): Reset, drain and prepare completed");
        mPcmRxHandle->handle->sync_ptr->flags = (SNDRV_PCM_SYNC_PTR_APPL | 
                                                 SNDRV_PCM_SYNC_PTR_AVAIL_MIN);
        sync_ptr(mPcmRxHandle->handle);
    }*/
    mFrameCount = 0;
    if(mOpenMS11Decoder == true)
        mBitstreamSM->resetBitstreamPtr();
    LOGD("AudioSessionOutALSA::flush X");
    return NO_ERROR;
}

status_t AudioSessionOutALSA::resume()
{
    Mutex::Autolock autoLock(mLock);
    if(mPcmRxHandle) {
        if (ioctl(mPcmRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,0) < 0) {
            LOGE("RESUME failed for use case %s", mPcmRxHandle->useCase);
        }
    }
    if(mCompreRxHandle && mUseTunnelDecode) {
        if (mTunnelSeeking) {
            drainTunnel();
            mTunnelSeeking = false;
        } else if (ioctl(mCompreRxHandle->handle->fd, SNDRV_PCM_IOCTL_PAUSE,0) < 0) {
            LOGE("RESUME failed on use case %s", mCompreRxHandle->useCase);
        }
        mTunnelPaused = false;
    }
    return NO_ERROR;
}

status_t AudioSessionOutALSA::stop()
{
    Mutex::Autolock autoLock(mLock);
    // close all the existing PCM devices
    // ToDo: How to make sure all the data is rendered before closing
    mSkipWrite = true;
    mWriteCv.signal();
    if(mPcmRxHandle)
        closeDevice(mPcmRxHandle);
    if(mCompreRxHandle)
        closeDevice(mCompreRxHandle);
    if(mPcmTxHandle)
        closeDevice(mPcmTxHandle);
    if(mSpdifRxHandle)
        closeDevice(mSpdifRxHandle);

    mRoutePcmAudio = false;
    mRouteCompreAudio = false;
    mRoutePcmToSpdif = false;
    mRouteCompreToSpdif = false;
    mUseTunnelDecode = false;
    mCaptureFromProxy = false;
    mOpenMS11Decoder = false;
    mAacConfigDataSet = false;
    mWMAConfigDataSet = false;

    return NO_ERROR;
}

status_t AudioSessionOutALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioSessionOutALSA::standby()
{
    Mutex::Autolock autoLock(mLock);
    LOGD("standby");
    if(mPcmRxHandle) {
        mPcmRxHandle->module->standby(mPcmRxHandle);
    }

    if (mPowerLock) {
        release_wake_lock ("AudioOutLock");
        mPowerLock = false;
    }

    mFrameCount = 0;
    if(mOpenMS11Decoder == true)
        mBitstreamSM->resetBitstreamPtr();
    return NO_ERROR;
}

#define USEC_TO_MSEC(x) ((x + 999) / 1000)

uint32_t AudioSessionOutALSA::latency() const
{
    // Android wants latency in milliseconds.
    return USEC_TO_MSEC (mPcmRxHandle->latency);
}

status_t AudioSessionOutALSA::setObserver(void *observer)
{
    LOGV("Registering the callback \n");
    mObserver = reinterpret_cast<AudioEventObserver *>(observer);
    return NO_ERROR;
}

status_t AudioSessionOutALSA::getTimeStamp(uint64_t *timeStamp)
{
    LOGV("getTimeStamp \n");

    if (!timeStamp)
        return -1;

    *timeStamp = -1;
    Mutex::Autolock autoLock(mLock);
    if (mCompreRxHandle && mUseTunnelDecode) {
        struct snd_compr_tstamp tstamp;
        tstamp.timestamp = -1;
        if (ioctl(mCompreRxHandle->handle->fd, SNDRV_COMPRESS_TSTAMP, &tstamp)){
            LOGE("Failed SNDRV_COMPRESS_TSTAMP\n");
            return -1;
        } else {
            LOGV("Timestamp returned = %lld\n", tstamp.timestamp);
            *timeStamp = tstamp.timestamp;
            return NO_ERROR;
        }
    }
    return NO_ERROR;
}

// return the number of audio frames written by the audio dsp to DAC since
// the output has exited standby
status_t AudioSessionOutALSA::getRenderPosition(uint32_t *dspFrames)
{
    Mutex::Autolock autoLock(mLock);
    *dspFrames = mFrameCount;
    return NO_ERROR;
}

status_t AudioSessionOutALSA::openA2dpOutput()
{
    hw_module_t *mod;
    int      format = AUDIO_FORMAT_PCM_16_BIT;
    uint32_t channels = AUDIO_CHANNEL_OUT_STEREO;
    uint32_t sampleRate = 44100;
    status_t status;
    LOGV("openA2dpOutput");
    int rc = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, (const char*)"a2dp", 
                                    (const hw_module_t**)&mod);
    if (rc) {
        LOGE("Could not get a2dp hardware module");
        return NO_INIT;
    }

    rc = audio_hw_device_open(mod, &mA2dpDevice);
    if(rc) {
        LOGE("couldn't open a2dp audio hw device");
        return NO_INIT;
    }
    status = mA2dpDevice->open_output_stream(mA2dpDevice, AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP, 
                                    &format, &channels, &sampleRate, &mA2dpStream);
    if(status != NO_ERROR) {
        LOGE("Failed to open output stream for a2dp: status %d", status);
    }
    return status;
}

status_t AudioSessionOutALSA::closeA2dpOutput()
{
    LOGV("closeA2dpOutput");
    if(!mA2dpDevice){
        LOGE("No Aactive A2dp output found");
        return BAD_VALUE;
    }

    mA2dpDevice->close_output_stream(mA2dpDevice, mA2dpStream);
    mA2dpStream = NULL;

    audio_hw_device_close(mA2dpDevice);
    mA2dpDevice = NULL;
    return NO_ERROR;
}

status_t AudioSessionOutALSA::startA2dpOutput()
{
    LOGV("startA2dpOutput");
    int err = pthread_create(&mA2dpThread, (const pthread_attr_t *) NULL,
                             a2dpThreadWrapper,
                             this);

    return err;
}

status_t AudioSessionOutALSA::stopA2dpOutput()
{
    LOGV("stopA2dpOutput");
    mExitA2dpThread = true;
    pthread_join(mA2dpThread,NULL);
    return NO_ERROR;
}

void *AudioSessionOutALSA::a2dpThreadWrapper(void *me) {
    static_cast<AudioSessionOutALSA *>(me)->a2dpThreadFunc();
    return NULL;
}

void AudioSessionOutALSA::a2dpThreadFunc()
{
    if(!mA2dpStream) {
        LOGE("No valid a2dp output stream found");
        return;
    }
    if(!mProxyPcmHandle) {
        LOGE("No valid mProxyPcmHandle found");
        return;
    }

    setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"AudioHAL A2dpThread", 0, 0, 0);

    int a2dpBufSize = mA2dpStream->common.get_buffer_size(&mA2dpStream->common);

    void *a2dpBuffer = malloc(a2dpBufSize);
    if(!a2dpBuffer) {
        LOGE("Could not allocate buffer: a2dpBuffer");
        return;
    }
    int proxyBufSize = mProxyPcmHandle->period_size;
    void *proxyBuffer = malloc(proxyBufSize);
    if(!proxyBuffer) {
        LOGE("Could not allocate buffer: proxyBuffer");
        return;
    }

    while(!mExitA2dpThread) {
        // 1. Read from Proxy device
        int bytesRead = 0;
        while( (a2dpBufSize -  bytesRead) >= proxyBufSize) {
            int err = pcm_read(mProxyPcmHandle, a2dpBuffer + bytesRead, proxyBufSize);
            if(err) {
                LOGE("pcm_read on Proxy port failed with err %d", err);
            } else {
                LOGV("pcm_read on proxy device is success, bytesRead = %d", proxyBufSize);
            }
            bytesRead += proxyBufSize;
        }
        // 2. Buffer the data till the requested buffer size from a2dp output stream
        // 3. write to a2dp output stream
        LOGV("Writing %d bytes to a2dp output", bytesRead);
        mA2dpStream->write(mA2dpStream, a2dpBuffer, bytesRead);
    }
}

status_t AudioSessionOutALSA::openProxyDevice()
{
    char *deviceName = "hw:0,8";
    struct snd_pcm_hw_params *params = NULL;
    struct snd_pcm_sw_params *sparams = NULL;
    int flags = (PCM_IN | PCM_NMMAP | PCM_STEREO | DEBUG_ON);

    LOGV("openProxyDevice");
    mProxyPcmHandle = pcm_open(flags, deviceName);
    if (!pcm_ready(mProxyPcmHandle)) {
        LOGE("Opening proxy device failed");
        goto bail;
    }
    LOGV("Proxy device opened successfully: mProxyPcmHandle %p", mProxyPcmHandle);
    mProxyPcmHandle->channels = 2;
    mProxyPcmHandle->rate     = 48000;
    mProxyPcmHandle->flags    = flags;
    mProxyPcmHandle->period_size = 480;

    params = (struct snd_pcm_hw_params*) malloc(sizeof(struct snd_pcm_hw_params));
    if (!params) {
         goto bail;
    }

    param_init(params);

    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS,
                   (mProxyPcmHandle->flags & PCM_MMAP)? SNDRV_PCM_ACCESS_MMAP_INTERLEAVED
                   : SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
                   SNDRV_PCM_FORMAT_S16_LE);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT,
                   SNDRV_PCM_SUBFORMAT_STD);
    param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, mProxyPcmHandle->period_size);
    param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS,
                   mProxyPcmHandle->channels - 1 ? 32 : 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS,
                   mProxyPcmHandle->channels);
    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, mProxyPcmHandle->rate);

    param_set_hw_refine(mProxyPcmHandle, params);

    if (param_set_hw_params(mProxyPcmHandle, params)) {
        LOGE("Failed to set hardware params on Proxy device");
        param_dump(params);
        goto bail;
    }

    param_dump(params);
    mProxyPcmHandle->buffer_size = pcm_buffer_size(params);
    mProxyPcmHandle->period_size = pcm_period_size(params);
    mProxyPcmHandle->period_cnt  = mProxyPcmHandle->buffer_size/mProxyPcmHandle->period_size;
    sparams = (struct snd_pcm_sw_params*) malloc(sizeof(struct snd_pcm_sw_params));
    if (!sparams) {
        LOGE("Failed to allocated software params for Proxy device");
        goto bail;
    }
   sparams->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
   sparams->period_step = 1;
   sparams->avail_min = (mProxyPcmHandle->flags & PCM_MONO) ?
       mProxyPcmHandle->period_size/2 : mProxyPcmHandle->period_size/4;
   sparams->start_threshold = 1;
   sparams->stop_threshold = (mProxyPcmHandle->flags & PCM_MONO) ?
       mProxyPcmHandle->buffer_size/2 : mProxyPcmHandle->buffer_size/4;
   sparams->xfer_align = (mProxyPcmHandle->flags & PCM_MONO) ?
       mProxyPcmHandle->period_size/2 : mProxyPcmHandle->period_size/4; /* needed for old kernels */
   sparams->silence_size = 0;
   sparams->silence_threshold = 0;

   if (param_set_sw_params(mProxyPcmHandle, sparams)) {
        LOGE("Failed to set software params on Proxy device");
        goto bail;
   }
   if (pcm_prepare(mProxyPcmHandle)) {
       LOGE("Failed to pcm_prepare on Proxy device");
       goto bail;
   }
   if(params) delete params;
   if(sparams) delete sparams;
   return NO_ERROR;

bail:
   if(mProxyPcmHandle) pcm_close(mProxyPcmHandle);
   if(params) delete params;
   if(sparams) delete sparams;
   return NO_INIT;
}

status_t AudioSessionOutALSA::openDevice(char *useCase, bool bIsUseCase, int devices)
{
    alsa_handle_t alsa_handle;
    status_t status = NO_ERROR;
    LOGD("openDevice: E");
    alsa_handle.module      = mALSADevice;
    alsa_handle.bufferSize  = mInputBufferSize;
    alsa_handle.devices     = devices;
    alsa_handle.handle      = 0;
    alsa_handle.format      = (mFormat == AUDIO_FORMAT_PCM_16_BIT ? SNDRV_PCM_FORMAT_S16_LE : mFormat);
    alsa_handle.channels    = mChannels;
    alsa_handle.sampleRate  = mSampleRate;
    alsa_handle.mode        = mParent->mode();
    alsa_handle.latency     = PLAYBACK_LATENCY;
    alsa_handle.rxHandle    = 0;
    alsa_handle.ucMgr       = mUcMgr;
    strlcpy(alsa_handle.useCase, useCase, sizeof(alsa_handle.useCase));
    char *ucmDevice = mALSADevice->getUCMDevice(devices & AudioSystem::DEVICE_OUT_ALL, 0);
    if(bIsUseCase) {
        snd_use_case_set_case(mUcMgr, "_verb", alsa_handle.useCase, ucmDevice);
    } else {
        snd_use_case_set_case(mUcMgr, "_enamod", alsa_handle.useCase, ucmDevice);
    }
    status = mALSADevice->open(&alsa_handle);
    if(status != NO_ERROR) {
        LOGE("Could not open the ALSA device for use case %s", alsa_handle.useCase);
        mALSADevice->close(&alsa_handle);
    } else{
        mParent->mDeviceList.push_back(alsa_handle);
    }
    LOGD("openDevice: X");
    return status;
}

status_t AudioSessionOutALSA::closeDevice(alsa_handle_t *pHandle)
{
    status_t status = NO_ERROR;
    LOGV("closeDevice: useCase %s", pHandle->useCase);
    if(mSessionId != TUNNEL_SESSION_ID) {
        if(pHandle) {
            status = mALSADevice->close(pHandle);
        }
    }
    return status;
}

status_t AudioSessionOutALSA::doRouting(int devices)
{
    status_t status = NO_ERROR;
    char *use_case;
    char *ucmDevice = mALSADevice->getUCMDevice(devices & AudioSystem::DEVICE_OUT_ALL, 0);

    LOGV("doRouting: devices 0x%x", devices);
    mDevices = devices;
    if(mDevices & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        mCaptureFromProxy = true;
        mRouteAudioToA2dp = true;
        mDevices &= ~AudioSystem::DEVICE_OUT_ALL_A2DP;
        //ToDo: Handle A2dp+Speaker
        //mDevices |= AudioSystem::DEVICE_OUT_PROXY;
        mDevices = AudioSystem::DEVICE_OUT_PROXY;
    } else {
        mRouteAudioToA2dp = false;
        mCaptureFromProxy = false;
    }
    if(mPcmRxHandle && devices != mPcmRxHandle->devices) {
        mALSADevice->route(mPcmRxHandle, devices, mParent->mode());
        //snd_use_case_set_case(mUcMgr, "_swdev", mPcmRxHandle->useCase, ucmDevice);
        mPcmRxHandle->devices = devices;
    }
    if(mUseTunnelDecode) {
        mALSADevice->route(mCompreRxHandle, devices, mParent->mode());
        mCompreRxHandle->devices = devices;
    } else if(mRouteCompreToSpdif && !(devices & AudioSystem::DEVICE_OUT_SPDIF)) {
        mRouteCompreToSpdif = false;
        status = closeDevice(mCompreRxHandle);
    } else if(!mRouteCompreToSpdif && (devices & AudioSystem::DEVICE_OUT_SPDIF)) {
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
#if 0
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            status = openDevice(SND_USE_CASE_VERB_HIFI_COMPRESSED, true, AudioSystem::DEVICE_OUT_SPDIF);
        } else {
            status = openDevice(SND_USE_CASE_MOD_PLAY_MUSIC_COMPRESSED, false, AudioSystem::DEVICE_OUT_SPDIF);
        }
#endif
        free(use_case);
        if(status == NO_ERROR) {
            mRouteCompreToSpdif = true;
        }
    }
    if(mCaptureFromProxy) {
        if(!mProxyPcmHandle) {
            status = openProxyDevice();
        }
        if(status == NO_ERROR && mRouteAudioToA2dp && !mA2dpDevice) {
            status = openA2dpOutput();
            if(status == NO_ERROR) {
                status = startA2dpOutput();
            }
        }
    } else {
        if(mProxyPcmHandle) {
            LOGV("Closing the Proxy device");
            pcm_close(mProxyPcmHandle);
            mProxyPcmHandle = NULL;
        }
        if(mA2dpOutputStarted) {
            status = stopA2dpOutput();
            mA2dpOutputStarted = false;
            closeA2dpOutput();
        }
    }
    return status;
}

}       // namespace android_audio_legacy
