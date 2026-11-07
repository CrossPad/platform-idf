/**
 * @file audio_module.h
 * @brief Declarations of the audio module
 *
 * @date 07.03.2026
 * @author Marcel Licence
 */

#ifndef MAIN_AUDIO_MODULE_H_
#define MAIN_AUDIO_MODULE_H_


bool AudioModule_RT_Setup(void);
void AudioModule_RT_Process(void);
/** RT, while the render is offline (audio_rt_offline()): the clock and the
 *  pads tap only, captured for a freeze; nothing to I2S. */
bool AudioModule_RT_Offline(void);

bool AudioModule_BG_Setup(void);
void AudioModule_BG_Process(void);


#endif /* MAIN_AUDIO_MODULE_H_ */
