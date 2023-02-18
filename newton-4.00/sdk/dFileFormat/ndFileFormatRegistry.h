/* Copyright (c) <2003-2022> <Julio Jerez, Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 
* 3. This notice may not be removed or altered from any source distribution.
*/

#ifndef _ND_FILE_FORMAT_REGISTRY_H__
#define _ND_FILE_FORMAT_REGISTRY_H__

#include "ndFileFormatStdafx.h"
#include "ndTinyXmlGlue.h"

class ndFileFormatRegistry : public ndClassAlloc
{
	protected:	
	ndFileFormatRegistry(const char* const className);
	virtual ~ndFileFormatRegistry();
	
	public:
	virtual void SaveBody(nd::TiXmlElement* const parentNode, const ndBody* const body);
	virtual void SaveNotify(nd::TiXmlElement* const parentNode, const ndBodyNotify* const notify);
	virtual void SaveCollision(nd::TiXmlElement* const parentNode, const ndShapeInstance* const collision);

	static ndFileFormatRegistry* GetHandler(const char* const className);

	private:
	static void Init();
	static ndFixSizeArray<ndFileFormatRegistry*, 256> m_registry;

	ndUnsigned64 m_hash;
	friend class ndFileFormat;
};

inline void ndFileFormatRegistry::SaveBody(nd::TiXmlElement* const, const ndBody* const)
{
	ndAssert(0);
}

inline void ndFileFormatRegistry::SaveNotify(nd::TiXmlElement* const, const ndBodyNotify* const)
{
	ndAssert(0);
}

inline void ndFileFormatRegistry::SaveCollision(nd::TiXmlElement* const, const ndShapeInstance* const)
{
	ndAssert(0);
}
#endif 

