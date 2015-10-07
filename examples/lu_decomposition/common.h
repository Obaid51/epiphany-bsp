/*
This file is part of the Epiphany BSP library.

Copyright (C) 2014-2015 Buurlage Wits
Support e-mail: <info@buurlagewits.nl>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License (LGPL)
as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
and the GNU Lesser General Public License along with this program,
see the files COPYING and COPYING.LESSER. If not, see
<http://www.gnu.org/licenses/>.
*/

#define _LOC_M 0x4800
#define _LOC_N 0x4804
#define _LOC_DIM 0x4808
#define _LOC_MATRIX 0x480c

#define _LOC_RS 0x5800
#define _LOC_ARK (_LOC_RS + sizeof(int) * M)
#define _LOC_R (_LOC_ARK + sizeof(float) * M)
#define _LOC_PI (_LOC_R + sizeof(int))
#define _LOC_PI_IN (_LOC_PI + sizeof(int) * entries_per_col)
#define _LOC_ROW_IN (_LOC_PI_IN + sizeof(int) * 2)
#define _LOC_COL_IN (_LOC_ROW_IN + sizeof(float) * dim)
