// Strike
// Bad Ass Hackers 2005 or something lol

#pragma once

//#include "defines.h"

#include <windows.h>
//#include "ResFile.h"

// CMP header format
struct tCMP_Header
{
	char verification[4];	// should be 'CMP '
	long version;			// CMP file version
	long transparency;		// transparency flag
	char unknown[52];		// unknown data, padding?
};

// CMP pixel rgb
struct tCMP_RGB
{
	unsigned char r, g, b;		// the color makeup
};

// CMP data format
struct tCMP_Data
{
	struct tCMP_Header header;			// CMP file header
	struct tCMP_RGB palette[256];		// CMP palette info
	unsigned char lightmap[16384];		// CMP lighting info  ( 64 * 256 )
	unsigned char *transTable;			// pointer to optional transparency tables
};

// CMP Object declaration
class CCMP
{
private:
	////////////////////////////////////////////////////////
	// Function		:		nullify
	//
	// Purpose		:		Nullifies values
	//
	// In			:		N/A
	//
	// Out			:		N/A
	//
	// Return		:		N/A
	////////////////////////////////////////////////////////
	void nullify();
protected:
	// The colormap data
	struct tCMP_Data m_data;

public:
	////////////////////////////////////////////////////////
	// Function		:		newColormap
	//
	// Purpose		:		Destroys the current colormap and sets defaults
	//
	// In			:		N/A
	//
	// Out			:		N/A
	//
	// Return		:		N/A
	////////////////////////////////////////////////////////
	void newColormap();

	////////////////////////////////////////////////////////
	// Function		:		loadColormap
	//
	// Purpose		:		Imports a .CMP file into the colormap data
	//
	// In			:		szFilename - String containing .cmp file to load
	//
	// Out			:		N/A
	//
	// Return		:		True on success
	////////////////////////////////////////////////////////
	//bool loadColormap(char *szFilename);

	////////////////////////////////////////////////////////
	// Function		:		loadColormap
	//
	// Purpose		:		Imports a .CMP file into the colormap data
	//
	// In			:		fStream - The filestream to load the colormap from
	//
	// Out			:		N/A
	//
	// Return		:		True on success
	////////////////////////////////////////////////////////
	//bool loadColormap(CResFile *pResFile);

	////////////////////////////////////////////////////////
	// Function		:		saveColormap
	//
	// Purpose		:		Exports the colormap data into a .CMP
	//
	// In			:		szFilename - String containing .cmp file to create/overwrite
	//
	// Out			:		N/A
	//
	// Return		:		True on success
	////////////////////////////////////////////////////////
	bool saveColormap(char *szFilename);

	////////////////////////////////////////////////////////
	// Function		:		getColor
	//	
	// Purpose		:		Gets the requested color in the form of a 32-bit COLORREF
	//
	// In			:		nColorId - Any palette entry (0-255)
	//
	// Out			:		N/A
	//
	// Return		:		The RGB color
	////////////////////////////////////////////////////////
	COLORREF getColor(int nColorId)
	{
		// make sure id is good
		if((nColorId < 0) || (nColorId > 255))
			return 0;

		// get rgb entry
		struct tCMP_RGB *entry = getPalette() + nColorId;

		// construct and return a color
		return RGB(entry->r, entry->g, entry->b);
	}

	////////////////////////////////////////////////////////
	// Function		:		getColor
	//	
	// Purpose		:		Gets the requested color in the form of a three 8-bit R/G/B values
	//
	// In			:		nColorId - Any palette entry (0-255)
	//
	// Out			:		pcRed	- Storage location for Red value (0-255). If NULL, this color will not be retrieved
	//						pcGreen	- Storage location for Green value (0-255). If NULL, this color will not be retrieved
	//						pcBlue	- Storage location for Blue value (0-255). If NULL, this color will not be retrieved
	//
	// Return		:		True on success. Function will fail and return false if ID is bad
	////////////////////////////////////////////////////////
	bool getColor(int nColorId, unsigned char *cRed, unsigned char *cGreen, unsigned char *cBlue)
	{
		// make sure id is good
		if((nColorId < 0) || (nColorId > 255))
			return false;

		// get rgb entry
		struct tCMP_RGB *entry = getPalette() + nColorId;

		// get color red
		if(cRed != 0)
			*cRed = entry->r;

		// get color green
		if(cGreen != 0)
			*cGreen = entry->g;

		// get color blue
		if(cBlue != 0)
			*cBlue = entry->b;

		// success
		return true;
	}

