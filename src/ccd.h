// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef CCD_H
#define CCD_H


// Special thanks to the original authors of the ccd2iso:

// Danny Kurniawan and Kerry Harris (ccd2iso).

// Note: Their original code has been modernized and ported to C++.

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

const size_t DATA_SIZE = 2048;

struct __attribute__((packed)) CcdSectheaderSyn {
    uint8_t data[12];
};

struct __attribute__((packed)) CcdSectheaderHeader {
    uint8_t sectaddr_min, sectaddr_sec, sectaddr_frac;
    uint8_t mode;
};

struct __attribute__((packed)) CcdSectheader {
    CcdSectheaderSyn syn;
    CcdSectheaderHeader header;
};

struct __attribute__((packed)) CcdSector {
    CcdSectheader sectheader;
    union {
        struct {
            uint8_t data[DATA_SIZE];
            uint8_t edc[4];
            uint8_t unused[8];
            uint8_t ecc[276];
        } mode1;
        struct {
            uint8_t sectsubheader[8];
            uint8_t data[DATA_SIZE];
            uint8_t edc[4];
            uint8_t ecc[276];
        } mode2;
    } content;
};

#endif // CCD_H
