// Strike
// Bad Ass Hackers 2021-2024

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include <vector>
#include "CMP.h"
#include "b3d_decompress.h"

using namespace std;


struct B3DHEADER
{
	char magic0[8];			// "B3D FILE"
	int magic1;				// 2200
	int magic2;				// 293

	float dunno2;			// 0.0f on most;   0.1  on bigcity


	char blob[436];


	float fval;				// dunno looked like a float in hex
	char blob2[1568];
	char dudename[5];		// "MIKEE"  or  "JOHNB"  or wutev
	char blob3[179];
};


// returns 0-based file offset
int searchfile(FILE* f, const char* szFind)
{
	int fold = ftell(f);

	fseek(f, 0, SEEK_END);
	int fend = ftell(f);

	fseek(f, 0, SEEK_SET);

	int slen = strlen(szFind);
	char* scratch = new char[slen];

	int offset = 0;

	// file bad?
	if (fread(scratch, 1, slen, f) != slen)
	{
		fseek(f, fold, SEEK_SET);
		return -1;
	}

	// not the most efficient, cause we IO read 1 byte at a time..  but hey these old files are small so the code is simple :D
	for (;;)
	{
		// check if found;  then reset and return
		if (!strncmp(scratch, szFind, slen))
		{
			fseek(f, fold, SEEK_SET);
			return offset;
		}

		// if EOF and still not good then we are done
		if ((offset + slen) >= fend)// dunno if theres a +/- 1 error here but who cares.. there will be nothing interesting at end of whole file
		{
			fseek(f, fold, SEEK_SET);
			return -1;
		}

		// not it;  shift everything left  and read 1 more byte at the end
		for (int x = 1; x < slen; x++)
			scratch[x - 1] = scratch[x];

		fread(&scratch[slen - 1], 1, 1, f);
		offset += 1;
	}
}

struct TEXINSTANCE
{
	int width;
	int height;
	int palIndex;
	int reserved;		// always 0
};

// 28 bytes
struct B3DTEXHEADER
{
	char magic[8];		// "B3D_TEX "
	int someSize;
};

