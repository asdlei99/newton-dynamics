/* Copyright (c) <2003-2019> <Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely
*/
#ifndef __DEMO_MAIN_FRAME_H__
#define __DEMO_MAIN_FRAME_H__

#include "ndSandboxStdafx.h"
//#include "ShaderPrograms.h"

struct GLFWwindow;
struct ImDrawData;

//class DemoMesh;
//class DemoEntity;
//class DemoCamera;
//class DemoMeshInterface;
//class DemoCameraManager;

//class DemoEntityManager: public dList <DemoEntity*>
class ndDemoEntityManager
{
#if 0
	public:
	typedef void (*LaunchSDKDemoCallback) (DemoEntityManager* const scene);
	typedef void (*RenderGuiHelpCallback) (DemoEntityManager* const manager, void* const context);
	typedef void(*UpdateCameraCallback) (DemoEntityManager* const manager, void* const context, dFloat32 timestep);

	class TransparentMesh
	{
		public: 
		TransparentMesh()
			:m_matrix(dGetIdentityMatrix())
			,m_mesh(NULL)
		{
		}

		TransparentMesh(const dMatrix& matrix, const DemoMesh* const mesh)
			:m_matrix(matrix)
			,m_mesh(mesh)
		{
		}

		dMatrix m_matrix;
		const DemoMesh* m_mesh;
	};

	class TransparentHeap: public dUpHeap <TransparentMesh, dFloat32>
	{
		public:
		TransparentHeap()
			:dUpHeap <TransparentMesh, dFloat32>(2048)
		{
		}
	};

	class SDKDemos
	{
		public:
		const char *m_name;
		const char *m_description;
		LaunchSDKDemoCallback m_launchDemoCallback;
	};

	class ButtonKey
	{
		public:
		ButtonKey (bool initialState);
		int UpdateTrigger (bool triggerValue);
		int UpdatePushButton (bool triggerValue);
		int GetPushButtonState() const { return m_state ? 1 : 0;}

		private:
		bool m_state;
		bool m_memory0;
		bool m_memory1;
	};

	class EntityDictionary: public dTree<DemoEntity*, dScene::dTreeNode*>
	{
	};

	

	void Lock(unsigned& atomicLock);
	void Unlock(unsigned& atomicLock);

	int GetWidth() const;
	int GetHeight() const;

	NewtonWorld* GetNewton() const;
	void CreateSkyBox();

	void ResetTimer();
	void LoadScene (const char* const name);
	void RemoveEntity (DemoEntity* const ent);
	void RemoveEntity (dListNode* const entNode);

	void ImportPLYfile (const char* const name);

	DemoCamera* GetCamera() const;
	bool GetMousePosition (int& posX, int& posY) const;
	void SetCameraMatrix (const dQuaternion& rotation, const dVector& position);

	void PushTransparentMesh (const DemoMeshInterface* const mesh); 
	void SetUpdateCameraFunction(UpdateCameraCallback callback, void* const context);
	void Set2DDisplayRenderFunction (RenderGuiHelpCallback helpCallback, RenderGuiHelpCallback UIcallback, void* const context);

	bool IsShiftKeyDown () const;
	bool IsControlKeyDown () const;
	bool GetKeyState(int key) const;
	int GetJoystickAxis (dFloat32* const axisValues, int maxAxis = 8) const;
	int GetJoystickButtons (char* const axisbuttons, int maxButton = 32) const;

	void SerializedPhysicScene(const char* const name);
	void DeserializedPhysicScene(const char* const name);

	static void SerializeFile (void* const serializeHandle, const void* const buffer, int size);
	static void DeserializeFile (void* const serializeHandle, void* const buffer, int size);
	static void BodySerialization (NewtonBody* const body, void* const userData, NewtonSerializeCallback serializecallback, void* const serializeHandle);
	static void BodyDeserialization (NewtonBody* const body, void* const userData, NewtonDeserializeCallback serializecallback, void* const serializeHandle);

	static void OnCreateContact(const NewtonWorld* const world, NewtonJoint* const contact);
	static void OnDestroyContact(const NewtonWorld* const world, NewtonJoint* const contact);

	bool GetCaptured () const;
	bool GetMouseKeyState (int button ) const;
	int Print (const dVector& color, const char *fmt, ... ) const;
	int GetDebugDisplay() const;
	void SetDebugDisplay(int mode) const;

	const ShaderPrograms& GetShaderCache() const;  
	

	private:

	void RenderStats();
	
	void Cleanup();

	//void RenderUI();
	void RenderScene();
	
