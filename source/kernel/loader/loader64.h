/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef __LOADER64_H__
#define __LOADER64_H__

class X64CPU;

bool isElf64X64(const U8* header, U32 len);
BString getInterpreter64(FsOpenNode* openNode);
bool loadProgram64(KThread* thread, FsOpenNode* openNode);
X64CPU* x64ConvertThread(KThread* thread);
void x64SetupMainThread(KThread* thread, const std::vector<BString>& args, std::vector<BString>& env);
void x64SegFault(KThread* thread, U32 address, bool accessError);

#endif
