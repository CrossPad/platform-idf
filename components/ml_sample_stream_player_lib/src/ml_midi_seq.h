/*
 * Copyright (c) 2026 Marcel Licence
 */

/**
 * @file ml_midi_seq.h
 * @brief Header file for the MIDI sequencer module.
 *
 * This module allows recording and playback of time-aligned MIDI events.
 *
 * @date 18.04.2025
 * @author Marcel Licence
 */

#ifndef ML_MIDI_SEQ_H_
#define ML_MIDI_SEQ_H_


/*
 * Includes
 */
#include <stdint.h>
#include <stdbool.h> /**< Needed for 'bool' type */


/*
 * Enums
 */
enum mode_e
{
    none,
    m_playback,
    m_record,
    m_overdub,
    m_overdub_wait,
};

enum state_e
{
    s_idle,
    s_wait_for_sync,
    s_wait_note,
    s_pre_beat,
    s_running,
};


/*
 * Structures
 */
struct midi_seq_event_s
{
    uint32_t timestamp_on;
    uint8_t ch;
    uint8_t note;
    uint8_t vel;
    uint32_t bufferOut;
};

struct midi_seq_info_s
{
    uint32_t pos;
    uint32_t length;
    uint32_t event_count;
};

/*
 * Typedefs
 */
typedef void (*MidiEventCallback)(const struct midi_seq_event_s *event, void *user_data);

/**
 * @brief Sets the memory for the MIDI sequencer.
 * @param ptr Pointer to the memory block to be used by the sequencer.
 */
void ML_MidiSeq_SetMemory(void*ptr);

/**
 * @brief Sets static memory for the MIDI sequencer.
 */
void ML_MidiSeq_SetMemoryStatic(void);

/**
 * @brief Returns the required memory size for the MIDI sequencer.
 */
uint32_t ML_MidiSeq_GetMemorySize(void);

/**
 * @brief Initializes the MIDI sequencer.
 *
 * This function must be called before using other sequencer functions.
 *
 * @param ppqn Pulses per quarter note (timing resolution per beat).
 */
void ML_MidiSeq_Setup(uint32_t ppqn);

/**
 * @brief Call all callbacks with the current data
 */
void ML_MidiSeq_RequestAllInfos(void);

/**
 * @brief Updates the sequencer state.
 *
 * This should be called regularly to process internal timing.
 *
 * @param elapsed_pulses Number of pulses since last call.
 */
void ML_MidiSeq_Loop(uint32_t elapsed_pulses);

/**
 * @brief Notifies the sequencer that a new bar has started.
 *
 * @param param Optional parameter for internal control.
 * @param value Optional value related to the parameter.
 */
void ML_MidiSeq_Overdub(uint8_t param, uint8_t value);

/**
 * @brief Sets the sequencer mode to overdub
 */
void ML_MidiSeq_Overdub(void);

/**
 * @brief Sets the sequencer mode to overdub (but does not record new notes)
 */
void ML_MidiSeq_OverdubWait(void);

/**
 * @brief Clears all stored sequences and recorded data.
 *
 * @param param Optional parameter.
 * @param value Optional value.
 */
void ML_MidiSeq_ClearAll(uint8_t param __attribute__((unused)), uint8_t value);

/**
 * @brief Clears recorded data of the selected sequence.
 *
 * @param param Optional parameter.
 * @param value Shall be above 64 value.
 */
void ML_MidiSeq_ClearSeq(uint8_t param __attribute__((unused)), uint8_t value);

/**
 * @brief This function sets the playback quantization
 *        0 means one note per beat: 4th, 1: 8th, 2: 16th, 3: 32th, and so on
 *
 * @param param unused parameter.
 */
void ML_MidiSeq_SetQ(uint8_t q_factor);

