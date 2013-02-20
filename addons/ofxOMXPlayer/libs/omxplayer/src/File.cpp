#include "linux/PlatformDefs.h"
#include <iostream>
#include <stdio.h>
#include "utils/StdString.h"

#include "File.h"
using namespace XFILE;
using namespace std;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
#ifndef __GNUC__
#pragma warning (disable:4244)
#endif

//*********************************************************************************************
CFile::CFile()
{
	m_pFile = NULL;
	m_flags = 0;
	m_iLength = 0;
	doLoopFile = false;
}

//*********************************************************************************************
CFile::~CFile()
{
  if (m_pFile)
    fclose(m_pFile);
}

//*********************************************************************************************
bool CFile::Open(const CStdString& strFileName, unsigned int flags)
{
  m_flags = flags;
    
  m_pFile = fopen64(strFileName.c_str(), "r");
  if(!m_pFile)
    return false;

  fseeko64(m_pFile, 0, SEEK_END);
  m_iLength = ftello64(m_pFile);
  fseeko64(m_pFile, 0, SEEK_SET);

  return true;
}

bool CFile::OpenForWrite(const CStdString& strFileName, bool bOverWrite)
{
  return false;
}

bool CFile::Exists(const CStdString& strFileName, bool bUseCache /* = true */)
{
  FILE *fp = fopen64(strFileName.c_str(), "r");

  if(!fp)
    return false;

  fclose(fp);

  return true;
}

unsigned int CFile::Read(void *lpBuf, int64_t uiBufSize)
{
	unsigned int ret = 0;

	if(!m_pFile)
	{
		return 0;
	}
	
	if (doLoopFile) 
	{
		if (feof(m_pFile))
		{
			cout << "Looping via doLoopFile in CFile::Read" << endl;
			rewind(m_pFile);
		}
	}
	

	ret = fread(lpBuf, 1, uiBufSize, m_pFile);
		
	return ret;
}

//*********************************************************************************************
void CFile::Close()
{
  if(m_pFile)
    fclose(m_pFile);
  m_pFile = NULL;
}

//*********************************************************************************************
int64_t CFile::Seek(int64_t iFilePosition, int iWhence)
{
  if (!m_pFile)
    return -1;

  return fseeko64(m_pFile, iFilePosition, iWhence);;
}

//*********************************************************************************************
int64_t CFile::GetLength()
{
  return m_iLength;
}

//*********************************************************************************************
int64_t CFile::GetPosition()
{
  if (!m_pFile)
    return -1;

  return ftello64(m_pFile);
}

//*********************************************************************************************
int CFile::Write(const void* lpBuf, int64_t uiBufSize)
{
  return -1;
}

int CFile::IoControl(EIoControl request, void* param)
{
  if(request == IOCTRL_SEEK_POSSIBLE && m_pFile)
  {
    struct stat st;
    if (fstat(fileno(m_pFile), &st) == 0)
    {
      return !S_ISFIFO(st.st_mode);
    }
  }

  return -1;
}
