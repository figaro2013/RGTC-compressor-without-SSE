/*
Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
*/
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <emmintrin.h>

typedef uint8_t byte;
typedef uint32_t dword;
typedef uint64_t uint64;

//------------------------------------------------------------------------------------
//                              RGTC compression
//------------------------------------------------------------------------------------

void CompressRGTCFromRGBA8_Gen(const byte *srcPtr, int width, int height, int stride, byte *dstPtr) {
    int bw = (width + 3) / 4;
    int bh = (height + 3) / 4;
    uint64 *dstBlocks = (uint64*)dstPtr;
    byte block[4][4];

    for (int brow = 0; brow < bh; brow++) {
        for (int bcol = 0; bcol < bw; bcol++) {
            for (int comp = 0; comp < 2; comp++) {

                // load block
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++) {
                        int i = brow * 4 + r;
                        int j = bcol * 4 + c;
                        // use "clamp" continuation
                        if (i > height - 1)
                            i = height - 1;
                        if (j > width - 1)
                            j = width - 1;
                        block[r][c] = srcPtr[i * stride + 4 * j + comp];
                    }

                // compute min/max
                int minv = block[0][0];
                int maxv = block[0][0];
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++) {
                        if((int)block[r][c] < minv)
                            minv = (int)block[r][c];
                        if((int)block[r][c] > maxv)
                            maxv = (int)block[r][c];
                    }
                // be sure min < max, so that 7-step case is used
                if (minv == maxv) {
                    if (maxv < 255)
                        maxv++;
                    else
                        minv--;
                }

                uint64 blockData = maxv + (minv << 8);
                int bits = 16;
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++) {
                        // compute ratio
                        int numer = block[r][c] - minv;
                        int denom = maxv - minv;
                        #if 0
                        // find closest ramp point
                        int idx = (numer * 7 + (denom >> 1)) / denom;
                        #else
                        // this code yields closest ramp point in most cases
                        // among all 32K ratios D/N, there are 258 exceptions with N >= 65
                        // in exceptional cases, ratio is close to middle, and chosen ramp point is almost as close
                        // Note: this code is used here to match CompressRGTCFromRGBA8_Kernel8x4 !
                        int mult = ((7 << 12) + denom-1) / denom;
                        int idx = (mult * numer + (1 << 11)) >> 12;
                        #endif
                        // convert to DXT5 index
                        uint64 val = 8 - idx;
                        if (idx == 7)
                            val = 0;
                        if (idx == 0)
                            val = 1;
                        //append to bit stream
                        blockData += val << bits;
                        bits += 3;
                    }
                assert(bits == 64);

                *dstBlocks++ = blockData;
            }
        }
    }
}

//------------------------------------------------------------------------------------
//                       Full compression and mipmaps generation
//------------------------------------------------------------------------------------

struct MipmapLevel {
    byte *data;
    int width, height;
    int sizeInBytes;
};

struct MipmapLevel CompressAndNOTGenerateMipmaps(const byte *data, int width, int height) {
    struct MipmapLevel result;

    const byte *srcData = data;
    int levw = width;
    int levh = height;

    int bw = (levw + 3) >> 2;
    int bh = (levh + 3) >> 2;

    byte *dstData = (byte*)malloc(bw * bh * 16);
    CompressRGTCFromRGBA8_Gen(srcData, levw, levh, 4 * levw, dstData);

    result.data = dstData;
    result.width = levw;
    result.height = levh;
    result.sizeInBytes = bw * bh * 16;

    if (srcData != data)
        free((byte*)srcData);

    return result;
}

//------------------------------------------------------------------------------------
//                                 TGA reader
//------------------------------------------------------------------------------------

#pragma pack(push, 1)
typedef struct TgaImageSpecs {
    uint16_t originX;
    uint16_t originY;
    uint16_t width;
    uint16_t height;
    uint8_t bpp;
    uint8_t desc;
} TgaImageSpecs;
typedef struct TgaHeader {
    uint8_t identificationLength;
    uint8_t colorMapType;
    uint8_t imageTypeCode;
    char colorMapSpecs[5];
    TgaImageSpecs imageSpecs; 
} TgaHeader;
#pragma pack(pop)