void b3dexperiment(const char* szLevelFile, const char* szDumpDir, bool deleteDecomp)
{
	int lastslash = -1;
	int dot = -1;
	for (int i = strlen(szLevelFile) - 1; ; i--)
	{
		if (szLevelFile[i] == '.')
			dot = i;

		if (szLevelFile[i] == '\\' || szLevelFile[i] == '/')
		{
			lastslash = i;
			break;
		}
	}



	char levelname[32]; char* plevelname = levelname;
	for (int i = lastslash + 1; i < dot; i++)
		*(plevelname++) = szLevelFile[i];
	*plevelname = 0;



	char dumpdir[MAX_PATH];
	sprintf(dumpdir, "%s/%s", szDumpDir, levelname);


	CreateDirectory(dumpdir, NULL);



	char szDecodeFile[MAX_PATH];
	strcpy(szDecodeFile, dumpdir);
	strcat(szDecodeFile, "\\");
	strcat(szDecodeFile, levelname);
	strcat(szDecodeFile, ".dec");

	decompress(szLevelFile, szDecodeFile);


	FILE* f = fopen(szDecodeFile, "rb");



	int palsOffset = searchfile(f, "B3D_PALS");
	int texOffset = searchfile(f, "B3D_TEX");

	printf("%s:  PALS:%d  TEX:%d\n", szDecodeFile, palsOffset, texOffset);


	vector<tCMP_RGB*> pals;
	int texEnd;


	// XXXXXXX.b3d
	// 2212 bytes until first B3D_

	char* headerbuf = new char[2212];
	fread(headerbuf, 1, 2212, f);


	if (2212 != sizeof(B3DHEADER))
	{
		// if we're smart then these match :D
		printf("mismatch of b3d header size\n");
	}

	B3DHEADER* b3d = (B3DHEADER*)headerbuf;

	if (strncmp(b3d->magic0, "B3D FILE", 8) != 0 || b3d->magic1 != 2200 || b3d->magic2 != 293)
	{
		printf("header value check bad\n");
		goto done;
	}






	// read a palette.. how many palettes?
	{
		fseek(f, palsOffset + 8/*skip "B3D_PALS"*/, SEEK_SET);

		int value;
		fread(&value, 4, 1, f);

		//printf("INFO: pal unknown is: %d\n", value);

		int numpalettes = value / 768;

		printf("loading %d palettes...\n", numpalettes);
		for (int x = 0; x < numpalettes; x++)
		{
			tCMP_RGB* pal = new tCMP_RGB[256];
			fread(pal, sizeof(tCMP_RGB), 256, f);
			pals.push_back(pal);
		}
	}






	// lets just go to the tex offset
	fseek(f, texOffset, SEEK_SET);

	// read b3d tex header

	//if (sizeof(B3DTEXHEADER) != 28)
	//	printf("WARN: B3DTEXHEADER is not 28 bytes\n");

	B3DTEXHEADER texhdr;
	fread(&texhdr, sizeof(B3DTEXHEADER), 1, f);

	texEnd = texOffset + texhdr.someSize;

	for (int textry = 0; ; textry++)
	{
		printf("dumping %d\n", textry);

		// we done?
		int fcur = ftell(f);
		int fremain = texEnd - fcur;
		if (fremain <= 0)
		{
			// next thing should be like "B3D_"
			/*char magic[8];
			fread(magic, 1, 8, f);*/
			break;
		}


		// lets try loading one
		TEXINSTANCE ti;
		fread(&ti, sizeof(TEXINSTANCE), 1, f);


		printf("texdump %dx%d, %d, %d\n", ti.width, ti.height, ti.palIndex, ti.reserved);
		if (ti.reserved != 0)
		{
			printf("WHSFDFSDFAT");
		}


		unsigned char* tex = new unsigned char[ti.width * ti.height];
		fread(tex, 1, ti.width * ti.height, f);


		// dump bytes
		if (false)
		{
			char texdump[MAX_PATH];
			sprintf(texdump, "%s/%d.hex", dumpdir, textry);
			FILE* ft = fopen(texdump, "wb");
			fwrite(tex, 1, ti.width * ti.height, ft);
			fclose(ft);
		}


		// pick palette based on tex index?
		tCMP_RGB* pal = pals[ti.palIndex];


		// dump bmp?  lol
		{
			char bmpfile[MAX_PATH];
			sprintf(bmpfile, "%s/%d.bmp", dumpdir, textry);

			printf("dumping: %s...\n", bmpfile);

			BITMAPFILEHEADER bmFH;
			BITMAPINFOHEADER bmIH;

			ZeroMemory(&bmFH, sizeof(BITMAPFILEHEADER));
			ZeroMemory(&bmIH, sizeof(BITMAPINFOHEADER));

			// fill info header
			bmIH.biBitCount = 24;
			bmIH.biWidth = ti.width;
			bmIH.biHeight = -ti.height;
			bmIH.biPlanes = 1;
			bmIH.biSize = sizeof(BITMAPINFOHEADER);
			bmIH.biSizeImage = ti.width * ti.height * 3;

			// fill file header last
			bmFH.bfType = ('M' << 8) | 'B';
			bmFH.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
			bmFH.bfSize = bmFH.bfOffBits + bmIH.biSizeImage;

			errno = 0;
			FILE* fb = fopen(bmpfile, "wb");
			int fileerr = errno;

			fwrite(&bmFH, sizeof(BITMAPFILEHEADER), 1, fb);
			fwrite(&bmIH, sizeof(BITMAPINFOHEADER), 1, fb);

			for (int y = 0; y < ti.height; y++)
				for (int x = 0; x < ti.width; x++)
				{
					COLORREF clr;

					unsigned char pi = tex[x + y * ti.width];
					if (/*mi.trans &&*/ /*pi == 0 ||*/ pal == nullptr)
						clr = RGB(255, 0, 255);
					else
					{
						tCMP_RGB rgb = pal[pi];
						clr = RGB(rgb.r, rgb.g, rgb.b);
					}

					char r = GetRValue(clr);
					char g = GetGValue(clr);
					char b = GetBValue(clr);

					fwrite(&b, 1, 1, fb);
					fwrite(&g, 1, 1, fb);
					fwrite(&r, 1, 1, fb);

				}

			fclose(fb);


		}



		delete[] tex;

	}

done:
	for (vector<tCMP_RGB*>::iterator palI = pals.begin(); palI != pals.end(); palI++)
		delete[](*palI);
	pals.clear();

	delete[] headerbuf;

	fclose(f);



	if (deleteDecomp)
	{
		// lool we done?
		DeleteFile(szDecodeFile);
	}
}