/**
 * @brief This function sets the playback quantization
 *        0..4 means one note per beat: 4th
 *        5..9: 8th
 *        10..14: 16th
 *        15..19: 32th, and so on
 *
 * @param param unused parameter.
 * @param value Value will be divided by 5 to determine the q value
 */
void ML_MidiSeq_SetQ(uint8_t param __attribute__((unused)), uint8_t value);

/**
 * @brief Records a free-form MIDI message.
 *
 * @param data Pointer to MIDI message data.
 * @param len Length of the MIDI message.
 */
void ML_MidiSeq_Msg(uint8_t *data, uint8_t len);

/**
 * @brief Records a Note Off event.
 *
 * @param ch MIDI channel (0-15).
 * @param note Note number (0-127).
 */
void ML_MidiSeq_NoteOff(uint8_t ch, uint8_t note);

/**
 * @brief Records a Note On event.
 *
 * @param ch MIDI channel (0-15).
 * @param note Note number (0-127).
 * @param vel Velocity (0-127).
 */
void ML_MidiSeq_NoteOn(uint8_t ch, uint8_t note, uint8_t vel);

/**
 * @brief Puts the sequencer in record mode (with sync wait).
 *
 * @param param Optional parameter.
 * @param value Optional value.
 */
void ML_MidiSeq_Rec(uint8_t param, uint8_t value);

/**
 * @brief Puts the sequencer in record mode (simple version).
 */
void ML_MidiSeq_Rec(void);

/**
 * @brief Resets the sequencer to the beginning of the sequence.
 *
 * @param param Optional parameter.
 * @param value Optional value.
 */
void ML_MidiSeq_Reset(uint8_t param __attribute__((unused)), uint8_t value);

/**
 * @brief Resets the sequencer (simple version).
 */
void ML_MidiSeq_Reset(void);

/**
 * @brief Selects a sequence for playback.
 *
 * @param param Optional parameter.
 * @param value Optional value.
 */
void ML_MidiSeq_Select(uint8_t param, uint8_t value);

/**
 * @brief Selects a sequence by number.
 *
 * @param seqNumber The sequence index to select.
 */
void ML_MidiSeq_Select(uint8_t seqNumber);

/**
 * @brief Selects a sequence by number.
 *
 * @param seqNumber The sequence index to select.
 * @param enable    to enable or disable a sequence
 */
void ML_MidiSeq_SelectDirect(uint8_t seqNumber, bool enable);

/**
 * @brief Toggles a sequence on/off
 *
 * @param param Optional parameter.
 * @param value Optional value.
 */
void ML_MidiSeq_SelectToggleDirect(uint8_t param, uint8_t value);

/**
 * @brief Toggles a sequence on/off
 *
 * @param seqNumber The sequence index to select.
 */
void ML_MidiSeq_SelectToggleDirect(uint8_t seqNumber);

/**
 * @brief Selects a sequence for record.
 *
 * @param param Optional parameter.
 * @param value Optional value.
 */
void ML_MidiSeq_SelectSeqRecord(uint8_t param, uint8_t value);

/**
 * @brief Selects a sequence by number.
 *
 * @param seqNumber The sequence index to select.
 */
void ML_MidiSeq_SelectSeqRecord(uint8_t seqNumber);

/**
 * @brief Returns the current position of a sequence
 *
 * @param seqId The sequence id number.
 */
uint32_t ML_MidiSeq_GetSeqPos(uint8_t seqId);

/**
 * @brief Sets the length of a new sequence (in bars).
 *
 * The recording will stop automatically after the set number of bars.
 *
 * @param length Number of bars.
 */
void ML_MidiSeq_SetLength(uint8_t length);

/**
 * @brief Sets the length of a new sequence (in bars).
 *
 * This function is used within MIDI maps
 *
 * @param length Number of bars.
 * @param unused > 64 to update the length.
 */
void ML_MidiSeq_SetLenParam(uint8_t length, uint8_t unused);

