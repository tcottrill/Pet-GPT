// -----------------------------------------------------------------------------
// Game Engine Alpha - Generic Module
// Generic component or utility file for the Game Engine Alpha project. This
// file may contain helpers, shared utilities, or subsystems that integrate
// seamlessly with the engine's rendering, audio, and gameplay frameworks.
//
// Integration:
//   This library is part of the **Game Engine Alpha** project and is tightly
//   integrated with its texture management, logging, and math utility systems.
//
// Usage:
//   Include this module where needed. It is designed to work as a building block
//   for engine subsystems such as rendering, input, audio, or game logic.
//
// License:
//   This program is free software: you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation, either version 3 of the License, or
//   (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// -----------------------------------------------------------------------------


#include "fileio.h"
#include "stdlib.h"
#include "stdio.h"
#include "miniz.h"
#include <stdbool.h>
#include <string.h>

int filesz = 0;
size_t uncomp_size = 0;

#pragma warning ( disable:4996 )

int get_last_file_size()
{
	return filesz;
}

size_t get_last_zip_file_size()
{
	return  uncomp_size;
}

int getFileSize(FILE* input)
{
	int fileSizeBytes;

	fseek(input, 0, SEEK_END);
	fileSizeBytes = ftell(input);
	fseek(input, 0, SEEK_SET);
	return fileSizeBytes;
}

unsigned char* load_file(const char* filename)
{
	unsigned char* buf;

	FILE* fd = fopen(filename, "rb");
	if (!fd)
	{
		LOG_INFO("Filed to find file! %s", filename);
		return (0);
	}

	filesz = getFileSize(fd);
	buf = (unsigned char*)malloc(filesz);
	fread(buf, 1, filesz, fd);
	fclose(fd);
	return buf;
}

int save_file(const char* filename, unsigned char* buf, int size)
{
	FILE* fd = fopen(filename, "wb");
	if (!fd) { LOG_INFO("Filed to save file %s.", filename); return 0; }

	fwrite(buf, size, 1, fd);
	fclose(fd);
	return 1;
}

// ToDo: Add a debug clause in front of the logging to disable it
unsigned char* load_generic_zip(const char* archname, const char* filename)
{
	mz_bool status;
	mz_uint file_index = -1;
	mz_zip_archive zip_archive;
	mz_zip_archive_file_stat file_stat;

	unsigned char* buf = NULL;
	int ret = 1; // Zero Means the file didn't load, 1 Means everything is hunky dory. We start with one.

	LOG_INFO("Opening Archive %s", archname);
	// Now try to open the archive.
	memset(&zip_archive, 0, sizeof(zip_archive));
	status = mz_zip_reader_init_file(&zip_archive, archname, 0);
	if (!status) { LOG_INFO("Zip Archive %s not found. (Check your path?)", archname); ret = 0; goto end; }

	// Find the requested file, ignore case
	file_index = mz_zip_reader_locate_file(&zip_archive, filename, 0, 0);
	if (file_index == -1) { LOG_INFO("Error: File %s not found in Zip Archive %s", filename, archname); ret = 0; goto end; }

	// Get information on the current file
	status = mz_zip_reader_file_stat(&zip_archive, file_index, &file_stat);
	if (status != MZ_TRUE) { LOG_INFO("Error reading Zip File Info, it's probably corrupt"); ret = 0; goto end; }

	//Fill in the size in case we need to get it later
	uncomp_size = (size_t)file_stat.m_uncomp_size;

	//Create the unsigned char buffer
	buf = (unsigned char*)malloc(uncomp_size);
	if (!buf) { LOG_INFO("Failed to create char buffer, mem error?"); ret = 0; goto end; }

	// Read (decompress) the file
	status = mz_zip_reader_extract_to_mem(&zip_archive, file_index, buf, uncomp_size, 0);
	if (status != MZ_TRUE) { LOG_INFO("Failed to extract zip file to mem for some weird reason"); ret = 0; goto end; }

end:
	// Close the archive
	LOG_INFO("Closing Archive");
	mz_zip_reader_end(&zip_archive);

	if (ret) { LOG_INFO("Zip file loaded Successfully: %d from archive %s", filename, archname); }
	else { LOG_INFO("Zip file %d in archive %s failed to load!", filename, archname); return 0; }

	return buf;
}

// ToDo: Add a debug clause in front of the logging to disable it
bool saveGenericZip(const char* archname, const char* filename, unsigned char* data)
{
	bool status;

	status = mz_zip_add_mem_to_archive_file_in_place(archname, filename, data, strlen((char*)data) + 1, 0, 0, MZ_BEST_COMPRESSION);
	if (!status)
	{
		LOG_INFO("mz_zip_add_mem_to_archive_file_in_place failed!");
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}