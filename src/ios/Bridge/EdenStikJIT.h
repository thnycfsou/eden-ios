// SPDX-FileCopyrightText: Copyright 2026 eden-ios contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef EDEN_STIKJIT_H
#define EDEN_STIKJIT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool eden_stikjit_prepare_arena(void);
bool eden_stikjit_arena_ready(void);
bool eden_stikjit_execution_test_passed(void);
bool eden_stikjit_run_execution_test(void);
const char* eden_stikjit_status(void);

#ifdef __cplusplus
}
#endif

#endif // EDEN_STIKJIT_H
