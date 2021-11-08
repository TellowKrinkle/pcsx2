/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"
#include "GSDrawScanlineCodeGenerator.h"
#include "GSDrawScanlineCodeGenerator.all.h"
#include "GSVertexSW.h"
#include <map>
#include <mutex>

static std::map<uint64, bool> s_use_new_renderer;
static std::mutex s_use_new_renderer_mutex;
static constexpr const char* s_newrenderer_fname = "/tmp/PCSX2UseNewRenderer.txt";

[[clang::optnone]]
static bool shouldUseNewRenderer(uint64 key)
{
	std::lock_guard<std::mutex> l(s_use_new_renderer_mutex);

	if (s_use_new_renderer.empty())
	{
		FILE* file = fopen(s_newrenderer_fname, "r");
		if (file)
		{
			char* str = nullptr;
			size_t linecap = 0;
			while (getline(&str, &linecap, file) >= 0)
			{
				uint64 key;
				char yn;
				if (sscanf(str, "%llx %c", &key, &yn) == 2)
				{
					if (yn != 'Y' && yn != 'N' && yn != 'y' && yn != 'n')
						fprintf(stderr, "Failed to parse %s: Not y/n\n", str);
					s_use_new_renderer[key] = (yn == 'Y' || yn == 'y') ? true : false;
				}
				else
				{
					fprintf(stderr, "Failed to process line %s\n", str);
				}
			}
			if (str)
				free(str);
			fclose(file);
		}
	}

	auto idx = s_use_new_renderer.find(key);
	if (idx == s_use_new_renderer.end())
	{
		s_use_new_renderer[key] = false;
		// Rewrite file
		FILE* file = fopen(s_newrenderer_fname, "w");
		if (file)
		{
			for (const auto& pair : s_use_new_renderer)
			{
				fprintf(file, "%016llX %c %s\n", pair.first, pair.second ? 'Y' : 'N', GSScanlineSelector(pair.first).to_string().c_str());
			}
			fclose(file);
		}
		return false;
	}

	return idx->second;
}

void GSDSDrawScanline(int pixels, int left, int top, const GSVertexSW& scan, const GSScanlineGlobalData& m_global, GSScanlineLocalData& m_local);

[[clang::optnone]]
GSDrawScanlineCodeGenerator::GSDrawScanlineCodeGenerator(void* param, uint64 key, void* code, size_t maxsize)
	: GSCodeGenerator(code, maxsize)
	, m_local(*(GSScanlineLocalData*)param)
	, m_rip(false)
{
	m_sel.key = key;

	if (m_sel.breakpoint)
		db(0xCC);

	if (key == 0x0055929F112E8374)
	{
//		m_sel.abe = 0;
//		m_sel.ztest = 0;
//		m_sel.zwrite = 0;
//		m_sel.zb = 0;
//		m_sel.mmin = 0;
//		m_sel.ltf = 0;
//		m_sel.atst = ATST_ALWAYS;
	}

	if (key == 0x0055929F112E8374)
	{
		auto before = size_;
		GSDrawScanlineCodeGenerator2(this, CPUInfo(m_cpu), (void*)&m_local, m_sel.key).Generate();
		auto after = size_;
	}
	else
	{
		push(rbp);
		mov(rbp, rsp);
		push(rbx);
		push(r15);
		mov(r9, (size_t)&m_local);
		mov(r15, ptr[r9 + offsetof(GSScanlineLocalData, gd)]);
		mov(r8, r15);
		auto psel = ptr[r15 + offsetof(GSScanlineGlobalData, sel)];
		mov(rbx, psel);
		mov(rax, m_sel.key);
		mov(psel, rax);
		mov(rax, (size_t)GSDSDrawScanline);
		call(rax);
		mov(psel, rbx);
		pop(r15);
		pop(rbx);
		pop(rbp);
		ret();
	}
}
