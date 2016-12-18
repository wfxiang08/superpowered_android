#include "SuperpoweredExample.h"
#include <SuperpoweredSimple.h>
#include <SuperpoweredCPU.h>
#include <jni.h>
#include <stdio.h>
#include <android/log.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

static void playerEventCallbackA(void *clientData,
                                 SuperpoweredAdvancedAudioPlayerEvent event, void * __unused value) {
    if (event == SuperpoweredAdvancedAudioPlayerEvent_LoadSuccess) {
    	SuperpoweredAdvancedAudioPlayer *playerA = *((SuperpoweredAdvancedAudioPlayer **)clientData);
        playerA->setBpm(126.0f);
        playerA->setFirstBeatMs(353);
        playerA->setPosition(playerA->firstBeatMs, false, false);
    };
}

static void playerEventCallbackB(void *clientData, SuperpoweredAdvancedAudioPlayerEvent event, void * __unused value) {
    if (event == SuperpoweredAdvancedAudioPlayerEvent_LoadSuccess) {
    	SuperpoweredAdvancedAudioPlayer *playerB = *((SuperpoweredAdvancedAudioPlayer **)clientData);
        playerB->setBpm(123.0f);
        playerB->setFirstBeatMs(40);
        playerB->setPosition(playerB->firstBeatMs, false, false);
    };
}

// 最核心的信号处理流程
static bool audioProcessing(void *clientdata,
                            short int *audioIO, int numberOfSamples,
                            int __unused samplerate) {

	return ((SuperpoweredExample *)clientdata)->process(audioIO, (unsigned int)numberOfSamples);
}

SuperpoweredExample::SuperpoweredExample(unsigned int samplerate, unsigned int buffersize,
                                         const char *path, int fileAoffset, int fileAlength,
                                         int fileBoffset, int fileBlength) :
        activeFx(0), crossValue(0.0f), volB(0.0f), volA(1.0f * headroom) {

    // 1. 分配内存
    stereoBuffer = (float *)memalign(16, (buffersize + 16) * sizeof(float) * 2);

    // 2. 创建两个AudioPlayer
    playerA = new SuperpoweredAdvancedAudioPlayer(&playerA, playerEventCallbackA, samplerate, 0);
    playerA->open(path, fileAoffset, fileAlength);
    playerB = new SuperpoweredAdvancedAudioPlayer(&playerB, playerEventCallbackB, samplerate, 0);
    playerB->open(path, fileBoffset, fileBlength);

    // syncMode的作用?
    playerA->syncMode = playerB->syncMode = SuperpoweredAdvancedAudioPlayerSyncMode_TempoAndBeat;

    // 创建各种组件
    roll = new SuperpoweredRoll(samplerate);
    filter = new SuperpoweredFilter(SuperpoweredFilter_Resonant_Lowpass, samplerate);
    flanger = new SuperpoweredFlanger(samplerate);

    // 整体驱动元素
    audioSystem = new SuperpoweredAndroidAudioIO(samplerate, buffersize,
                                                 false, // enable input
                                                 true,  // enable output
                                                 audioProcessing, this,
                                                 -1, SL_ANDROID_STREAM_MEDIA, buffersize * 2);
}

SuperpoweredExample::~SuperpoweredExample() {
    delete audioSystem;
    delete playerA;
    delete playerB;
    free(stereoBuffer);
}

void SuperpoweredExample::onPlayPause(bool play) {
    if (!play) {
        playerA->pause();
        playerB->pause();
    } else {
        bool masterIsA = (crossValue <= 0.5f);
        playerA->play(!masterIsA);
        playerB->play(masterIsA);
    };

    // 在录制过程中要求CPU全速运行
    SuperpoweredCPU::setSustainedPerformanceMode(play); // <-- Important to prevent audio dropouts.
}

void SuperpoweredExample::onCrossfader(int value) {
    crossValue = float(value) * 0.01f;
    if (crossValue < 0.01f) {
        volA = 1.0f * headroom;
        volB = 0.0f;
    } else if (crossValue > 0.99f) {
        volA = 0.0f;
        volB = 1.0f * headroom;
    } else { // constant power curve
        volA = cosf(float(M_PI_2) * crossValue) * headroom;
        volB = cosf(float(M_PI_2) * (1.0f - crossValue)) * headroom;
    };
}

// 如何选择音效，如何在Native层打印Log
void SuperpoweredExample::onFxSelect(int value) {
	__android_log_print(ANDROID_LOG_VERBOSE, "SuperpoweredExample", "FXSEL %i", value);
	activeFx = (unsigned char)value;
}

// 如何Enable，或者Diable Filter呢?
void SuperpoweredExample::onFxOff() {
    filter->enable(false);
    roll->enable(false);
    flanger->enable(false);
}