	////////////////////////////////////////////////////////
	// Function		:		setColor
	//	
	// Purpose		:		Sets the specified color
	//
	// In			:		nColorId - Any palette entry (0-255)
	//						nRed	 - Red value (0-255)
	//						nGreen	 - Green value (0-255)
	//						nBlue    - Blue value (0-255)
	//
	// Out			:		N/A
	//
	// Return		:		N/A
	////////////////////////////////////////////////////////
	void setColor(int nColorId, int nRed, int nGreen, int nBlue)
	{
		// make sure id is good
		if((nColorId < 0) || (nColorId > 255))
			return;

		// get the palette entry
		struct tCMP_RGB *entry = getPalette() + nColorId;

		// set values
		entry->r = (nRed>=255 ? 255 : (nRed<=0 ? 0 : nRed));			// clamp value to 0-255
		entry->g = (nGreen>=255 ? 255 : (nGreen<=0 ? 0 : nGreen));		// clamp value to 0-255
		entry->b = (nBlue>=255 ? 255 : (nBlue<=0 ? 0 : nBlue));			// clamp value to 0-255
	}

	////////////////////////////////////////////////////////
	// Function		:		setColor
	//	
	// Purpose		:		Sets the specified color
	//
	// In			:		nColorId - Any palette entry (0-255)
	//						color    - COLORREF of the color
	//
	// Out			:		N/A
	//
	// Return		:		N/A
	////////////////////////////////////////////////////////
	void setColor(int nColorId, COLORREF color)
	{
		// make sure id is good
		if((nColorId < 0) || (nColorId > 255))
			return;

		// get the palette entry
		struct tCMP_RGB *entry = getPalette() + nColorId;

		// set values
		entry->r = GetRValue(color);
		entry->g = GetGValue(color);
		entry->b = GetBValue(color);
	}

	////////////////////////////////////////////////////////
	// Function		:		getLightValue
	//
	// Purpose		:		Returns specified light value
	//
	// In			:		nTableId - Any table (0-63)
	//						nEntryId - Any table entry (0-255)
	//
	// Out			:		N/A
	//
	// Return		:		The requested light value (0-255)
	////////////////////////////////////////////////////////
	int getLightValue(int nTableId, int nEntryId)
	{
		if((nTableId>=0) && (nTableId<=63) && (nEntryId >= 0) && (nEntryId <= 255))
			return (int)m_data.lightmap[nEntryId + nTableId * 256];
		else
			return 0;
	}

	////////////////////////////////////////////////////////
	// Function		:		setLightValue
	//
	// Purpose		:		Sets light value to specified entry
	//
	// In			:		nTableId	- Any table (0-63)
	//						nEntryId	- Any table entry (0-255)
	//						nLightValue - The light value to set (0-255)
	//
	// Out			:		N/A
	//
	// Return		:		N/A
	////////////////////////////////////////////////////////
	void setLightValue(int nTableId, int nEntryId, int nLightValue)
	{
		// make sure ids are good
		if((nTableId < 0) || (nTableId > 63) || (nEntryId < 0) || (nEntryId > 255))
			return;

		m_data.lightmap[nEntryId + nTableId * 256] = (unsigned char)(nLightValue>=255 ? 255 : (nLightValue<=0 ? 0 : nLightValue));
	}

