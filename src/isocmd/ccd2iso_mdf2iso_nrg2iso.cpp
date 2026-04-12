// SPDX-License-Identifier: GPL-3.0-or-later

#include "../headers.h"
#include "../mdf.h"
#include "../ccd.h"


// Special thanks to the original authors of the conversion tools:

// Salvatore Santagati (mdf2iso).
// Grégory Kokanosky (nrg2iso).
// Danny Kurniawan and Kerry Harris (ccd2iso).

// Note: Their original code has been modernized and ported to C++.


// MDF2ISO

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


ConversionResult convertMdfToIso(const std::string& mdfPath, const std::string& isoPath, std::atomic<size_t>* completedBytes) {
    if (g_operationCancelled.load()) return ConversionResult::Cancelled;

    std::ifstream mdfFile(mdfPath, std::ios::binary);
    if (!mdfFile.is_open()) return ConversionResult::Failed;

    mdfFile.seekg(32768);
    char buf[12];
    if (!mdfFile.read(buf, 8) || std::memcmp("CD001", buf + 1, 5) == 0)
        return ConversionResult::Failed;

    if (g_operationCancelled.load()) return ConversionResult::Cancelled;

    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile.is_open()) return ConversionResult::Failed;

    size_t seek_ecc = 0, sector_size = 0, seek_head = 0, sector_data = 0;

    mdfFile.seekg(0);
    if (!mdfFile.read(buf, 12)) return ConversionResult::Failed;

    if (std::memcmp("\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, 12) == 0) {
        mdfFile.seekg(2352);
        if (!mdfFile.read(buf, 12)) return ConversionResult::Failed;
        if (std::memcmp("\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", buf, 12) == 0) {
            seek_ecc = 288; sector_size = 2352; sector_data = 2048; seek_head = 16;
        } else {
            seek_ecc = 384; sector_size = 2448; sector_data = 2048; seek_head = 16;
        }
    } else {
        seek_head = 0; sector_size = 2448; seek_ecc = 96; sector_data = 2352;
    }

    mdfFile.seekg(0, std::ios::end);
    size_t source_length = static_cast<size_t>(mdfFile.tellg()) / sector_size;
    mdfFile.seekg(0, std::ios::beg);

    std::vector<char> sectorBuffer(sector_data);

    while (source_length > 0) {
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            return ConversionResult::Cancelled;
        }

        mdfFile.seekg(static_cast<std::streamoff>(seek_head), std::ios::cur);
        if (!mdfFile.read(sectorBuffer.data(), sector_data)) return ConversionResult::Failed;
        mdfFile.seekg(static_cast<std::streamoff>(seek_ecc), std::ios::cur);

        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            return ConversionResult::Cancelled;
        }

        if (!isoFile.write(sectorBuffer.data(), sector_data)) return ConversionResult::Failed;

        if (completedBytes)
            completedBytes->fetch_add(sector_data, std::memory_order_relaxed);

        --source_length;
    }

    return ConversionResult::Success;
}


// CCD2ISO

/***************************************************************************
 *   Copyright (C) 2003 by Danny Kurniawan                                 *
 *   danny_kurniawan@users.sourceforge.net                                 *
 *                                                                         *
 *   Contributors:                                                         *
 *   - Kerry Harris <tomatoe-source@users.sourceforge.net>                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
 

ConversionResult convertCcdToIso(const std::string& ccdPath, const std::string& isoPath, std::atomic<size_t>* completedBytes) {
    if (g_operationCancelled.load()) return ConversionResult::Cancelled;

    std::ifstream ccdFile(ccdPath, std::ios::binary);
    if (!ccdFile) return ConversionResult::Failed;
    std::ofstream isoFile(isoPath, std::ios::binary);
    if (!isoFile) return ConversionResult::Failed;

    CcdSector sector;
    size_t sectorNum = 0;

    while (ccdFile.read(reinterpret_cast<char*>(&sector), sizeof(CcdSector))) {
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            return ConversionResult::Cancelled;
        }
        size_t bytesWritten = 0;

        switch (sector.sectheader.header.mode) {
            case 1:
                isoFile.write(reinterpret_cast<char*>(sector.content.mode1.data), DATA_SIZE);
                bytesWritten = DATA_SIZE;
                break;
            case 2:
                isoFile.write(reinterpret_cast<char*>(sector.content.mode2.data), DATA_SIZE);
                bytesWritten = DATA_SIZE;
                break;
            case 0xe2:
                return ConversionResult::Success;
            default:
                return ConversionResult::Failed;
        }

        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(isoPath);
            return ConversionResult::Cancelled;
        }

        if (!isoFile || bytesWritten != DATA_SIZE) return ConversionResult::Failed;

        if (completedBytes)
            completedBytes->fetch_add(bytesWritten, std::memory_order_relaxed);

        sectorNum++;
    }
    return ConversionResult::Success;
}


// NRG2ISO

/* 
   29/04/2021 Nrg2Iso v0.4.1

   Copyright (C) 2003-2021 Gregory Kokanosky <gregory.kokanosky@free.fr>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/


ConversionResult convertNrgToIso(const std::string& inputFile, const std::string& outputFile, std::atomic<size_t>* completedBytes) {
    if (g_operationCancelled.load()) return ConversionResult::Cancelled;

    std::ifstream nrgFile(inputFile, std::ios::binary);
    if (!nrgFile) return ConversionResult::Failed;

    constexpr size_t ISO_CHECK_OFFSET = 16 * 2048;
    char isoBuf[8];
    nrgFile.seekg(ISO_CHECK_OFFSET);
    nrgFile.read(isoBuf, 8);

    if (memcmp(isoBuf, "\x01" "CD001" "\x01\x00", 8) == 0)
        return ConversionResult::Failed;

    nrgFile.clear();
    nrgFile.seekg(307200, std::ios::beg);

    if (g_operationCancelled.load()) return ConversionResult::Cancelled;

    std::ofstream isoFile(outputFile, std::ios::binary);
    if (!isoFile) return ConversionResult::Failed;

    constexpr size_t BUFFER_SIZE = 1024 * 1024;
    std::vector<char> buffer(BUFFER_SIZE);

    while (nrgFile) {
        if (g_operationCancelled.load()) {
            isoFile.close();
            fs::remove(outputFile);
            return ConversionResult::Cancelled;
        }

        nrgFile.read(buffer.data(), BUFFER_SIZE);
        std::streamsize bytesRead = nrgFile.gcount();

        if (bytesRead > 0) {
            if (g_operationCancelled.load()) {
                isoFile.close();
                fs::remove(outputFile);
                return ConversionResult::Cancelled;
            }

            if (!isoFile.write(buffer.data(), bytesRead)) {
                isoFile.close();
                fs::remove(outputFile);
                return ConversionResult::Failed;
            }

            if (completedBytes)
                completedBytes->fetch_add(bytesRead, std::memory_order_relaxed);
        }

        if (nrgFile.eof() || nrgFile.fail()) break;
    }

    return ConversionResult::Success;
}
