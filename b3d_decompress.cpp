// Code provided by shiny

#include <stdio.h>
#include <vector>

//#pragma pack(push, 1)
typedef struct file_struct
{
	FILE* real_fhand;
	int is_compressed;
	int seek_pos_maybe;
	int tmpbuf1_pos;
	int tmpbuf2_pos;
	int tmpbuf2_left;
	int tmpbuf1_left;
	int field_18;
	int field_14;
	int field_20;
	uint8_t reading_bitmask;
	uint8_t last_read_tmpbuf2;
	uint8_t tmpbuf_1[0x1000];
	uint8_t tmpbuf_2[0x2000];
} file_struct;

//#pragma pack(pop)

int decompress_helper_1(file_struct* fhand, char a2)
{
	int v3; // [esp+0h] [ebp-8h]
	unsigned int v4; // [esp+4h] [ebp-4h]

	v4 = 1 << (a2 - 1);
	v3 = 0;
	while (v4)
	{
		if (fhand->reading_bitmask == 0x80)
		{
			if (fhand->tmpbuf2_pos >= fhand->tmpbuf2_left)
				return 0;
			fhand->last_read_tmpbuf2 = (uint8_t)fhand->tmpbuf_2[fhand->tmpbuf2_pos++];
		}
		if ((fhand->reading_bitmask & fhand->last_read_tmpbuf2) != 0)
			v3 |= v4;
		v4 >>= 1;
		fhand->reading_bitmask >>= 1;
		if (!fhand->reading_bitmask)
			fhand->reading_bitmask = 0x80;
	}
	return v3;
}

int file_decompressy_idk(file_struct* fhand)
{
	int result; // eax
	int bit_masked; // [esp+8h] [ebp-24h]
	int to_read_tmpbuf2; // [esp+Ch] [ebp-20h]
	int i; // [esp+10h] [ebp-1Ch]
	int v5; // [esp+14h] [ebp-18h]
	size_t v6; // [esp+18h] [ebp-14h]
	char v7; // [esp+1Ch] [ebp-10h]
	char v8; // [esp+1Ch] [ebp-10h]
	int v9; // [esp+24h] [ebp-8h]
	int16_t v10; // [esp+28h] [ebp-4h]

	v5 = fhand->tmpbuf2_left - 0x80;
	fhand->seek_pos_maybe = 0;
	result = (int)fhand;
	fhand->tmpbuf1_pos = 0;
	if (!fhand->field_18)
	{
		while (1)
		{
			result = fhand->tmpbuf1_pos;
			if (result >= 0xF80)
				break;
			if (fhand->reading_bitmask == 0x80)
			{
				if (!fhand->field_14 && fhand->tmpbuf2_pos >= v5)
				{
					if (fhand->tmpbuf2_pos < fhand->tmpbuf2_left)
						memcpy(fhand->tmpbuf_2, &fhand->tmpbuf_2[fhand->tmpbuf2_pos], fhand->tmpbuf2_left - fhand->tmpbuf2_pos);
					fhand->tmpbuf2_left -= fhand->tmpbuf2_pos;
					fhand->tmpbuf2_pos = 0;
					to_read_tmpbuf2 = 0x1000 - fhand->tmpbuf2_left;
					v6 = fread(&fhand->tmpbuf_2[fhand->tmpbuf2_left], 1u, to_read_tmpbuf2, fhand->real_fhand);
					if (v6 != to_read_tmpbuf2)
						fhand->field_14 = 1;
					fhand->tmpbuf2_left += v6;
					v5 = fhand->tmpbuf2_left - 128;
				}
				result = (int)fhand;
				if (fhand->tmpbuf2_pos >= fhand->tmpbuf2_left)
					return result;
				fhand->last_read_tmpbuf2 = (uint8_t)fhand->tmpbuf_2[fhand->tmpbuf2_pos++];
			}
			bit_masked = fhand->reading_bitmask & fhand->last_read_tmpbuf2;
			fhand->reading_bitmask >>= 1;
			if (!fhand->reading_bitmask)
				fhand->reading_bitmask = 0x80;
			if (bit_masked)
			{
				v7 = decompress_helper_1(fhand, 8);
				fhand->tmpbuf_2[fhand->field_20 + 0x1000] = v7;
				fhand->tmpbuf_1[fhand->tmpbuf1_pos++] = v7;
				fhand->field_20 = ((uint16_t)fhand->field_20 + 1) & 0xFFF;
			}
			else
			{
				result = decompress_helper_1(fhand, 12);
				v10 = result;
				if (!result)
				{
					fhand->field_18 = 1;
					return result;
				}
				v9 = decompress_helper_1(fhand, 4) + 1;
				for (i = 0; i <= v9; ++i)
				{
					v8 = fhand->tmpbuf_2[(((uint16_t)i + v10) & 0xFFF) + 4096];
					fhand->tmpbuf_1[fhand->tmpbuf1_pos++] = v8;
					fhand->tmpbuf_2[fhand->field_20 + 4096] = v8;
					fhand->field_20 = ((uint16_t)fhand->field_20 + 1) & 0xFFF;
				}
			}
		}
	}
	return result;
}

size_t fread_wrap_compressed(void* a1, size_t a2, size_t a3, file_struct* a4)
{
	int v5; // [esp+8h] [ebp-10h]
	signed int i; // [esp+Ch] [ebp-Ch]
	signed int bytes_read; // [esp+10h] [ebp-8h]
	char* v8; // [esp+14h] [ebp-4h]

	v8 = (char*)a1;
	if (!a4->is_compressed)
		return fread(a1, a2, a3, a4->real_fhand);

	v5 = 0;
	for (i = a2 * a3; i; i -= bytes_read)
	{
		if (a4->seek_pos_maybe >= a4->tmpbuf1_pos)
			file_decompressy_idk(a4);
		bytes_read = a4->tmpbuf1_pos - a4->seek_pos_maybe;

		//printf("read %x\n", bytes_read);

		if (!bytes_read)
			return v5 / (int)a2;
		if (bytes_read > i)
			bytes_read = i;
		memcpy(v8, &a4->tmpbuf_1[a4->seek_pos_maybe], bytes_read);
		v8 += bytes_read;
		a4->seek_pos_maybe += bytes_read;
		v5 += bytes_read;
	}
	return v5 / (int)a2;
}

void decompress(const char* open_fpath, const char* out_fpath)
{
	file_struct f2;
	FILE* f = fopen(open_fpath, "rb");
	FILE* fout = fopen(out_fpath, "wb");

	// Weird file struct
	memset(&f2, 0, sizeof(f2));
	f2.real_fhand = f;
	f2.is_compressed = 1;
	f2.reading_bitmask = 0x80;
	f2.field_20 = 1;

	// Read the header, edit compressed bit, and decompress as much as possible
	uint8_t tmp[0x10000];
	fread(tmp, 1, 0x8A4, f);
	tmp[0x328] = 0; // decompressed
	fwrite(tmp, 1, 0x8A4, fout);

	while (1)
	{
		size_t len = fread_wrap_compressed(tmp, 1, 0x10000, &f2);

		if (!len) break;
		fwrite(tmp, 1, len, fout);
	}

	//printf("Decompressed to `%s`\n", out_fpath);

	fclose(fout);
	fclose(f);
}