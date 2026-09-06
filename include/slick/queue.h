/********************************************************************************
 * Copyright (c) 2020-2026 Slick Quant LLC
 * All rights reserved
 *
 * This file is part of the SlickQueue. Redistribution and use in source and
 * binary forms, with or without modification, are permitted exclusively under
 * the terms of the MIT license which is available at
 * https://github.com/SlickQuant/slick-queue/blob/main/LICENSE
 *
 ********************************************************************************/

#pragma once

// Backward-compatibility shim. The canonical header is <slick/queue.hpp>.
#pragma once
#ifdef _MSC_VER
#  pragma message("warning: <slick/queue.h> is deprecated; use <slick/queue.hpp>")
#else
#  warning "<slick/queue.h> is deprecated; use <slick/queue.hpp>"
#endif
#include "queue.hpp"