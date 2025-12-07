/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) 2007-2010  The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "simd_detect.h"

#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

CPU_Features cpu_features = {0};

#ifdef _MSC_VER
static void cpuid(int info[4], int function_id)
{
	__cpuid(info, function_id);
}

static void cpuidex(int info[4], int function_id, int subfunction_id)
{
	__cpuidex(info, function_id, subfunction_id);
}
#else
static void cpuid(int info[4], int function_id)
{
	__cpuid(function_id, info[0], info[1], info[2], info[3]);
}

static void cpuidex(int info[4], int function_id, int subfunction_id)
{
	__cpuid_count(function_id, subfunction_id, info[0], info[1], info[2], info[3]);
}
#endif

void detect_cpu_features(void)
{
	int info[4];

	// Check for CPUID support
	cpuid(info, 0);
	int max_function_id = info[0];

	if (max_function_id >= 1)
	{
		cpuid(info, 1);
		// ECX register
		cpu_features.sse3   = (info[2] & (1 << 0)) != 0;
		cpu_features.ssse3  = (info[2] & (1 << 9)) != 0;
		cpu_features.sse41  = (info[2] & (1 << 19)) != 0;
		cpu_features.sse42  = (info[2] & (1 << 20)) != 0;
		cpu_features.avx    = (info[2] & (1 << 28)) != 0;

		// EDX register
		cpu_features.sse2   = (info[3] & (1 << 26)) != 0;
	}

	if (max_function_id >= 7)
	{
		cpuidex(info, 7, 0);
		// EBX register
		cpu_features.avx2      = (info[1] & (1 << 5)) != 0;
		cpu_features.avx512f   = (info[1] & (1 << 16)) != 0;
		cpu_features.avx512bw  = (info[1] & (1 << 30)) != 0;
	}

	printf("CPU Features: SSE2=%d SSE3=%d SSSE3=%d SSE4.1=%d SSE4.2=%d AVX=%d AVX2=%d AVX512F=%d AVX512BW=%d\n",
	       cpu_features.sse2, cpu_features.sse3, cpu_features.ssse3,
	       cpu_features.sse41, cpu_features.sse42,
	       cpu_features.avx, cpu_features.avx2,
	       cpu_features.avx512f, cpu_features.avx512bw);
}

const char* get_simd_status(void)
{
	static char status[128];

	if (cpu_features.avx512f && cpu_features.avx512bw)
		snprintf(status, sizeof(status), "AVX-512");
	else if (cpu_features.avx2)
		snprintf(status, sizeof(status), "AVX2");
	else if (cpu_features.avx)
		snprintf(status, sizeof(status), "AVX");
	else if (cpu_features.sse42)
		snprintf(status, sizeof(status), "SSE4.2");
	else if (cpu_features.sse2)
		snprintf(status, sizeof(status), "SSE2");
	else
		snprintf(status, sizeof(status), "None");

	return status;
}