byte* ReadTGA(const char *filename, int *width, int *height) {
    FILE *fin = fopen(filename, "rb");
    assert(fin);

    TgaHeader tgaHeader;
    fread(&tgaHeader, sizeof(TgaHeader), 1, fin);

    //no color map in the file
    assert(tgaHeader.colorMapType == 0);
    //true-color RGB without RLE compression
    assert(tgaHeader.imageTypeCode == 2);
    //only 32-bit RGBA images and 24-bit RGB images are supported
    assert(tgaHeader.imageSpecs.bpp == 32 || tgaHeader.imageSpecs.bpp == 24);
    //only old Truevision images supported, no way to set order of lines
    assert((tgaHeader.imageSpecs.desc & 0xF0) == 0);

    *width = tgaHeader.imageSpecs.width;
    *height = tgaHeader.imageSpecs.height;
    byte *data = (byte*)malloc(*width * *height * 4);

    int bpp = tgaHeader.imageSpecs.bpp / 8;
    fread(data, *width * *height * bpp, 1, fin);

    byte tmp = 0;
    for (int i = *width * *height - 1; i >= 0; i--) {
        data[4*i+0] = data[bpp*i+0];
        data[4*i+1] = data[bpp*i+1];
        data[4*i+2] = data[bpp*i+2];
        data[4*i+3] = 255;
        tmp = data[4*i+0];
        data[4*i+0] = data[4*i+2];
        data[4*i+2] = tmp;
    }

    fclose(fin);
    return data;
}

//------------------------------------------------------------------------------------
//                                 DDS writer
//------------------------------------------------------------------------------------

struct DdsPixelformat {
  dword dwSize;
  dword dwFlags;
  dword dwFourCC;
  dword dwRGBBitCount;
  dword dwRBitMask;
  dword dwGBitMask;
  dword dwBBitMask;
  dword dwABitMask;
};
struct DdsHeader {
  dword           dwSize;
  dword           dwFlags;
  dword           dwHeight;
  dword           dwWidth;
  dword           dwPitchOrLinearSize;
  dword           dwDepth;
  dword           dwMipMapCount;
  dword           dwReserved1[11];
  struct DdsPixelformat  ddspf;
  dword           dwCaps;
  dword           dwCaps2;
  dword           dwCaps3;
  dword           dwCaps4;
  dword           dwReserved2;
};
enum DdsFlags {
    DDSF_CAPS           = 0x00000001,
    DDSF_HEIGHT         = 0x00000002,
    DDSF_WIDTH          = 0x00000004,
    DDSF_PIXELFORMAT    = 0x00001000,
    DDSF_MIPMAPCOUNT    = 0x00020000,
    DDSF_LINEARSIZE     = 0x00080000,
    DDSF_FOURCC         = 0x00000004,
    DDSF_COMPLEX        = 0x00000008,
    DDSF_TEXTURE        = 0x00001000,
    DDSF_MIPMAP         = 0x00400000,
};

void WriteDDS(const char *filename, const struct MipmapLevel levels) {
    FILE *fout = fopen(filename, "wb");
    assert(fout);

    fwrite("DDS ", 4, 1, fout);

    struct DdsHeader header;
    memset(&header, 0, sizeof(header));
    header.dwSize = sizeof(header);
    header.dwFlags = DDSF_CAPS | DDSF_PIXELFORMAT | DDSF_WIDTH | DDSF_HEIGHT;
    header.dwFlags |= DDSF_LINEARSIZE | DDSF_MIPMAPCOUNT;
    header.dwWidth = levels.width;
    header.dwHeight = levels.height;
    header.dwPitchOrLinearSize = levels.sizeInBytes;
    header.dwMipMapCount = 1;
    header.ddspf.dwSize = sizeof(header.ddspf);
    header.ddspf.dwFlags = DDSF_FOURCC;
    memcpy(&header.ddspf.dwFourCC, "ATI2", 4);
    header.dwCaps = DDSF_TEXTURE | DDSF_MIPMAP | DDSF_COMPLEX;

    fwrite(&header, sizeof(header), 1, fout);

    fwrite(levels.data, levels.sizeInBytes, 1, fout);

    fclose(fout);
}

//------------------------------------------------------------------------------------
//                                 entry point
//------------------------------------------------------------------------------------

int main() {
    byte *data;
    int width;
    int height;

    char *filename = "woodfloor1"; //pinkpurplecircle multicolor1 woodcontainer1 woodfloor1
    char *path1 = "test_pics\\";
    char *readpath2 = ".tga";
    char *writepath2 = "_RGTC_encoded.dds";
    char *readfilepath = (char *) malloc(strlen(path1) + strlen(filename) + strlen(readpath2));
    sprintf(readfilepath, "%s%s%s", path1, filename, readpath2);
    data = ReadTGA(readfilepath, &width, &height);

    struct MipmapLevel levels;
    int startTime = clock();
    levels = CompressAndNOTGenerateMipmaps(data, width, height);
    int deltaTime = clock() - startTime;

    char *writefilepath = (char *) malloc(strlen(path1) + strlen(filename) + strlen(writepath2));
    sprintf(writefilepath, "%s%s%s", path1, filename, writepath2);
    WriteDDS(writefilepath, levels);

    printf("Running code took %0.2lf milliseconds\n", 1e+3 * deltaTime / CLOCKS_PER_SEC);

    return 0;
}
