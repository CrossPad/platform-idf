/*
 * Copyright (c) 2026 Marcel Licence
 */

/*
 * ml_smpl_stream_ply.h
 *
 *  Created on: 21.03.2024
 *      Author: Marcel Licence
 */

#ifndef ML_SMPL_STREAM_PLY_H_
#define ML_SMPL_STREAM_PLY_H_


#include <stdint.h>


#define SAMPLE_STREAM_PLY_CH_CNT    17

struct stereo_s
{
    int16_t *chL;
    int16_t *chR;
};

struct channel_s
{
    struct stereo_s stereo_s[SAMPLE_STREAM_PLY_CH_CNT];
};

struct sample_info_from_file
{
    uint16_t numberOfChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t bytesPerSample;
    uint16_t bitsPerSample;
    uint32_t dataLength;
    uint32_t sampleCount;
};


void SampleStreamPly_Process(struct channel_s *channels, uint32_t count);
void SampleStreamPly_Process(int16_t *chL, int16_t *chR, uint32_t count);
bool SampleStreamPlayer_Init(void);
bool SampleStreamPlayer_Init(uint32_t blockSize);
bool SampleStreamPlayer_Init(uint32_t startBlockSize, uint32_t blockSize);
void SampleStreamPlayer_DeInit(void);
bool SampleStreamPlayer_GetInfo(struct sample_info_from_file *ptr, const char *filename);
bool SampleStreamPlayer_SetupSample(uint8_t note, const char *filename);
bool SampleStreamPlayer_SetupSample(uint8_t note, const char *filename, uint8_t group);
bool SampleStreamPlayer_SetupSample(uint8_t note, const char *filename, uint8_t group, uint8_t wavIdx);
bool SampleStreamPlayer_SetupSample(uint8_t note, const char *filename, uint8_t group, uint8_t wavIdx, uint32_t skipSamples, uint32_t lenSamples);
void SampleStreamPlayer_Preview(const char *filename);
void SampleStreamPlayer_Preview(const char *filename, uint32_t start, uint32_t end);
void SampleStreamPlayer_BkgProc(void);
void SampleStreamPlayer_NoteOn(uint8_t note, uint8_t vel);
void SampleStreamPlayer_NoteOn(uint8_t ch, uint8_t note, uint8_t vel);
void SampleStreamPlayer_NoteOff(uint8_t note);
void SampleStreamPlayer_NoteOff(uint8_t ch, uint8_t note);

void SampleStreamPlayer_NoteOnCb(uint8_t note, uint8_t wavIdx);
void SampleStreamPlayer_NoteOffCb(uint8_t note, uint8_t wavIdx);

void SampleStreamPlayer_ErrorMsg(const char *msg);
void *SampleStreamPlayer_MallocFastMemory(uint32_t size);
void *SampleStreamPlayer_MallocSlowMemory(uint32_t size);
void SampleStreamPlayer_Free(void *ptr);
void SampleStreamPlayer_WipeOut(void);
void SampleStreamPlayer_WipeOut(uint8_t note);
uint8_t SampleStreamPlayer_GetFreeWavCnt(void);
void SampleStreamPlayer_SetMaxPolyphony(uint8_t maxPoly);

bool SampleStreamPlayer_GetWaveform(const char *filename, int16_t *buffer, uint32_t count);
bool SampleStreamPlayer_GetWaveform(const char *filename, int16_t *buffer, uint32_t count, uint32_t start, uint32_t end);

void SampleStreamPlayer_StartPlayer(uint8_t param, uint8_t value);
void SampleStreamPlayer_StopPlayer(uint8_t param, uint8_t value);
void SampleStreamPlayer_PausePlayer(uint8_t param, uint8_t value);

void SampleStreamPlayer_SetVolume(uint8_t note, uint8_t volume);
void SampleStreamPlayer_SetPan(uint8_t note, uint8_t pan);

void SampleStreamPlayer_SetLoopEnd(uint8_t note, uint32_t loop_end); /*!< sets the loop end for a certain note */
void SampleStreamPlayer_SetLoopEnd(uint8_t note); /*!< sets loop end to the current position of the selected sample */
void SampleStreamPlayer_SetLoopEnd(void); /*!< sets loop end to the current position of last played sample */
void SampleStreamPlayer_SetLoopEndHalf(void);

/* \brief all loop points will be cleared */
void SampleStreamPlayer_SetLoopClear(uint8_t note);
void SampleStreamPlayer_SetLoopClear(void);

/*
 * \brief   return load of main function: SampleStreamPly_Process
 *          1.0 would be 100% resulting in buffer under-runs
 */
float SampleStreamPly_GetLoadMain(void);

/*
 * \brief   return load of background function: SampleStreamPlayer_BkgProc
 *          1.0 would be 100% resulting in buffer under-runs
 */
float SampleStreamPly_GetLoadBknd(void);

/*
 * \brief   returns relative time between exit and next enter of SampleStreamPly_Process
 *          1.0 would be 100% resulting in buffer under-runs
 */
float SampleStreamPly_GetMaxcMain(void);

/*
 * \brief   returns time in ms between exit and next enter of SampleStreamPlayer_BkgProc
 *          is the time too high it can mean that SampleStreamPlayer_BkgProc was not able to process SD card data within time
 */
float SampleStreamPly_GetMaxcBknd(void);

/*
 * \brief   creates a new file (will overwrite existing files without asking)
 *          the wav header will be written but size, length set to zero
 */
void SampleStreamRecorder_Prepare(const char *filename);

/*
 * \brief   the recorder will be set to active and all processed samples will
 *          be stored to the prepared wav file
 */
void SampleStreamRecorder_Start(void);

/*
 * \brief   the recorder will get a stop signal and flushes all cached samples
 */
void SampleStreamRecorder_Stop(void);

/*
 * \brief   all samples going into this function can be recorder when recorder is active
 */
void SampleStreamRecorder_Process(const int16_t *chL, const int16_t *chR, uint32_t count);


/* File access callbacks */
uint32_t SampleStreamPlayer_FileOpen(const char *filename, const char *mode);
void SampleStreamPlayer_FileClose(uint32_t fileIdx);
uint32_t SampleStreamPlayer_FileRead(uint32_t fileIdx, uint8_t *buffer, uint32_t len);
void SampleStreamPlayer_FileSeek(uint32_t fileIdx, uint32_t pos);


#endif /* ML_SMPL_STREAM_PLY_H_ */
