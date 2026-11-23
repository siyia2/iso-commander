// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MDF_H
#define MDF_H

// Special thanks to the original author of mdf2iso:

// Salvatore Santagati (mdf2iso).

// Note: The original code has been modernized and ported to C++.

/*  $Id: mdf2iso.c, 22/05/05 

    Copyright (C) 2004,2005 Salvatore Santagati <salvatore.santagati@gmail.com>   

    This program is free software; you can redistribute it and/or modify  
    it under the terms of the GNU General Public License as published by  
    the Free Software Foundation; either version 2 of the License, or     
    (at your option) any later version.                                   

    This program is distributed in the hope that it will be useful,       
    but WITHOUT ANY WARRANTY; without even the implied warranty of        
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         
    GNU General Public License for more details.                          

    You should have received a copy of the GNU General Public License     
    along with this program; if not, write to the                         
    Free Software Foundation, Inc.,                                       
    59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.        
*/

// C++ Standard Library Headers
#include <cstddef>
#include <cstring>
#include <iosfwd>

struct MdfTypeInfo {
    size_t seek_ecc;
    size_t sector_size;
    size_t sector_data;
    size_t seek_head;

    MdfTypeInfo() : seek_ecc(0), sector_size(0), sector_data(0), seek_head(0) {}

    bool determineMdfType(std::ifstream& mdfFile);
};

#endif // MDF_H
