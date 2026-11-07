/*
 * Copyright (c) 2026 Marcel Licence
 */

/*
 * ml_smpl_stream_ply_helpers.h
 *
 *  Created on: 18.10.2025
 *      Author: Marcel Licence
 */

#ifndef ML_SMPL_STREAM_PLY_HELPERS_H_
#define ML_SMPL_STREAM_PLY_HELPERS_H_

/*
* Preview a sample from filename, from start to end (in samples)
* with given volume (0-127) and pan (0-127, 64=center)
*/
void SampleStreamPlayer_PreviewSave(char *filename, uint32_t start, uint32_t end, uint8_t vol = 127, uint8_t pan = 64);


#endif /* ML_SMPL_STREAM_PLY_HELPERS_H_ */