#define MINFREQ 60.0f
#define MAXFREQ 20000.0f

static inline float floatToFrequency(float value) {
    if (value > 0.97f) return MAXFREQ;
    if (value < 0.03f) return MINFREQ;
    value = powf(10.0f, (value + ((0.4f - fabsf(value - 0.4f)) * 0.3f)) * log10f(MAXFREQ - MINFREQ)) + MINFREQ;
    return value < MAXFREQ ? value : MAXFREQ;
}

void SuperpoweredExample::onFxValue(int ivalue) {
    float value = float(ivalue) * 0.01f;
    switch (activeFx) {
        case 1:
            filter->setResonantParameters(floatToFrequency(1.0f - value), 0.2f);
            filter->enable(true);
            flanger->enable(false);
            roll->enable(false);
            break;
        case 2:
            if (value > 0.8f) roll->beats = 0.0625f;
            else if (value > 0.6f) roll->beats = 0.125f;
            else if (value > 0.4f) roll->beats = 0.25f;
            else if (value > 0.2f) roll->beats = 0.5f;
            else roll->beats = 1.0f;
            roll->enable(true);
            filter->enable(false);
            flanger->enable(false);
            break;
        default:
            flanger->setWet(value);
            flanger->enable(true);
            filter->enable(false);
            roll->enable(false);
    };
}

// 核心函数
// output似乎是被reset过的，不做任何处理输出为silence
//
bool SuperpoweredExample::process(short int *output, unsigned int numberOfSamples) {

    bool masterIsA = (crossValue <= 0.5f);
    double masterBpm = masterIsA ? playerA->currentBpm : playerB->currentBpm;
    // When playerB needs it, playerA has already stepped this value, so save it now.
    double msElapsedSinceLastBeatA = playerA->msElapsedSinceLastBeat;

    __android_log_print(ANDROID_LOG_VERBOSE, "SuperpoweredExample",
                        "msElapsedSinceLastBeatA %.3f, B: %.3f",
                        msElapsedSinceLastBeatA,
                        playerB->msElapsedSinceLastBeat);

    // PlayerA直接覆盖: stereoBuffer
    bool silence = !playerA->process(stereoBuffer, false, numberOfSamples,
                                     volA, masterBpm,
                                     playerB->msElapsedSinceLastBeat);

    // 如果PlayerA没有输出数据，则直接覆盖stereoBuffer；做一个数据的Merge
    if (playerB->process(stereoBuffer, !silence,
                         numberOfSamples, volB, masterBpm, msElapsedSinceLastBeatA)) {
        silence = false;
    }

    roll->bpm = flanger->bpm = (float)masterBpm; // Syncing fx is one line.

    // 注意每一个Filter的接口
    if (roll->process(silence ? NULL : stereoBuffer, stereoBuffer, numberOfSamples) && silence) silence = false;

    if (!silence) {
        filter->process(stereoBuffer, stereoBuffer, numberOfSamples);
        flanger->process(stereoBuffer, stereoBuffer, numberOfSamples);
    };

    // The stereoBuffer is ready now, let's put the finished audio into the requested buffers.
    if (!silence) {
        SuperpoweredFloatToShortInt(stereoBuffer, output, numberOfSamples);
    }
    return !silence;
}

static SuperpoweredExample *example = NULL;


// 底层的API
extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_SuperpoweredExample(
        JNIEnv *javaEnvironment, jobject __unused obj,
        jint samplerate, jint buffersize,
        jstring apkPath, jint fileAoffset, jint fileAlength, jint fileBoffset, jint fileBlength) {

    const char *path = javaEnvironment->GetStringUTFChars(apkPath, JNI_FALSE);

    // 初始化AudioEngine
    example = new SuperpoweredExample((unsigned int)samplerate, (unsigned int)buffersize, path,
                                      fileAoffset, fileAlength, fileBoffset, fileBlength);
    javaEnvironment->ReleaseStringUTFChars(apkPath, path);
}

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_onPlayPause(
        JNIEnv * __unused javaEnvironment, jobject __unused obj, jboolean play) {
	example->onPlayPause(play);
}

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_onCrossfader(
        JNIEnv * __unused javaEnvironment, jobject __unused obj, jint value) {
	example->onCrossfader(value);
}

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_onFxSelect(
        JNIEnv * __unused javaEnvironment, jobject __unused obj, jint value) {
	example->onFxSelect(value);
}

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_onFxOff(
        JNIEnv * __unused javaEnvironment, jobject __unused obj) {
	example->onFxOff();
}

extern "C" JNIEXPORT void Java_com_superpowered_crossexample_MainActivity_onFxValue(
        JNIEnv * __unused javaEnvironment, jobject __unused obj, jint value) {
	example->onFxValue(value);
}
