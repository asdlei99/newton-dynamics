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

#include "ndSandboxStdafx.h"
#include "ndTargaToOpenGl.h"
#include "ndDemoMeshInterface.h"

ndDemoSubMeshMaterial::ndDemoSubMeshMaterial()
	:m_ambient(0.8f, 0.8f, 0.8f, 1.0f)
	,m_diffuse(0.8f, 0.8f, 0.8f, 1.0f)
	,m_specular(1.0f, 1.0f, 1.0f, 1.0f)
	,m_opacity(1.0f)
	,m_shiness(100.0f)
	,m_textureHandle(0)
{
	m_textureName[0] = 0;
}

ndDemoSubMeshMaterial::~ndDemoSubMeshMaterial()
{
	if (m_textureHandle)
	{
		ReleaseTexture(m_textureHandle);
	}
}

GLint ndDemoSubMeshMaterial::GetTexture() const
{
	return m_textureHandle;
}

void ndDemoSubMeshMaterial::SetTexture(GLint textureHandle)
{
	if (textureHandle != m_textureHandle)
	{
		if (m_textureHandle)
		{
			ReleaseTexture(m_textureHandle);
		}
		AddTextureRef(textureHandle);
		m_textureHandle = textureHandle;
	}
}

ndDemoMeshInterface::ndDemoMeshInterface()
	:ndClassAlloc()
	,m_refCount(1)
	,m_name()
	,m_isVisible(true)
{
}

ndDemoMeshInterface::~ndDemoMeshInterface()
{
	//ndAssert(m_refCount == 0);
}

ndDemoMeshInterface* ndDemoMeshInterface::AddRef()
{
	m_refCount++;
	return this;
}

ndInt32 ndDemoMeshInterface::Release()
{
	//ndAssert(0);
	m_refCount--;
	if (m_refCount == 0)
	{
		delete this;
	}
	return m_refCount;
}

const ndString& ndDemoMeshInterface::GetName () const
{
	return m_name;
}

bool ndDemoMeshInterface::GetVisible () const
{
	return m_isVisible;
}

void ndDemoMeshInterface::SetVisible (bool visibilityFlag)
{
	m_isVisible = visibilityFlag;
}

ndDemoSubMesh::ndDemoSubMesh ()
	:m_material()
	,m_indexCount(0)
	,m_segmentStart(0)
	,m_hasTranparency(false)
{
}

ndDemoSubMesh::~ndDemoSubMesh ()
{
}

void ndDemoSubMesh::SetOpacity(ndFloat32 opacity)
{
	m_material.m_opacity = GLfloat(opacity);
	m_material.m_ambient[3] = GLfloat(opacity);
	m_material.m_diffuse[3] = GLfloat(opacity);
	m_material.m_specular[3] = GLfloat(opacity);
	m_hasTranparency = (opacity <= 0.99f) ? true : false;
}

