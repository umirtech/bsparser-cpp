// SPDX-FileCopyrightText: 2026 Mohd Umeer
// SPDX-License-Identifier: BSD-2-Clause

#ifndef BSPARSER_H
#define BSPARSER_H

/*
 * ===========================================================================
 * bsparser.h — single public-facing umbrella header
 * ===========================================================================
 *
 * One header for every consumer.  It dynamically adapts to the compilation
 * environment and selects the correct implementation:
 *
 *   Axis 1 — Language
 *       C  (no __cplusplus)                       → C public API
 *       C++                                         → see axis 2
 *
 *   Axis 2 — C++ standard
 *       C++20 or later                             → C++20 template API
 *       (pre-C++20, or MSVC without /Zc:__cplusplus) → C public API
 *
 *   Axis 3 — Build configuration (user override)
 *       #define BS_USE_C_API   or   #define BS_FORCE_C_API
 *           → force the C public API even from C++20.  Use this when you
 *             link the compiled bs_capi STATIC/SHARED library instead of
 *             consuming the header-only templates (ABI stability, no
 *             template instantiation in your TU, or a non-C++20 toolchain).
 *
 * Effectively:
 *
 *   C / pre-C++20 / forced        →  extern "C" API in bs_capi.h
 *                                    (link the bs_capi library)
 *
 *   C++20 (default)               →  header-only templates in bsparser.hpp
 *                                    (nothing to link)
 *
 * The chosen path is reported via BSPARSER_C_API / BSPARSER_CXX_API so
 * callers can assert which ABI they got.
 */

#if defined(BS_FORCE_C_API) || defined(BS_USE_C_API)

#define BSPARSER_USE_C_API 1

#elif !defined(__cplusplus)

#define BSPARSER_USE_C_API 1

#elif (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L) || \
    (defined(__cplusplus) && __cplusplus >= 202002L)

#define BSPARSER_USE_C_API 0

#else

#define BSPARSER_USE_C_API 1

#endif

#if BSPARSER_USE_C_API

#define BSPARSER_C_API 1
#include "bs_capi.h"

#else

#define BSPARSER_CXX_API 1
#include "bsparser.hpp"

#endif

#endif /* BSPARSER_H */
