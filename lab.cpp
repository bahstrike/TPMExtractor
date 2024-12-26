// Strike
// Bad Ass Hackers 2021-2024

#include "lab.h"
#include <Windows.h>
#include <stdio.h>

struct LABENTRY
{
	int a;
	int offset;
	int length;
	char type[4];
};

void extractlab(const char* szLabFile, const char* szDumpPath)
{
	FILE* biglab = fopen(szLabFile, "rb");
	if (biglab == nullptr)
	{
		printf("Couldn't open: %s\n", szLabFile);
		return;
	}

	char verif[4];
	fread(verif, 1, 4, biglab);

	int fileLenOffset;
	fread(&fileLenOffset, 4, 1, biglab);

	int numFiles;
	fread(&numFiles, 4, 1, biglab);

	int dunno;
	fread(&dunno, 4, 1, biglab);

	LABENTRY* entries = new LABENTRY[numFiles];
	fread(entries, sizeof(LABENTRY), numFiles, biglab);

	const int maxfilename = 128;

	char* entryNames = new char[numFiles * maxfilename];

	for (int fi = 0; fi < numFiles; fi++)
	{
		LABENTRY* entry = &entries[fi];

		char* sz = &entryNames[fi * maxfilename];

		for (int ci = 0; ; ci++)
		{
			char c;
			fread(&c, 1, 1, biglab);
			sz[ci] = c;
			if (c == 0)
				break;
		}



		//printf("%d: %d, %d, %d, %c%c%c%c\t%s\n", fi, entry->a, entry->offset, entry->length, entry->type[3], entry->type[2], entry->type[1], entry->type[0], &entryNames[fi*32]);
	}


	for (int fi = 0; fi < numFiles; fi++)
	{
		LABENTRY* entry = &entries[fi];

		char outfile[MAX_PATH];
		sprintf(outfile, "%s/%s", szDumpPath, &entryNames[fi * maxfilename]);

		printf("Extracting %s\n", outfile);

		char* tmp = new char[entry->length];
		fread(tmp, 1, entry->length, biglab);

		FILE* fo = fopen(outfile, "wb");
		fwrite(tmp, 1, entry->length, fo);
		fclose(fo);

		delete[] tmp;
	}



	delete[] entryNames;
	delete[] entries;


	fclose(biglab);
}