	void UpdatePhysics(dFloat32 timestep);
	dFloat32 CalculateInteplationParam () const;

	void CalculateFPS(dFloat32 timestep);
	
	void ShowMainMenuBar();
	void LoadVisualScene(dScene* const scene, EntityDictionary& dictionary);

	void ToggleProfiler();
	static void PostUpdateCallback(const NewtonWorld* const world, dFloat32 timestep);

	void ApplyMenuOptions();
	void LoadDemo(int menu);
	
	DemoEntity* m_sky;
	NewtonWorld* m_world;
	DemoCameraManager* m_cameraManager;
	ShaderPrograms m_shadeCache;
	void* m_renderUIContext;
	void* m_updateCameraContext;
	RenderGuiHelpCallback m_renderDemoGUI;
	RenderGuiHelpCallback m_renderHelpMenus;
	UpdateCameraCallback m_updateCamera;

	unsigned64 m_microsecunds;
	TransparentHeap m_tranparentHeap;

	int m_currentScene;
	int m_lastCurrentScene;
	int m_framesCount;
	int m_physicsFramesCount;
	int m_currentPlugin;
	dFloat32 m_fps;
	dFloat32 m_timestepAcc;
	dFloat32 m_currentListenerTimestep;
	dFloat32 m_mainThreadPhysicsTime;
	dFloat32 m_mainThreadPhysicsTimeAcc;

	int m_solverPasses;
	int m_solverSubSteps;
	int m_broadPhaseType;
	int m_workerThreads;
	int m_debugDisplayMode;
	int m_collisionDisplayMode;
	
	bool m_showUI;
	bool m_showAABB;
	bool m_showStats;
	
	bool m_autoSleepMode;
	bool m_hideVisualMeshes;
	bool m_showNormalForces;
	bool m_showCenterOfMass;
	bool m_showBodyFrame;
	bool m_updateMenuOptions;
	bool m_showContactPoints;
	bool m_showJointDebugInfo;
	bool m_showListenersDebugInfo;
	bool m_showCollidingFaces;
	bool m_suspendPhysicsUpdate;
	bool m_asynchronousPhysicsUpdate;
	bool m_solveLargeIslandInParallel;
	bool m_showRaycastHit;

	unsigned m_profilerMode;
	unsigned m_contactLock;
	unsigned m_deleteLock;
	dList<NewtonJoint*> m_contactList;

	static SDKDemos m_demosSelection[];
	friend class DemoEntityListener;
	friend class DemoListenerManager;
#endif

	public:
	ndDemoEntityManager();
	~ndDemoEntityManager();

	void Run();

	private:
	static void RenderDrawListsCallback(ImDrawData* const draw_data);
	static void ErrorCallback(int error, const char* const description);
	static void CharCallback(GLFWwindow* window, unsigned int ch);
	static void KeyCallback(GLFWwindow* const window, int key, int, int action, int mods);
	static void CursorposCallback(GLFWwindow* const window, double x, double y);
	static void MouseScrollCallback(GLFWwindow* const window, double x, double y);
	static void MouseButtonCallback(GLFWwindow* const window, int button, int action, int mods);

	void LoadFont();
	void BeginFrame();
	

	GLFWwindow* m_mainFrame;

	int	m_defaultFont;
	bool m_hasJoytick;
	bool m_mousePressed[3];
};

#if 0
inline NewtonWorld* DemoEntityManager::GetNewton() const
{
	return m_world;
}

// for simplicity we are not going to run the demo in a separate thread at this time
// this confuses many user int thinking it is more complex than it really is  
inline void DemoEntityManager::Lock(unsigned& atomicLock)
{
	while (NewtonAtomicSwap((int*)&atomicLock, 1)) {
		NewtonYield();
	}
}

inline void DemoEntityManager::Unlock(unsigned& atomicLock)
{
	NewtonAtomicSwap((int*)&atomicLock, 0);
}

inline int DemoEntityManager::GetWidth() const 
{ 
	ImGuiIO& io = ImGui::GetIO();
	return (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x);
}

inline int DemoEntityManager::GetHeight() const 
{ 
	ImGuiIO& io = ImGui::GetIO();
	return (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y);
}

inline int DemoEntityManager::GetDebugDisplay() const
{
	dAssert (0);
	return 0;
}

inline void DemoEntityManager::SetDebugDisplay(int mode) const
{
	dAssert (0);
}

inline const ShaderPrograms& DemoEntityManager::GetShaderCache() const
{
	return m_shadeCache;
}
#endif

#endif