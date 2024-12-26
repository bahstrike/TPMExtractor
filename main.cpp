// Strike
// Bad Ass Hackers 2021-2024

#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include "b3d.h"
#include "baf.h"
#include "lab.h"
#include "./unshieldv3/unshieldv3.h"

bool fileexist(const char* sz)
{
	FILE* f = fopen(sz, "rb");
	if (f == nullptr)
		return false;

	fclose(f);
	return true;
}

int main(int argc, char* argv[])
{
	printf("---------------------------------------------------\n");
	printf("|          TPM Extractor - BAH 2021-2024          |\n");
	printf("|            Strike & shiny & KnightCop           |\n");
	printf("|                                                 |\n");
	printf("|       .Z decompression by Wolfgang Frisch       |\n");
	printf("|                                                 |\n");
	printf("|     This project is in no way affiliated with   |\n");
	printf("|       LEC or Disney. Use at your own peril.     |\n");
	printf("---------------------------------------------------\n");


	if (argc < 3)
	{
		printf("Usage: TPMExtractor.exe DiscDir DumpDir\n");
		goto end;
	}

	char discRoot[MAX_PATH];
	strcpy(discRoot, argv[1]);

	char dumpRoot[MAX_PATH];
	strcpy(dumpRoot, argv[2]);





	char bigZPath[MAX_PATH];
	sprintf(bigZPath, "%s/GAMEDATA/GOBS/BIG.Z", discRoot);

	if(!fileexist(bigZPath))
	{
		printf("Could not find: %s\n", bigZPath);
		goto end;
	}
	printf("Install disc: %s\n", discRoot);



	{
		printf("Dump directory: %s\n", dumpRoot);

		if(!CreateDirectory(dumpRoot, NULL))
			if (GetLastError() == ERROR_PATH_NOT_FOUND)
			{
				printf("Couldn't create dump directory: %s\n", dumpRoot);
				goto end;
			}
	}



	char bigLabDump[MAX_PATH];
	sprintf(bigLabDump, "%s/biglab/", dumpRoot);
	CreateDirectory(bigLabDump, NULL);


	char checklab[MAX_PATH];
	sprintf(checklab, "%s/_backup_.baf", bigLabDump);
	//if (!fileexist(checklab))
	{
		char bigLabPath[MAX_PATH];
		sprintf(bigLabPath, "%s/big.lab", dumpRoot);
		//if (!fileexist(bigLabPath))
		{
			printf("Extracting BIG.Z -> big.lab\n");

			if (!extract(bigZPath, dumpRoot))
				goto end;
		}


		printf("Extract big.lab to: %s...\n", bigLabDump);
		extractlab(bigLabPath, bigLabDump);


		DeleteFile(bigLabPath);
	}



	char localizeLabDump[MAX_PATH];
	sprintf(localizeLabDump, "%s/localizelab", dumpRoot);
	CreateDirectory(localizeLabDump, NULL);

	sprintf(checklab, "%s/courier.gcf", localizeLabDump);
	//if (!fileexist(checklab))
	{
		printf("Extract LOCALIZE.LAB to: %s...\n", localizeLabDump);

		char localizeLabPath[MAX_PATH];
		sprintf(localizeLabPath, "%s/GAMEDATA/GOBS/LOCALIZE.LAB", discRoot);

		extractlab(localizeLabPath, localizeLabDump);
	}



	char voiceLabDump[MAX_PATH];
	sprintf(voiceLabDump, "%s/voicelab", dumpRoot);
	CreateDirectory(voiceLabDump, NULL);

	sprintf(checklab, "%s/3GD6715.WV", voiceLabDump);
	//if (!fileexist(checklab))
	{
		printf("Extract VOICE.LAB to: %s...\n", voiceLabDump);

		char voiceLabPath[MAX_PATH];
		sprintf(voiceLabPath, "%s/GAMEDATA/GOBS/VOICE.LAB", discRoot);

		extractlab(voiceLabPath, voiceLabDump);
	}



	{
		char levelDumpRoot[MAX_PATH];
		sprintf(levelDumpRoot, "%s/LEVEL/", dumpRoot);

		CreateDirectory(levelDumpRoot, NULL);



		const char* levels[] = {
		"ASSAULT.B3D",
		"BIGCITY.B3D",
		"ESPA.B3D",
		"FEDSHIP.B3D",
		"FINAL.B3D",
		"GARDEN.B3D",
		"GUNGA.B3D",
		"MAUL.B3D",
		"QUEEN.B3D",
		"RACE.B3D",
		"SWAMP.B3D"
		};

		for (int x = 0; x < sizeof(levels) / sizeof(char*); x++)
		{
			char b3dfile[MAX_PATH];
			sprintf(b3dfile, "%s/GAMEDATA/LEVEL/%s", discRoot, levels[x]);


			b3dexperiment(b3dfile, levelDumpRoot, true);
		}
	}



	{
		printf("Processing .BAF files...\n");

		char bafdump[MAX_PATH];
		sprintf(bafdump, "%s/BAF/", dumpRoot);
		CreateDirectory(bafdump, NULL);

		char findbaf[MAX_PATH];
		sprintf(findbaf, "%s*.baf", bigLabDump);
		WIN32_FIND_DATA fd;
		HANDLE searchHandle = FindFirstFile(findbaf, &fd);
		while (searchHandle != INVALID_HANDLE_VALUE)
		{
			// dont read paths
			if ((~fd.dwFileAttributes) & FILE_ATTRIBUTE_DIRECTORY)
			{
				char baffile[MAX_PATH];
				sprintf(baffile, "%s/%s", bigLabDump, fd.cFileName);


				char bafname[32];
				strcpy(bafname, fd.cFileName);
				for(int i=0; ; i++)
					if (bafname[i] == '.')
					{
						bafname[i] = 0;
						break;
					}

				char baffiledump[MAX_PATH];
				sprintf(baffiledump, "%s/%s/", bafdump, bafname);
				CreateDirectory(baffiledump, NULL);


				dumpbaf(baffile, baffiledump);
			}

			if (!FindNextFile(searchHandle, &fd))
				break;
		}

	}


end:
	printf("\n\n---------------Press any key to exit---------------\n");

	while (!_getch())
		Sleep(10);

	return 0;
}