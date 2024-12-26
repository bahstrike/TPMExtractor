// Strike
// Bad Ass Hackers 2005 or something lol

#include ".\cmp.h"

// string routines
#include <string.h>
#include <fstream>

using namespace std;

// New cmp method
void CCMP::newColormap()
{
	// destroy colormap
	shutdown();

	// now set defaults

	// set file verification string
	strncpy(m_data.header.verification, "CMP ", 4);

	// set version
	m_data.header.version = 20;

	// set transparency boolean
	m_data.header.transparency = 0;
}

// Load cmp method
/*bool CCMP::loadColormap(char *szFilename)
{
	bool bResult = false;			// set default return val
	CResFile resfile;

	// try to open the file
	if(resfile.open(szFilename))
	{
		// load the colormap
		bResult = loadColormap(&resfile);

		// close the file
		resfile.shutdown();
	}

	// return result
	return bResult;
}

// Load cmp method
bool CCMP::loadColormap(CResFile *dat)
{
	bool bResult = false;			// set default return val

	// make sure we are destroyed
	shutdown();

	// make sure file was opened
	if(dat->isOpen())
	{
		// try to read the header, palette, and lightmap, and make sure we are loading a CMP
		dat->read(&m_data.header, sizeof(struct tCMP_Header) + (sizeof(struct tCMP_RGB) * 256) + (64 * 256));

		// see if there's a transtable to load
		if(m_data.header.transparency != 0)
		{
			// there is, allocate memory
			m_data.transTable = new unsigned char[256 * 256];

			// load it up
			dat->read(m_data.transTable, 256 * 256);
		}

		// success, set result
		bResult = true;
	}

	// return result
	return bResult;
}*/

// Save cmp method
bool CCMP::saveColormap(char *szFilename)
{
	bool bResult = false;			// set default return val
	ofstream fStream;

	// open file for output
	fStream.open(szFilename, ios::out | ios::binary | ios::trunc);

	// make sure file got trunc'd or created or whatever
	if(fStream.is_open())
	{
		// output data (header, palette, lightmap)
		fStream.write((char*)&m_data, sizeof(struct tCMP_Header) + (sizeof(struct tCMP_RGB) * 256) + (64 * 256));

		// see if we need to write a transtable, and do so if need be
		if(m_data.header.transparency)
			fStream.write((char*)m_data.transTable, 256 * 256);

		// success
		bResult = true;

		// close the file
		fStream.close();
	}

	// return result
	return bResult;
}

// Shutdown method
void CCMP::shutdown()
{
	// destroy the cmp data
	if(m_data.transTable)
		delete [] m_data.transTable;

	// zero out values
	nullify();
}

// Nullify method
void CCMP::nullify()
{
	memset(&m_data, 0, sizeof(struct tCMP_Data));
}

// Constructor
CCMP::CCMP(void)
{
	// zero out important stuff
	nullify();
}

// Destructor
CCMP::~CCMP(void)
{
	// make sure we get shut down
	shutdown();
}
