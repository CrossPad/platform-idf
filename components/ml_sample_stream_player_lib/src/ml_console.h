/*
 * Copyright (c) 2026 Marcel Licence
 */

/*
 * ml_console.h
 *
 *  Created on: 21.06.2025
 *      Author: PC
 */

#ifndef SRC_ML_CONSOLE_H_
#define SRC_ML_CONSOLE_H_


#include <stdint.h>


struct console_entry_s
{
    const char *cmd;
    void (*cb)(char *argv[], int argc);
};

void Console_Init(void);
void Console_Loop(uint32_t ticks);
void Console_InputChar(char c);
void Console_Cmd(char *argv[], int argc);
void Console_AddCmd(struct console_entry_s *cmdEntry);

void Console_RemovePrompt(void);
void Console_RestorePrompt(void);

void Console_Print(const char *str);
void Console_Printf(const char *format, ...);
void Console_PutChar(char c);
void Console_PutString(const char *str);

#endif /* SRC_ML_CONSOLE_H_ */
