/* Copyright (c) <2003-2022> <Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely
*/

#ifndef _ND_URDF_FILE_H_
#define _ND_URDF_FILE_H_

class ndModelArticulation;
class ndUrdfFile : public ndClassAlloc
{
	public:
	ndUrdfFile();
	virtual ~ndUrdfFile();

	virtual ndModelArticulation* Import(const char* const fileName);
	virtual void Export(const char* const fileName, ndModelArticulation* const model);

	private:
	void CheckUniqueNames(ndModelArticulation* const model);
	void AddLinks(nd::TiXmlElement* const rootNode, const ndModelArticulation* const model);
	void AddJoints(nd::TiXmlElement* const rootNode, const ndModelArticulation* const model);
};


#endif