/**
 * @brief Enables or disables looping playback.
 *
 * @param loop_enabled If true, looping is active. If false, playback stops at the end.
 */
void ML_MidiSeq_SetLoop(bool loop_enabled);

/**
 * @brief Sets the number of bars to wait before starting (sync wait).
 *
 * @param length Bar count to wait before starting playback or recording.
 */
void ML_MidiSeq_SetPreLen(uint8_t length);

/**
 * @brief Starts playback (with optional sync wait).
 *
 * @param param Optional parameter.
 * @param value Optional value.
 */
void ML_MidiSeq_Playback(uint8_t param, uint8_t value);

/**
 * @brief Starts playback (simple version).
 */
void ML_MidiSeq_Playback(void);

/**
 * @brief Stops playback and sets the sequencer to idle.
 *
 * @param param Optional parameter.
 * @param value Optional value.
 */
void ML_MidiSeq_Stop(uint8_t param, uint8_t value);

/**
 * @brief Stops the sequencer (simple version).
 */
void ML_MidiSeq_Stop(void);

/**
 * @brief This functions goes through all events of a sequence and the callback will be called
 *
 * This function should be implemented by the application.
 *
 * @param idx index of the sequence.
 * @param cb pointer to callback function which should be called for each event.
 * @param user_data pointer to user data.
 */
void ML_MidiSeq_ForEachEvent(uint8_t idx, MidiEventCallback cb, void *user_data);

/**
 * @brief This function return information about a certain sequence
 *
 * @param info pointer to a info structure
 * @param sequence_number number of the sequence
 */
void ML_MidiSeq_GetSeqInfo(struct midi_seq_info_s *info, uint8_t sequence_number);
/* === Callback functions === */

/**
 * @brief Called when a Note On event is played back by the sequencer.
 *
 * This function should be implemented by the application.
 *
 * @param ch MIDI channel.
 * @param note MIDI note number.
 * @param vel Velocity.
 */
void ML_MidiSeq_NoteOn_Cb(uint8_t ch, uint8_t note, uint8_t vel);

/**
 * @brief Called when a Note Off event is played back by the sequencer.
 *
 * This function should be implemented by the application.
 *
 * @param ch MIDI channel.
 * @param note MIDI note number.
 */
void ML_MidiSeq_NoteOff_Cb(uint8_t ch, uint8_t note);

/**
 * @brief Return the pointer of a version string
 */
const char *ML_MidiSeq_GetVersionStr(void);

/* optional callbacks */
void ML_MidiSeq_BarChanged(uint8_t bar);
void ML_MidiSeq_CurrentSequenceChanged(uint8_t sequence);
void ML_MidiSeq_ModeChanged(enum mode_e mode);
void ML_MidiSeq_ModeNextChanged(enum mode_e mode);
void ML_MidiSeq_QueuedSequenceChanged(uint8_t sequence);
void ML_MidiSeq_StateChanged(enum state_e state);
void ML_MidiSeq_StateNextChanged(enum state_e state);

void ML_MidiSeq_SeqLengthChanged(uint8_t seq_id, uint32_t length);
void ML_MidiSeq_SeqPosChanged(uint8_t seq_id, uint32_t pos);
void ML_MidiSeq_SeqEventsChanged(uint8_t seq_id, uint32_t count);

void ML_MidiSeq_RecLengthChanged(uint32_t length);
void ML_MidiSeq_RecPosChanged(uint32_t pos);
void ML_MidiSeq_RecEventsChanged(uint32_t count);

void ML_MidiSeq_RecDestChanged(uint32_t seq_id);

void ML_MidiSeq_PreCountChanged(uint32_t count);
void ML_MidiSeq_RecMaxLengthChanged(uint32_t length);

void ML_MidiSeq_MemInfoChanged(uint8_t mem_info, uint32_t mem_size);

void ML_MidiSeq_NoteCountChanged(uint8_t count);


#endif /* ML_MIDI_SEQ_H_ */