	////////////////////////////////////////////////////////
	// Function		:		getTransparencyValue
	//
	// Purpose		:		Returns specified transparency value. This will always return 0 if there is no transparency table.
	//
	// In			:		nTableId	- Any table (0-255)
	//						nEntryId	- Any table entry (0-255)
	//
	// Out			:		N/A
	//
	// Return		:		The requested transparency value (0-255)
	////////////////////////////////////////////////////////
	int getTransparencyValue(int nTableId, int nEntryId)
	{
		// make sure trans table is good and color id is good
		if((((nEntryId>=0) && (nEntryId<=255)) && ((nTableId>=0) && (nTableId<=255))) && m_data.header.transparency)
			return (int)m_data.transTable[nEntryId + nTableId * 256];
		else
			return 0;
	}

	////////////////////////////////////////////////////////
	// Function		:		setTransparencyValue
	//
	// Purpose		:		Sets transparency value to specified entry. If no transparency table
	//						currently exists, it is created.
	//
	// In			:		nTableId	- Any table (0-255)
	//						nEntryId	- Any table entry (0-255)
	//						nTransValue - The transparency value (0-255)
	//
	// Out			:		N/A
	//
	// Return		:		N/A
	////////////////////////////////////////////////////////
	void setTransparencyValue(int nTableId, int nEntryId, int nTransValue)
	{
		// make sure ids are good
		if((nTableId < 0) || (nTableId > 255) || (nEntryId < 0) || (nEntryId > 255))
			return;

		// is there not a transparency table yet?
		if(!m_data.header.transparency)
		{
			// allocate memory
			m_data.transTable = new unsigned char[256 * 256];

			// zero memory
			memset(m_data.transTable, 0, 256 * 256);
		}

		// store the value
		m_data.transTable[nEntryId + nTableId * 256] = (unsigned char)(nTransValue>=255 ? 255 : (nTransValue<=0 ? 0 : nTransValue));
	}

	////////////////////////////////////////////////////////
	// Function		:		getPalette
	//
	// Purpose		:		Returns pointer to palette data. Array is 256 long
	//
	// In			:		N/A
	//
	// Out			:		N/A
	//
	// Return		:		Pointer to palette data
	////////////////////////////////////////////////////////
	struct tCMP_RGB *getPalette()
	{
		return m_data.palette;
	}

	////////////////////////////////////////////////////////
	// Function		:		getLightmap
	//
	// Purpose		:		Returns pointer to lightmap. Array is 64 tables long, 256 entries per table.
	//						Use the formula   EntryId + (TableId * 256) to get correct offset into memory for a value
	//
	// In			:		N/A
	//
	// Out			:		N/A
	//
	// Return		:		Pointer to lightmap
	////////////////////////////////////////////////////////
	unsigned char *getLightmap()
	{
		return m_data.lightmap;
	}

	////////////////////////////////////////////////////////
	// Function		:		getTransparencyTable
	//
	// Purpose		:		Returns pointer to transparency table. Array is 256 tables long, 256 entries per table.
	//						Use the formula   EntryId + (TableId * 256) to get correct offset into memory for a value
	//						Returns 0 if CMP does not contain a transparency table.
	//
	// In			:		N/A
	//
	// Out			:		N/A
	//
	// Return		:		Pointer to trans table, or 0 if it does not exist
	////////////////////////////////////////////////////////
	unsigned char *getTransparencyTable()
	{
		return m_data.transTable;
	}

	////////////////////////////////////////////////////////
	// Function		:		hasTransparencyTable
	//
	// Purpose		:		Determines if the CMP has a transparency table.
	//
	// In			:		N/A
	//
	// Out			:		N/A
	//
	// Return		:		True if the colormap has a transparency table.
	////////////////////////////////////////////////////////
	bool hasTransparencyTable()
	{
		return (m_data.header.transparency != 0);
	}

	////////////////////////////////////////////////////////
	// Function		:		shutdown
	//
	// Purpose		:		Shuts down the colormap object
	//
	// In			:		N/A
	//
	// Out			:		N/A
	//
	// Return		:		N/A
	////////////////////////////////////////////////////////
	void shutdown();

	// Constructor / Destructor
	CCMP(void);
	~CCMP(void);
};
