// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MDF_H
#define MDF_H


// Special thanks to the original authors of mdf2iso:

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


struct MdfTypeInfo {
    size_t seek_ecc;
    size_t sector_size;
    size_t sector_data;
    size_t seek_head;

    MdfTypeInfo() : seek_ecc(0), sector_size(0), sector_data(0), seek_head(0) {}

    bool determineMdfType(std::ifstream& mdfFile) {
        char buf[12];
        mdfFile.seekg(0);
        if (!mdfFile.read(buf, 12)) {
            return false;
        }

        if (std::memcmp("\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, 12) == 0) {
            mdfFile.seekg(2352);
            if (!mdfFile.read(buf, 12)) {
                return false;
            }

            if (std::memcmp("\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, 12) == 0) {
                // Type 1: 2352-byte sectors with 2048-byte user data
                seek_ecc = 288;
                sector_size = 2352;
                sector_data = 2048;
                seek_head = 16;
            } else {
                // Type 2: 2448-byte sectors with 2048-byte user data
                seek_ecc = 384;
                sector_size = 2448;
                sector_data = 2048;
                seek_head = 16;
            }
        } else {
            // Type 3: 2448-byte sectors with 2352-byte user data
            seek_head = 0;
            sector_size = 2448;
            seek_ecc = 96;
            sector_data = 2352;
        }

        return true;
    }
};

#endif // MDF_H
