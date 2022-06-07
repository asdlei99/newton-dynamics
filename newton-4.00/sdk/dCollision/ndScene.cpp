/* Copyright (c) <2003-2021> <Julio Jerez, Newton Game Dynamics>
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

#include "ndCoreStdafx.h"
#include "ndCollisionStdafx.h"
#include "ndScene.h"
#include "ndShapeNull.h"
#include "ndBodyNotify.h"
#include "ndShapeCompound.h"
#include "ndBodyKinematic.h"
#include "ndContactNotify.h"
#include "ndContactSolver.h"
#include "ndRayCastNotify.h"
#include "ndConvexCastNotify.h"
#include "ndBodyTriggerVolume.h"
#include "ndBodiesInAabbNotify.h"
#include "ndJointBilateralConstraint.h"
#include "ndShapeStaticProceduralMesh.h"

#define D_CONTACT_DELAY_FRAMES		4
#define D_NARROW_PHASE_DIST			ndFloat32 (0.2f)
#define D_CONTACT_TRANSLATION_ERROR	ndFloat32 (1.0e-3f)
#define D_CONTACT_ANGULAR_ERROR		(ndFloat32 (0.25f * ndDegreeToRad))

ndVector ndScene::m_velocTol(ndFloat32(1.0e-16f));
ndVector ndScene::m_angularContactError2(D_CONTACT_ANGULAR_ERROR * D_CONTACT_ANGULAR_ERROR);
ndVector ndScene::m_linearContactError2(D_CONTACT_TRANSLATION_ERROR * D_CONTACT_TRANSLATION_ERROR);

ndScene::ndFitnessList::ndFitnessList()
	:ndList <ndSceneTreeNode*, ndContainersFreeListAlloc<ndSceneTreeNode*>>()
	,m_currentCost(ndFloat32(0.0f))
	,m_currentNode(nullptr)
	,m_index(0)
{
}

ndScene::ndFitnessList::ndFitnessList(const ndFitnessList& src)
	:ndList <ndSceneTreeNode*, ndContainersFreeListAlloc<ndSceneTreeNode*>>()
	,m_currentCost(src.m_currentCost)
	,m_currentNode(src.m_currentNode)
	,m_index(src.m_index)
{
	ndFitnessList* const stealData = (ndFitnessList*)&src;
	ndNode* nextNode;
	for (ndNode* node = stealData->GetFirst(); node; node = node = nextNode)
	{
		nextNode = node->GetNext();
		stealData->Unlink(node);
		Append(node);
	}
}

void ndScene::ndFitnessList::AddNode(ndSceneTreeNode* const node)
{
	node->m_fitnessNode = Append(node);
}

void ndScene::ndFitnessList::RemoveNode(ndSceneTreeNode* const node)
{
	dAssert(node->m_fitnessNode);
	if (node->m_fitnessNode == m_currentNode)
	{
		m_currentNode = node->m_fitnessNode->GetNext();
	}
	Remove(node->m_fitnessNode);
	node->m_fitnessNode = nullptr;
}

ndFloat64 ndScene::ndFitnessList::TotalCost() const
{
	D_TRACKTIME();
	ndFloat64 cost = ndFloat32(0.0f);
	for (ndNode* node = GetFirst(); node; node = node->GetNext()) {
		ndSceneNode* const box = node->GetInfo();
		cost += box->m_surfaceArea;
	}
	return cost;
}
	
ndScene::ndScene()
	:ndThreadPool("newtonWorker")
	,m_bodyList()
	,m_contactArray()
	,m_scratchBuffer(1024 * sizeof (void*))
	,m_sceneBodyArray(1024)
	,m_activeConstraintArray(1024)
	,m_specialUpdateList()
	,m_backgroundThread()
	,m_lock()
	,m_rootNode(nullptr)
	,m_sentinelBody(nullptr)
	,m_contactNotifyCallback(new ndContactNotify())
	,m_treeEntropy(ndFloat32(0.0f))
	,m_fitness()
	,m_timestep(ndFloat32 (0.0f))
	,m_lru(D_CONTACT_DELAY_FRAMES)
	,m_forceBalanceSceneCounter(0)
	,m_bodyListChanged(1)
	,m_forceBalanceScene(0)
{
	m_sentinelBody = new ndBodySentinel;
	m_contactNotifyCallback->m_scene = this;

	#ifdef D_NEW_SCENE
	for (ndInt32 i = 0; i < D_MAX_THREADS_COUNT; ++i)
	{
		m_particalNewPairs[i].Resize(256);
	}
	#endif	
}

ndScene::ndScene(const ndScene& src)
	:ndThreadPool("newtonWorker")
	,m_bodyList(src.m_bodyList)
	,m_contactArray(src.m_contactArray)
	,m_scratchBuffer()
	,m_sceneBodyArray()
	,m_activeConstraintArray()
	,m_specialUpdateList()
	,m_backgroundThread()
	,m_lock()
	,m_rootNode(nullptr)
	,m_sentinelBody(nullptr)
	,m_contactNotifyCallback(nullptr)
	,m_treeEntropy(ndFloat32(0.0f))
	,m_fitness(src.m_fitness)
	,m_timestep(ndFloat32(0.0f))
	,m_lru(src.m_lru)
	,m_forceBalanceSceneCounter(src.m_forceBalanceSceneCounter)
	,m_bodyListChanged(src.m_bodyListChanged)
	,m_forceBalanceScene(src.m_forceBalanceScene)
{
	ndScene* const stealData = (ndScene*)&src;

	SetThreadCount(src.GetThreadCount());
	m_backgroundThread.SetThreadCount(m_backgroundThread.GetThreadCount());

	m_scratchBuffer.Swap(stealData->m_scratchBuffer);
	m_sceneBodyArray.Swap(stealData->m_sceneBodyArray);
	m_activeConstraintArray.Swap(stealData->m_activeConstraintArray);

	ndSwap(m_rootNode, stealData->m_rootNode);
	ndSwap(m_sentinelBody, stealData->m_sentinelBody);
	ndSwap(m_contactNotifyCallback, stealData->m_contactNotifyCallback);
	m_contactNotifyCallback->m_scene = this;

	ndList<ndBodyKinematic*>::ndNode* nextNode;
	for (ndList<ndBodyKinematic*>::ndNode* node = stealData->m_specialUpdateList.GetFirst(); node; node = nextNode)
	{
		nextNode = node->GetNext();
		stealData->m_specialUpdateList.Unlink(node);
		m_specialUpdateList.Append(node);
	}

	for (ndBodyList::ndNode* node = m_bodyList.GetFirst(); node; node = node->GetNext())
	{
		ndBodyKinematic* const body = node->GetInfo();
		body->m_sceneForceUpdate = 1;
		ndSceneBodyNode* const sceneNode = body->GetSceneBodyNode();
		if (sceneNode)
		{
			body->SetSceneNodes(this, node);
		}
		dAssert (body->GetContactMap().SanityCheck());
	}

	#ifdef D_NEW_SCENE
	for (ndInt32 i = 0; i < D_MAX_THREADS_COUNT; ++i)
	{
		m_particalNewPairs[i].Resize(256);
	}
	#endif	
}

ndScene::~ndScene()
{
	Cleanup();
	Finish();
	if (m_contactNotifyCallback)
	{
		delete m_contactNotifyCallback;
	}
	ndFreeListAlloc::Flush();
}

void ndScene::Sync()
{
	ndThreadPool::Sync();
}

void ndScene::CollisionOnlyUpdate()
{
	D_TRACKTIME();
	Begin();
	m_lru = m_lru + 1;
	InitBodyArray();
	BalanceScene();
	FindCollidingPairs();
	CalculateContacts();
	End();
}

void ndScene::ThreadFunction()
{
	D_TRACKTIME();
	CollisionOnlyUpdate();
}

void ndScene::Begin()
{
	ndThreadPool::Begin();
}

void ndScene::End()
{
	ndThreadPool::End();
}


void ndScene::Update(ndFloat32 timestep)
{
	// wait until previous update complete.
	Sync();

	// save time state for use by the update callback
	m_timestep = timestep;

	// update the next frame asynchronous 
	TickOne();
}

ndContactNotify* ndScene::GetContactNotify() const
{
	return m_contactNotifyCallback;
}

void ndScene::SetContactNotify(ndContactNotify* const notify)
{
	dAssert(m_contactNotifyCallback);
	delete m_contactNotifyCallback;
	
	if (notify)
	{
		m_contactNotifyCallback = notify;
	}
	else
	{
		m_contactNotifyCallback = new ndContactNotify();
	}
	m_contactNotifyCallback->m_scene = this;
}

void ndScene::DebugScene(ndSceneTreeNotiFy* const notify)
{
	for (ndFitnessList::ndNode* node = m_fitness.GetFirst(); node; node = node->GetNext())
	{
		if (node->GetInfo()->GetLeft()->GetAsSceneBodyNode())
		{
			notify->OnDebugNode(node->GetInfo()->GetLeft());
		}
		if (node->GetInfo()->GetRight()->GetAsSceneBodyNode())
		{
			notify->OnDebugNode(node->GetInfo()->GetRight());
		}
	}
}

bool ndScene::AddBody(ndBodyKinematic* const body)
{
	if ((body->m_scene == nullptr) && (body->m_sceneNode == nullptr))
	{
		m_bodyListChanged = 1;
		ndBodyList::ndNode* const node = m_bodyList.Append(body);
		body->SetSceneNodes(this, node);
		m_contactNotifyCallback->OnBodyAdded(body);

		body->UpdateCollisionMatrix();
		ndSceneBodyNode* const bodyNode = new ndSceneBodyNode(body);
		AddNode(bodyNode);

		if (body->GetAsBodyTriggerVolume() || body->GetAsBodyPlayerCapsule())
		{
			body->m_spetialUpdateNode = m_specialUpdateList.Append(body);
		}

		return true;
	}
	return false;
}

bool ndScene::RemoveBody(ndBodyKinematic* const body)
{
	ndSceneBodyNode* const node = body->GetSceneBodyNode();
	if (node)
	{
		RemoveNode(node);
	}

	ndBodyKinematic::ndContactMap& contactMap = body->GetContactMap();
	while (contactMap.GetRoot())
	{
		ndContact* const contact = contactMap.GetRoot()->GetInfo();
		m_contactArray.DeleteContact(contact);
	}

	if (body->m_scene && body->m_sceneNode)
	{
		m_bodyListChanged = 1;
		if (body->GetAsBodyTriggerVolume() || body->GetAsBodyPlayerCapsule())
		{
			m_specialUpdateList.Remove(body->m_spetialUpdateNode);
			body->m_spetialUpdateNode = nullptr;
		}

		m_bodyList.Remove(body->m_sceneNode);
		body->SetSceneNodes(nullptr, nullptr);
		m_contactNotifyCallback->OnBodyRemoved(body);
		return true;
	}
	return false;
}

ndSceneTreeNode* ndScene::InsertNode(ndSceneNode* const root, ndSceneNode* const node)
{
	ndVector p0;
	ndVector p1;

	ndSceneNode* sibling = root;
	ndFloat32 surfaceArea = CalculateSurfaceArea(node, sibling, p0, p1);
	while (!sibling->GetAsSceneBodyNode() && (surfaceArea >= sibling->m_surfaceArea))
	{
		sibling->m_minBox = p0;
		sibling->m_maxBox = p1;
		sibling->m_surfaceArea = surfaceArea;
	
		ndVector leftP0;
		ndVector leftP1;
		ndFloat32 leftSurfaceArea = CalculateSurfaceArea(node, sibling->GetLeft(), leftP0, leftP1);
		
		ndVector rightP0;
		ndVector rightP1;
		ndFloat32 rightSurfaceArea = CalculateSurfaceArea(node, sibling->GetRight(), rightP0, rightP1);
	
		if (leftSurfaceArea < rightSurfaceArea) 
		{
			p0 = leftP0;
			p1 = leftP1;
			sibling = sibling->GetLeft();
			surfaceArea = leftSurfaceArea;
		}
		else 
		{
			p0 = rightP0;
			p1 = rightP1;
			sibling = sibling->GetRight();
			surfaceArea = rightSurfaceArea;
		}
	}
	
	ndSceneTreeNode* const parent = new ndSceneTreeNode(sibling, node);
	return parent;
}

void ndScene::RotateLeft(ndSceneTreeNode* const node, ndSceneNode** const root)
{
	ndVector cost1P0;
	ndVector cost1P1;

	ndSceneTreeNode* const parent = (ndSceneTreeNode*)node->m_parent;
	dAssert(parent && !parent->GetAsSceneBodyNode());
	ndFloat32 cost1 = CalculateSurfaceArea(node->m_left, parent->m_left, cost1P0, cost1P1);

	ndVector cost2P0;
	ndVector cost2P1;
	ndFloat32 cost2 = CalculateSurfaceArea(node->m_right, parent->m_left, cost2P0, cost2P1);

	ndFloat32 cost0 = node->m_surfaceArea;
	if ((cost1 <= cost0) && (cost1 <= cost2)) 
	{
		node->m_minBox = parent->m_minBox;
		node->m_maxBox = parent->m_maxBox;
		node->m_surfaceArea = parent->m_surfaceArea;

		ndSceneTreeNode* const grandParent = (ndSceneTreeNode*)parent->m_parent;
		if (grandParent) 
		{
			if (grandParent->m_left == parent) 
			{
				grandParent->m_left = node;
			}
			else 
			{
				dAssert(grandParent->m_right == parent);
				grandParent->m_right = node;
			}
		}
		else 
		{
			(*root) = node;
		}

		node->m_parent = parent->m_parent;
		parent->m_parent = node;
		node->m_left->m_parent = parent;
		parent->m_right = node->m_left;
		node->m_left = parent;

		parent->m_minBox = cost1P0;
		parent->m_maxBox = cost1P1;
		parent->m_surfaceArea = cost1;
	}
	else if ((cost2 <= cost0) && (cost2 <= cost1)) 
	{
		node->m_minBox = parent->m_minBox;
		node->m_maxBox = parent->m_maxBox;
		node->m_surfaceArea = parent->m_surfaceArea;

		ndSceneTreeNode* const grandParent = (ndSceneTreeNode*)parent->m_parent;
		if (grandParent) 
		{
			if (grandParent->m_left == parent) 
			{
				grandParent->m_left = node;
			}
			else 
			{
				dAssert(grandParent->m_right == parent);
				grandParent->m_right = node;
			}
		}
		else 
		{
			(*root) = node;
		}

		node->m_parent = parent->m_parent;
		parent->m_parent = node;
		node->m_right->m_parent = parent;
		parent->m_right = node->m_right;
		node->m_right = parent;

		parent->m_minBox = cost2P0;
		parent->m_maxBox = cost2P1;
		parent->m_surfaceArea = cost2;
	}
}

void ndScene::RotateRight(ndSceneTreeNode* const node, ndSceneNode** const root)
{
	ndVector cost1P0;
	ndVector cost1P1;

	ndSceneTreeNode* const parent = (ndSceneTreeNode*)node->m_parent;
	dAssert(parent && !parent->GetAsSceneBodyNode());

	ndFloat32 cost1 = CalculateSurfaceArea(node->m_right, parent->m_right, cost1P0, cost1P1);

	ndVector cost2P0;
	ndVector cost2P1;
	ndFloat32 cost2 = CalculateSurfaceArea(node->m_left, parent->m_right, cost2P0, cost2P1);

	ndFloat32 cost0 = node->m_surfaceArea;
	if ((cost1 <= cost0) && (cost1 <= cost2)) 
	{
		node->m_minBox = parent->m_minBox;
		node->m_maxBox = parent->m_maxBox;
		node->m_surfaceArea = parent->m_surfaceArea;

		ndSceneTreeNode* const grandParent = (ndSceneTreeNode*)parent->m_parent;
		if (grandParent) 
		{
			dAssert(!grandParent->GetAsSceneBodyNode());
			if (grandParent->m_left == parent) 
			{
				grandParent->m_left = node;
			}
			else 
			{
				dAssert(grandParent->m_right == parent);
				grandParent->m_right = node;
			}
		}
		else 
		{
			(*root) = node;
		}

		node->m_parent = parent->m_parent;
		parent->m_parent = node;
		node->m_right->m_parent = parent;
		parent->m_left = node->m_right;
		node->m_right = parent;
		parent->m_minBox = cost1P0;
		parent->m_maxBox = cost1P1;
		parent->m_surfaceArea = cost1;

	}
	else if ((cost2 <= cost0) && (cost2 <= cost1)) 
	{
		node->m_minBox = parent->m_minBox;
		node->m_maxBox = parent->m_maxBox;
		node->m_surfaceArea = parent->m_surfaceArea;

		ndSceneTreeNode* const grandParent = (ndSceneTreeNode*)parent->m_parent;
		if (parent->m_parent) 
		{
			if (grandParent->m_left == parent) 
			{
				grandParent->m_left = node;
			}
			else 
			{
				dAssert(grandParent->m_right == parent);
				grandParent->m_right = node;
			}
		}
		else 
		{
			(*root) = node;
		}

		node->m_parent = parent->m_parent;
		parent->m_parent = node;
		node->m_left->m_parent = parent;
		parent->m_left = node->m_left;
		node->m_left = parent;

		parent->m_minBox = cost2P0;
		parent->m_maxBox = cost2P1;
		parent->m_surfaceArea = cost2;
	}
}

void ndScene::ImproveNodeFitness(ndSceneTreeNode* const node, ndSceneNode** const root)
{
	dAssert(node->GetLeft());
	dAssert(node->GetRight());

	ndSceneNode* const parent = node->m_parent;
	if (parent && parent->m_parent) 
	{
		dAssert(!parent->GetAsSceneBodyNode());
		if (parent->GetLeft() == node) 
		{
			RotateRight(node, root);
		}
		else 
		{
			RotateLeft(node, root);
		}
	}
	dAssert(!m_rootNode->m_parent);
}

ndFloat64 ndScene::ReduceEntropy(ndFitnessList& fitness, ndSceneNode** const root)
{
	D_TRACKTIME();

	if (!fitness.m_currentNode) 
	{
		fitness.m_currentCost = fitness.TotalCost();
		fitness.m_currentNode = fitness.GetFirst();
	}
	else
	{
		ndInt32 count = 0;
		ndFitnessList::ndNode* node = fitness.m_currentNode;
		for ( ;node && count < 64; node = node->GetNext())
		{
			count++;
			ImproveNodeFitness(node->GetInfo(), root);
		}
		fitness.m_currentNode = node;
	}
	return fitness.m_currentCost;
}

ndSceneNode* ndScene::BuildTopDown(ndSceneNode** const leafArray, ndInt32 firstBox, ndInt32 lastBox, ndFitnessList::ndNode** const nextNode)
{
	class ndBlockSegment
	{
		public:
		ndSceneTreeNode* m_node;
		ndInt32 m_start;
		ndInt32 m_count;
	};

	class ndBoxStats
	{
		public:
		ndVector m_median;
		ndVector m_varian;
		ndVector m_minP;
		ndVector m_maxP;
	};

	ndInt32 stack = 1;
	ndBoxStats boxStats[D_MAX_THREADS_COUNT];
	ndBlockSegment stackPool[D_SCENE_MAX_STACK_DEPTH];

	dAssert(firstBox >= 0);
	dAssert(lastBox >= 0);

	if (lastBox == firstBox)
	{
		return leafArray[firstBox];
	}

	D_TRACKTIME();
	ndSceneTreeNode* const root = (*nextNode)->GetInfo();
	root->m_left = nullptr;
	root->m_right = nullptr;
	root->m_parent = nullptr;
	*nextNode = (*nextNode)->GetNext();
	
	stackPool[0].m_node = root;
	stackPool[0].m_start = firstBox;
	stackPool[0].m_count = lastBox - firstBox + 1;

	m_sceneBodyArray.SetCount(stackPool[0].m_count);
	ndSceneNode** const tmpBuffer = (ndSceneNode**) &m_sceneBodyArray[0];

	while (stack)
	{
		stack--;
		ndBlockSegment block = stackPool[stack];

		if (block.m_count == 2)
		{
			ndSceneTreeNode* const node = block.m_node;
			dAssert(node->m_left == nullptr);
			dAssert(node->m_right == nullptr);

			node->m_left = leafArray[block.m_start];
			node->m_left->m_parent = node;

			node->m_right = leafArray[block.m_start + 1];
			node->m_right->m_parent = node;

			const ndVector minP(node->m_left->m_minBox.GetMin(node->m_right->m_minBox));
			const ndVector maxP(node->m_left->m_maxBox.GetMax(node->m_right->m_maxBox));
			node->SetAabb(minP, maxP);
		}
		else
		{
			ndInt32 boxCount = block.m_count;
			ndSceneNode** const boxArray = &leafArray[block.m_start];

			auto CalculateBoxStats = ndMakeObject::ndFunction([this, boxArray, boxCount, tmpBuffer, &boxStats](ndInt32 threadIndex, ndInt32 threadCount)
			{
				D_TRACKTIME();

				ndVector median(ndVector::m_zero);
				ndVector varian(ndVector::m_zero);
				ndVector minP(ndFloat32(1.0e15f));
				ndVector maxP(ndFloat32(-1.0e15f));

				const ndStartEnd startEnd(boxCount, threadIndex, threadCount);
				for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
				{
					ndSceneNode* const node = boxArray[i];
					tmpBuffer[i] = node;

					dAssert(node->GetAsSceneBodyNode());
					minP = minP.GetMin(node->m_minBox);
					maxP = maxP.GetMax(node->m_maxBox);
					ndVector p(ndVector::m_half * (node->m_minBox + node->m_maxBox));
					median += p;
					varian += p * p;
				}
				boxStats[threadIndex].m_median = median;
				boxStats[threadIndex].m_varian = varian;
				boxStats[threadIndex].m_minP = minP;
				boxStats[threadIndex].m_maxP = maxP;
			});
			ParallelExecute(CalculateBoxStats);

			ndVector minP(boxStats[0].m_minP);
			ndVector maxP(boxStats[0].m_maxP);
			ndVector median(boxStats[0].m_median);
			ndVector varian(boxStats[0].m_varian);

			const ndInt32 threadCount = GetThreadCount();
			for (ndInt32 i = 1; i < threadCount; ++i)
			{
				minP = minP.GetMin(boxStats[i].m_minP);
				maxP = maxP.GetMax(boxStats[i].m_maxP);
				median += boxStats[i].m_median;
				varian += boxStats[i].m_varian;
			}

			block.m_node->SetAabb(minP, maxP);
			varian = varian.Scale(ndFloat32(boxCount)) - median * median;

			ndInt32 index = 0;
			ndFloat32 maxVarian = ndFloat32(-1.0e15f);
			for (ndInt32 i = 0; i < 3; ++i)
			{
				if (varian[i] > maxVarian)
				{
					index = i;
					maxVarian = varian[i];
				}
			}

			ndUnsigned32 prefixScan[32];
			if (maxVarian > ndFloat32(1.0e-3f))
			{
				class ndSpliteTest
				{
					public:
					ndSpliteTest(const ndFloat32 dist, ndInt32 index)
						:m_dist(dist)
						, m_index(index)
					{
					}

					ndFloat32 m_dist;
					ndInt32 m_index;
				};
				ndVector center = median.Scale(ndFloat32(1.0f) / ndFloat32(boxCount));
				ndFloat32 test = center[index];

				class ndEvaluateKey
				{
					public:
					ndEvaluateKey(void* const context)
					{
						ndSpliteTest* const test = (ndSpliteTest*)context;
						m_dist = test->m_dist;
						m_index = test->m_index;
					}

					ndUnsigned32 GetKey(const ndSceneNode* const node) const
					{
						const ndFloat32 val = (node->m_minBox[m_index] + node->m_maxBox[m_index]) * ndFloat32(0.5f);
						const ndUnsigned32 key = (val > m_dist) ? 1 : 0;
						return key;
					}

					ndFloat32 m_dist;
					ndInt32 m_index;
				};
				
				ndSpliteTest context(test, index);
				ndCountingSort<ndSceneNode*, ndEvaluateKey, 1>(*this, tmpBuffer, boxArray, boxCount, prefixScan, &context);
			}
			else
			{
				prefixScan[0] = 0;
				prefixScan[1] = boxCount / 2;
				prefixScan[2] = boxCount;
			}

			const ndInt32 leftCount = prefixScan[1] - prefixScan[0];
			if (leftCount == 1)
			{
				block.m_node->m_left = leafArray[block.m_start];
				block.m_node->m_left->m_parent = block.m_node;
			}
			else
			{
				ndSceneTreeNode* const node = (*nextNode)->GetInfo();
				*nextNode = (*nextNode)->GetNext();

				node->m_left = nullptr;
				node->m_right = nullptr;
				node->m_parent = block.m_node;
				block.m_node->m_left = node;

				stackPool[stack].m_node = node;
				stackPool[stack].m_count = leftCount;
				stackPool[stack].m_start = block.m_start;
				stack++;
				dAssert(stack < sizeof(stackPool) / sizeof(stackPool[0]));
			}

			const ndInt32 rightCount = prefixScan[2] - prefixScan[1];
			if (rightCount == 1)
			{
				block.m_node->m_right = leafArray[block.m_start + leftCount];
				block.m_node->m_right->m_parent = block.m_node;
			}
			else
			{
				ndSceneTreeNode* const node = (*nextNode)->GetInfo();
				*nextNode = (*nextNode)->GetNext();

				node->m_left = nullptr;
				node->m_right = nullptr;
				node->m_parent = block.m_node;
				block.m_node->m_right = node;

				stackPool[stack].m_node = node;
				stackPool[stack].m_count = rightCount;
				stackPool[stack].m_start = block.m_start + leftCount;
				stack++;
				dAssert(stack < sizeof(stackPool) / sizeof(stackPool[0]));
			}
		}
	}
	return root;
}

void ndScene::UpdateFitness(ndFitnessList& fitness, ndFloat64& oldEntropy, ndSceneNode** const root)
{
	if (*root) 
	{
		D_TRACKTIME();

		m_forceBalanceSceneCounter++;
		if (m_forceBalanceSceneCounter > 256)
		{
			m_forceBalanceScene = 1;
		}

		ndSceneNode* const parent = (*root)->m_parent;

		(*root)->m_parent = nullptr;
		ndFloat64 entropy = ReduceEntropy(fitness, root);

		if (m_forceBalanceScene || (entropy > (oldEntropy * ndFloat32(1.5f))) || (entropy < (oldEntropy * ndFloat32(0.75f))))
		{
			if (fitness.GetFirst()) 
			{
				m_scratchBuffer.SetCount((fitness.GetCount() * 2 + 16) * sizeof (ndSceneNode*));
				ndSceneNode** const leafArray = (ndSceneNode**)&m_scratchBuffer[0];
				ndSceneNode** const leafArrayUnsorted = &leafArray[m_bodyList.GetCount() + 1];

				ndInt32 leafNodesCount = 0;
				for (ndFitnessList::ndNode* nodePtr = fitness.GetFirst(); nodePtr; nodePtr = nodePtr->GetNext()) 
				{
					ndSceneNode* const node = nodePtr->GetInfo();

					ndSceneNode* const leftNode = node->GetLeft();
					ndBodyKinematic* const leftBody = leftNode->GetBody();
					if (leftBody) 
					{
						node->SetAabb(leftBody->m_minAabb, leftBody->m_maxAabb);
						leafArrayUnsorted[leafNodesCount] = leftNode;
						leafNodesCount++;
						dAssert(leafNodesCount <= m_bodyList.GetCount());
					}

					ndSceneNode* const rightNode = node->GetRight();
					ndBodyKinematic* const rightBody = rightNode->GetBody();
					if (rightBody) 
					{
						rightNode->SetAabb(rightBody->m_minAabb, rightBody->m_maxAabb);
						leafArrayUnsorted[leafNodesCount] = rightNode;
						leafNodesCount++;
						dAssert(leafNodesCount <= m_bodyList.GetCount());
					}
				}
				
				ndFitnessList::ndNode* nodePtr = fitness.GetFirst();

				class ndEvaluateKey
				{
					public:
					#define D_AABB_AREA_FACTOR	ndFloat32 (5.0f)

					ndEvaluateKey(void* const)
					{
						m_factor = ndFloat32(1.0f) / ndLog(D_AABB_AREA_FACTOR * D_AABB_AREA_FACTOR);
					}

					ndUnsigned32 GetKey(const ndSceneNode* const element) const
					{
						ndFloat32 areaA = element->m_surfaceArea;
						ndFloat32 compressedValue = m_factor * ndLog(areaA);
						dAssert(compressedValue <= 255);
						ndInt32 key = ndUnsigned32 (ndFloor (compressedValue));
						key = 255 - ndClamp(key, 0, 255);
						return key;
					}

					ndFloat32 m_factor;
				};

				ndUnsigned32 prefixScan[(1 << 8) + 1];
				ndCountingSort<ndSceneNode*, ndEvaluateKey, 8>(*this, leafArrayUnsorted, leafArray, leafNodesCount, prefixScan, nullptr);
				
				class ndItemRun
				{
					public:
					ndUnsigned32 m_start;
					ndUnsigned32 m_count;
				};

				ndInt32 pairsCount = 0;
				ndItemRun pairs[1 << 8];
				for (ndInt32 i = 0; i < (1 << 8); ++i)
				{
					ndUnsigned32 count = prefixScan[i + 1] - prefixScan[i];
					if (count)
					{
						pairs[pairsCount].m_count = count;
						pairs[pairsCount].m_start = prefixScan[i];
						pairsCount++;
					}
				}

				dAssert(pairsCount);
				if (pairsCount == 1)
				{
					*root = BuildTopDown(leafArray, 0, 0, &nodePtr);
				}
				else
				{
					ndSceneTreeNode* nodes[1 << 8];
					for (ndInt32 i = 0; i < (pairsCount-1); ++i)
					{
						ndSceneTreeNode* parentNode = nodePtr->GetInfo();
						nodePtr = nodePtr->GetNext();
						parentNode->m_left = nullptr;
						parentNode->m_parent = nullptr;

						const ndItemRun& pair = pairs[i];
						parentNode->m_right = BuildTopDown(leafArray, pair.m_start, pair.m_start + pair.m_count - 1, &nodePtr);
						parentNode->m_right->m_parent = parentNode;
						nodes[i] = parentNode;
					}

					ndSceneTreeNode* const lastNode = nodes[pairsCount - 2];
					lastNode->m_left = BuildTopDown(leafArray, pairs[pairsCount - 1].m_start, pairs[pairsCount - 1].m_start + pairs[pairsCount - 1].m_count - 1, &nodePtr);
					lastNode->m_left->m_parent = nodes[pairsCount - 2];
					const ndVector minP(lastNode->m_left->m_minBox.GetMin(lastNode->m_right->m_minBox));
					const ndVector maxP(lastNode->m_left->m_maxBox.GetMax(lastNode->m_right->m_maxBox));
					lastNode->SetAabb(minP, maxP);

					for (ndInt32 i = pairsCount - 2; i; --i)
					{
						ndSceneTreeNode* const childNode = nodes[i];
						ndSceneTreeNode* const parentNode = nodes[i - 1];
						
						childNode->m_parent = parentNode;
						parentNode->m_left = childNode;
						const ndVector minBox(parentNode->m_left->m_minBox.GetMin(parentNode->m_right->m_minBox));
						const ndVector maxBox(parentNode->m_left->m_maxBox.GetMax(parentNode->m_right->m_maxBox));
						parentNode->SetAabb(minBox, maxBox);
					}

					*root = nodes[0];
				}
				
				dAssert(!(*root)->m_parent);
				entropy = fitness.TotalCost();
				fitness.m_currentCost = entropy;
			}
			oldEntropy = entropy;
			m_forceBalanceScene = 0;
			m_forceBalanceSceneCounter = 0;
		}
		(*root)->m_parent = parent;
	}
}

void ndScene::BalanceScene()
{
	D_TRACKTIME();
	UpdateFitness(m_fitness, m_treeEntropy, &m_rootNode);
}

void ndScene::UpdateTransformNotify(ndInt32 threadIndex, ndBodyKinematic* const body)
{
	if (body->m_transformIsDirty)
	{
		body->m_transformIsDirty = 0;
		ndBodyNotify* const notify = body->GetNotifyCallback();
		if (notify)
		{
			notify->OnTransform(threadIndex, body->GetMatrix());
		}
	}
}

void ndScene::UpdateAabb(ndInt32, ndBodyKinematic* const body)
{
	if (!body->m_equilibrium | body->m_sceneForceUpdate)
	{
		ndSceneBodyNode* const bodyNode = body->GetSceneBodyNode();
		body->UpdateCollisionMatrix();

		dAssert(!bodyNode->GetLeft());
		dAssert(!bodyNode->GetRight());
		dAssert(!body->GetCollisionShape().GetShape()->GetAsShapeNull());

		const ndInt32 test = dBoxInclusionTest(body->m_minAabb, body->m_maxAabb, bodyNode->m_minBox, bodyNode->m_maxBox);
		if (!test)
		{
			bodyNode->SetAabb(body->m_minAabb, body->m_maxAabb);
			if (!m_rootNode->GetAsSceneBodyNode())
			{
				const ndSceneNode* const root = (m_rootNode->GetLeft() && m_rootNode->GetRight()) ? nullptr : m_rootNode;
				dAssert(root == nullptr);
				for (ndSceneNode* parent = bodyNode->m_parent; parent != root; parent = parent->m_parent)
				{
					ndScopeSpinLock lock(parent->m_lock);
					ndVector minBox;
					ndVector maxBox;
					ndFloat32 area = CalculateSurfaceArea(parent->GetLeft(), parent->GetRight(), minBox, maxBox);
					if (dBoxInclusionTest(minBox, maxBox, parent->m_minBox, parent->m_maxBox))
					{
						break;
					}
					parent->m_minBox = minBox;
					parent->m_maxBox = maxBox;
					parent->m_surfaceArea = area;
				}
			}
		}
		body->m_sceneEquilibrium = !body->m_sceneForceUpdate & (test != 0);
		body->m_sceneForceUpdate = 0;
	}
	else
	{
		body->m_sceneEquilibrium = 1;
	}
}

bool ndScene::ValidateContactCache(ndContact* const contact, const ndVector& timestep) const
{
	dAssert(contact && (contact->GetAsContact()));

	if (contact->m_maxDOF)
	{
		ndBodyKinematic* const body0 = contact->GetBody0();
		ndBodyKinematic* const body1 = contact->GetBody1();

		ndVector positStep(timestep * (body0->m_veloc - body1->m_veloc));
		positStep = ((positStep.DotProduct(positStep)) > m_velocTol) & positStep;
		contact->m_positAcc += positStep;

		ndVector positError2(contact->m_positAcc.DotProduct(contact->m_positAcc));
		ndVector positSign(ndVector::m_negOne & (positError2 < m_linearContactError2));
		if (positSign.GetSignMask())
		{
			ndVector rotationStep(timestep * (body0->m_omega - body1->m_omega));
			rotationStep = ((rotationStep.DotProduct(rotationStep)) > m_velocTol) & rotationStep;
			contact->m_rotationAcc = contact->m_rotationAcc * ndQuaternion(rotationStep.m_x, rotationStep.m_y, rotationStep.m_z, ndFloat32(1.0f));

			ndVector angle(contact->m_rotationAcc & ndVector::m_triplexMask);
			ndVector rotatError2(angle.DotProduct(angle));
			ndVector rotationSign(ndVector::m_negOne & (rotatError2 < m_linearContactError2));
			if (rotationSign.GetSignMask())
			{
				return true;
			}
		}
	}
	return false;
}

void ndScene::CalculateJointContacts(ndInt32 threadIndex, ndContact* const contact)
{
	ndBodyKinematic* const body0 = contact->GetBody0();
	ndBodyKinematic* const body1 = contact->GetBody1();
	
	dAssert(body0->GetScene() == this);
	dAssert(body1->GetScene() == this);

	dAssert(contact->m_material);
	dAssert(m_contactNotifyCallback);
	bool processContacts = m_contactNotifyCallback->OnAabbOverlap(contact, m_timestep);
	if (processContacts)
	{
		dAssert(!body0->GetAsBodyTriggerVolume());
		dAssert(!body0->GetCollisionShape().GetShape()->GetAsShapeNull());
		dAssert(!body1->GetCollisionShape().GetShape()->GetAsShapeNull());
			
		ndContactPoint contactBuffer[D_MAX_CONTATCS];
		ndContactSolver contactSolver(contact, m_contactNotifyCallback, m_timestep, threadIndex);
		contactSolver.m_separatingVector = contact->m_separatingVector;
		contactSolver.m_contactBuffer = contactBuffer;
		contactSolver.m_intersectionTestOnly = body0->m_contactTestOnly | body1->m_contactTestOnly;
		
		ndInt32 count = contactSolver.CalculateContactsDiscrete ();
		if (count)
		{
			contact->SetActive(true);
			if (contactSolver.m_intersectionTestOnly)
			{
				if (!contact->m_isIntersetionTestOnly)
				{
					ndBodyTriggerVolume* const trigger = body1->GetAsBodyTriggerVolume();
					if (trigger)
					{
						trigger->OnTriggerEnter(body0, m_timestep);
					}
				}
				contact->m_isIntersetionTestOnly = 1;
			}
			else
			{
				dAssert(count <= (D_CONSTRAINT_MAX_ROWS / 3));
				ProcessContacts(threadIndex, count, &contactSolver);
				dAssert(contact->m_maxDOF);
				contact->m_isIntersetionTestOnly = 0;
			}
		}
		else
		{
			if (contactSolver.m_intersectionTestOnly)
			{
				ndBodyTriggerVolume* const trigger = body1->GetAsBodyTriggerVolume();
				if (trigger)
				{
					body1->GetAsBodyTriggerVolume()->OnTriggerExit(body0, m_timestep);
				}
				contact->m_isIntersetionTestOnly = 1;
			}
			contact->m_maxDOF = 0;
		}
	}
}

void ndScene::ProcessContacts(ndInt32, ndInt32 contactCount, ndContactSolver* const contactSolver)
{
	ndContact* const contact = contactSolver->m_contact;
	contact->m_positAcc = ndVector::m_zero;
	contact->m_rotationAcc = ndQuaternion();

	ndBodyKinematic* const body0 = contact->m_body0;
	ndBodyKinematic* const body1 = contact->m_body1;
	dAssert(body0);
	dAssert(body1);
	dAssert(body0 != body1);

	contact->m_material = m_contactNotifyCallback->GetMaterial(contact, body0->GetCollisionShape(), body1->GetCollisionShape());
	const ndContactPoint* const contactArray = contactSolver->m_contactBuffer;
	
	ndInt32 count = 0;
	ndVector cachePosition[D_MAX_CONTATCS];
	ndContactPointList::ndNode* nodes[D_MAX_CONTATCS];
	ndContactPointList& contactPointList = contact->m_contacPointsList;
	for (ndContactPointList::ndNode* contactNode = contactPointList.GetFirst(); contactNode; contactNode = contactNode->GetNext()) 
	{
		nodes[count] = contactNode;
		cachePosition[count] = contactNode->GetInfo().m_point;
		count++;
	}
	
	const ndVector& v0 = body0->m_veloc;
	const ndVector& w0 = body0->m_omega;
	const ndVector& com0 = body0->m_globalCentreOfMass;
	
	const ndVector& v1 = body1->m_veloc;
	const ndVector& w1 = body1->m_omega;
	const ndVector& com1 = body1->m_globalCentreOfMass;

	ndVector controlDir0(ndVector::m_zero);
	ndVector controlDir1(ndVector::m_zero);
	ndVector controlNormal(contactArray[0].m_normal);
	ndVector vel0(v0 + w0.CrossProduct(contactArray[0].m_point - com0));
	ndVector vel1(v1 + w1.CrossProduct(contactArray[0].m_point - com1));
	ndVector vRel(vel1 - vel0);
	dAssert(controlNormal.m_w == ndFloat32(0.0f));
	ndVector tangDir(vRel - controlNormal * vRel.DotProduct(controlNormal));
	dAssert(tangDir.m_w == ndFloat32(0.0f));
	ndFloat32 diff = tangDir.DotProduct(tangDir).GetScalar();
	
	ndInt32 staticMotion = 0;
	if (diff <= ndFloat32(1.0e-2f)) 
	{
		staticMotion = 1;
		if (ndAbs(controlNormal.m_z) > ndFloat32(0.577f)) 
		{
			tangDir = ndVector(-controlNormal.m_y, controlNormal.m_z, ndFloat32(0.0f), ndFloat32(0.0f));
		}
		else 
		{
			tangDir = ndVector(-controlNormal.m_y, controlNormal.m_x, ndFloat32(0.0f), ndFloat32(0.0f));
		}
		controlDir0 = controlNormal.CrossProduct(tangDir);
		dAssert(controlDir0.m_w == ndFloat32(0.0f));
		dAssert(controlDir0.DotProduct(controlDir0).GetScalar() > ndFloat32(1.0e-8f));
		controlDir0 = controlDir0.Normalize();
		controlDir1 = controlNormal.CrossProduct(controlDir0);
		dAssert(ndAbs(controlNormal.DotProduct(controlDir0.CrossProduct(controlDir1)).GetScalar() - ndFloat32(1.0f)) < ndFloat32(1.0e-3f));
	}
	
	ndFloat32 maxImpulse = ndFloat32(-1.0f);
	for (ndInt32 i = 0; i < contactCount; ++i) 
	{
		ndInt32 index = -1;
		ndFloat32 min = ndFloat32(1.0e20f);
		ndContactPointList::ndNode* contactNode = nullptr;
		for (ndInt32 j = 0; j < count; ++j) 
		{
			ndVector v(ndVector::m_triplexMask & (cachePosition[j] - contactArray[i].m_point));
			dAssert(v.m_w == ndFloat32(0.0f));
			diff = v.DotProduct(v).GetScalar();
			if (diff < min) 
			{
				index = j;
				min = diff;
				contactNode = nodes[j];
			}
		}
	
		if (contactNode) 
		{
			count--;
			dAssert(index != -1);
			nodes[index] = nodes[count];
			cachePosition[index] = cachePosition[count];
		}
		else 
		{
			contactNode = contactPointList.Append();
		}

		ndContactMaterial* const contactPoint = &contactNode->GetInfo();
	
		dAssert(dCheckFloat(contactArray[i].m_point.m_x));
		dAssert(dCheckFloat(contactArray[i].m_point.m_y));
		dAssert(dCheckFloat(contactArray[i].m_point.m_z));
		dAssert(contactArray[i].m_body0);
		dAssert(contactArray[i].m_body1);
		dAssert(contactArray[i].m_shapeInstance0);
		dAssert(contactArray[i].m_shapeInstance1);
		dAssert(contactArray[i].m_body0 == body0);
		dAssert(contactArray[i].m_body1 == body1);
		contactPoint->m_point = contactArray[i].m_point;
		contactPoint->m_normal = contactArray[i].m_normal;
		contactPoint->m_penetration = contactArray[i].m_penetration;
		contactPoint->m_body0 = contactArray[i].m_body0;
		contactPoint->m_body1 = contactArray[i].m_body1;
		contactPoint->m_shapeInstance0 = contactArray[i].m_shapeInstance0;
		contactPoint->m_shapeInstance1 = contactArray[i].m_shapeInstance1;
		contactPoint->m_shapeId0 = contactArray[i].m_shapeId0;
		contactPoint->m_shapeId1 = contactArray[i].m_shapeId1;
		contactPoint->m_material = *contact->m_material;
	
		if (staticMotion) 
		{
			if (contactPoint->m_normal.DotProduct(controlNormal).GetScalar() > ndFloat32(0.9995f)) 
			{
				contactPoint->m_dir0 = controlDir0;
				contactPoint->m_dir1 = controlDir1;
			}
			else 
			{
				if (ndAbs(contactPoint->m_normal.m_z) > ndFloat32(0.577f))
				{
					tangDir = ndVector(-contactPoint->m_normal.m_y, contactPoint->m_normal.m_z, ndFloat32(0.0f), ndFloat32(0.0f));
				}
				else 
				{
					tangDir = ndVector(-contactPoint->m_normal.m_y, contactPoint->m_normal.m_x, ndFloat32(0.0f), ndFloat32(0.0f));
				}
				contactPoint->m_dir0 = contactPoint->m_normal.CrossProduct(tangDir);
				dAssert(contactPoint->m_dir0.m_w == ndFloat32(0.0f));
				dAssert(contactPoint->m_dir0.DotProduct(contactPoint->m_dir0).GetScalar() > ndFloat32(1.0e-8f));
				contactPoint->m_dir0 = contactPoint->m_dir0.Normalize();
				contactPoint->m_dir1 = contactPoint->m_normal.CrossProduct(contactPoint->m_dir0);
				dAssert(ndAbs(contactPoint->m_normal.DotProduct(contactPoint->m_dir0.CrossProduct(contactPoint->m_dir1)).GetScalar() - ndFloat32(1.0f)) < ndFloat32(1.0e-3f));
			}
		}
		else 
		{
			ndVector veloc0(v0 + w0.CrossProduct(contactPoint->m_point - com0));
			ndVector veloc1(v1 + w1.CrossProduct(contactPoint->m_point - com1));
			ndVector relReloc(veloc1 - veloc0);
	
			dAssert(contactPoint->m_normal.m_w == ndFloat32(0.0f));
			ndFloat32 impulse = relReloc.DotProduct(contactPoint->m_normal).GetScalar();
			if (ndAbs(impulse) > maxImpulse) 
			{
				maxImpulse = ndAbs(impulse);
			}
	
			ndVector tangentDir(relReloc - contactPoint->m_normal.Scale(impulse));
			dAssert(tangentDir.m_w == ndFloat32(0.0f));
			diff = tangentDir.DotProduct(tangentDir).GetScalar();
			if (diff > ndFloat32(1.0e-2f)) 
			{
				dAssert(tangentDir.m_w == ndFloat32(0.0f));
				contactPoint->m_dir0 = tangentDir.Normalize();
			}
			else 
			{
				if (ndAbs(contactPoint->m_normal.m_z) > ndFloat32(0.577f)) 
				{
					tangentDir = ndVector(-contactPoint->m_normal.m_y, contactPoint->m_normal.m_z, ndFloat32(0.0f), ndFloat32(0.0f));
				}
				else 
				{
					tangentDir = ndVector(-contactPoint->m_normal.m_y, contactPoint->m_normal.m_x, ndFloat32(0.0f), ndFloat32(0.0f));
				}
				contactPoint->m_dir0 = contactPoint->m_normal.CrossProduct(tangentDir);
				dAssert(contactPoint->m_dir0.m_w == ndFloat32(0.0f));
				dAssert(contactPoint->m_dir0.DotProduct(contactPoint->m_dir0).GetScalar() > ndFloat32(1.0e-8f));
				contactPoint->m_dir0 = contactPoint->m_dir0.Normalize();
			}
			contactPoint->m_dir1 = contactPoint->m_normal.CrossProduct(contactPoint->m_dir0);
			dAssert(ndAbs(contactPoint->m_normal.DotProduct(contactPoint->m_dir0.CrossProduct(contactPoint->m_dir1)).GetScalar() - ndFloat32(1.0f)) < ndFloat32(1.0e-3f));
		}
		dAssert(contactPoint->m_dir0.m_w == ndFloat32(0.0f));
		dAssert(contactPoint->m_dir0.m_w == ndFloat32(0.0f));
		dAssert(contactPoint->m_normal.m_w == ndFloat32(0.0f));
	}
	
	for (ndInt32 i = 0; i < count; ++i) 
	{
		contactPointList.Remove(nodes[i]);
	}
	
	contact->m_maxDOF = ndUnsigned32(3 * contactPointList.GetCount());
	m_contactNotifyCallback->OnContactCallback(contact, m_timestep);
}

void ndScene::SubmitPairs(ndSceneBodyNode* const leafNode, ndSceneNode* const node, ndInt32 threadId)
{
	ndSceneNode* pool[D_SCENE_MAX_STACK_DEPTH];

	ndBodyKinematic* const body0 = leafNode->GetBody() ? leafNode->GetBody() : nullptr;
	dAssert(body0);

	const ndVector boxP0(leafNode->m_minBox);
	const ndVector boxP1(leafNode->m_maxBox);
	const bool test0 = (body0->m_invMass.m_w != ndFloat32(0.0f)) & body0->GetCollisionShape().GetCollisionMode();

	pool[0] = node;
	ndInt32 stack = 1;
	while (stack && (stack < (D_SCENE_MAX_STACK_DEPTH - 16)))
	{
		stack--;
		ndSceneNode* const rootNode = pool[stack];
		if (dOverlapTest(rootNode->m_minBox, rootNode->m_maxBox, boxP0, boxP1)) 
		{
			if (rootNode->GetAsSceneBodyNode()) 
			{
				dAssert(!rootNode->GetRight());
				dAssert(!rootNode->GetLeft());
				
				ndBodyKinematic* const body1 = rootNode->GetBody();
				dAssert(body1);
				const bool test1 = (body1->m_invMass.m_w != ndFloat32(0.0f)) & body1->GetCollisionShape().GetCollisionMode();
				const bool test = test0 | test1;
				if (test)
				{
					AddPair(body0, body1, threadId);
				}
			}
			else 
			{
				ndSceneTreeNode* const tmpNode = rootNode->GetAsSceneTreeNode();
				dAssert(tmpNode->m_left);
				dAssert(tmpNode->m_right);
		
				pool[stack] = tmpNode->m_left;
				stack++;
				dAssert(stack < ndInt32(sizeof(pool) / sizeof(pool[0])));
		
				pool[stack] = tmpNode->m_right;
				stack++;
				dAssert(stack < ndInt32(sizeof(pool) / sizeof(pool[0])));
			}
		}
	}

	if (stack)
	{
		m_forceBalanceScene = 1;
	}
}

ndJointBilateralConstraint* ndScene::FindBilateralJoint(ndBodyKinematic* const body0, ndBodyKinematic* const body1) const
{
	if (body0->m_jointList.GetCount() <= body1->m_jointList.GetCount())
	{
		for (ndJointList::ndNode* node = body0->m_jointList.GetFirst(); node; node = node->GetNext())
		{
			ndJointBilateralConstraint* const joint = node->GetInfo();
			if ((joint->GetBody0() == body1) || (joint->GetBody1() == body1))
			{
				return joint;
			}
		}
	}
	else
	{
		for (ndJointList::ndNode* node = body1->m_jointList.GetFirst(); node; node = node->GetNext())
		{
			ndJointBilateralConstraint* const joint = node->GetInfo();
			if ((joint->GetBody0() == body0) || (joint->GetBody1() == body0))
			{
				return joint;
			}
		}
	}
	return nullptr;
}


void ndScene::UpdateBodyList()
{
	D_TRACKTIME();
	if (m_bodyListChanged)
	{
		ndInt32 index = 0;
		ndArray<ndBodyKinematic*>& view = GetActiveBodyArray();
		view.SetCount(m_bodyList.GetCount());
		for (ndBodyList::ndNode* node = m_bodyList.GetFirst(); node; node = node->GetNext())
		{
			ndBodyKinematic* const body = node->GetInfo();
			view[index] = node->GetInfo();
			++index;

			dAssert(!body->GetCollisionShape().GetShape()->GetAsShapeNull());
			bool inScene = true;
			if (!body->GetSceneBodyNode())
			{
				inScene = AddBody(body);
			}
			dAssert(inScene && body->GetSceneBodyNode());
		}
		m_bodyListChanged = 0;
		view.PushBack(m_sentinelBody);
	}
}

void ndScene::InitBodyArray()
{
	D_TRACKTIME();
	ndInt32 scans[D_MAX_THREADS_COUNT][2];
	auto BuildBodyArray = ndMakeObject::ndFunction([this, &scans](ndInt32 threadIndex, ndInt32 threadCount)
	{
		D_TRACKTIME();
		const ndArray<ndBodyKinematic*>& view = GetActiveBodyArray();
		
		ndInt32* const scan = &scans[threadIndex][0];
		scan[0] = 0;
		scan[1] = 0;
		
		const ndFloat32 timestep = m_timestep;
		const ndStartEnd startEnd(view.GetCount() - 1, threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndBodyKinematic* const body = view[i];
			body->ApplyExternalForces(threadIndex, timestep);

			body->PrepareStep(i);
			UpdateAabb(threadIndex, body);

			const ndInt32 key = body->m_sceneEquilibrium;
			scan[key] ++;
		}
	});

	auto CompactMovingBodies = ndMakeObject::ndFunction([this, &scans](ndInt32 threadIndex, ndInt32 threadCount)
	{
		D_TRACKTIME();
		const ndArray<ndBodyKinematic*>& activeBodyArray = GetActiveBodyArray();
		ndBodyKinematic** const sceneBodyArray = &m_sceneBodyArray[0];

		const ndArray<ndBodyKinematic*>& view = m_bodyList.m_view;
		ndInt32* const scan = &scans[threadIndex][0];

		const ndStartEnd startEnd(view.GetCount() - 1, threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndBodyKinematic* const body = activeBodyArray[i];
			const ndInt32 key = body->m_sceneEquilibrium;
			const ndInt32 index = scan[key];
			sceneBodyArray[index] = body;
			scan[key] ++;
		}
	});

	ParallelExecute(BuildBodyArray);
	ndInt32 sum = 0;
	ndInt32 threadCount = GetThreadCount();
	for (ndInt32 j = 0; j < 2; ++j)
	{
		for (ndInt32 i = 0; i < threadCount; ++i)
		{
			const ndInt32 count = scans[i][j];
			scans[i][j] = sum;
			sum += count;
		}
	}

	ndInt32 movingBodyCount = scans[0][1] - scans[0][0];
	m_sceneBodyArray.SetCount(m_bodyList.GetCount());
	if (movingBodyCount)
	{
		ParallelExecute(CompactMovingBodies);
	}

	m_sceneBodyArray.SetCount(movingBodyCount);

	ndBodyKinematic* const sentinelBody = m_sentinelBody;
	sentinelBody->PrepareStep(GetActiveBodyArray().GetCount() - 1);

	sentinelBody->m_isStatic = 1;
	sentinelBody->m_autoSleep = 1;
	sentinelBody->m_equilibrium = 1;
	sentinelBody->m_equilibrium0 = 1;
	sentinelBody->m_isJointFence0 = 1;
	sentinelBody->m_isJointFence1 = 1;
	sentinelBody->m_isConstrained = 0;
	sentinelBody->m_sceneEquilibrium = 1;
	sentinelBody->m_weigh = ndFloat32(0.0f);
}

void ndScene::FindCollidingPairs(ndBodyKinematic* const body, ndInt32 threadId)
{
	ndSceneBodyNode* const bodyNode = body->GetSceneBodyNode();
	for (ndSceneNode* ptr = bodyNode; ptr->m_parent; ptr = ptr->m_parent)
	{
		ndSceneTreeNode* const parent = ptr->m_parent->GetAsSceneTreeNode();
		dAssert(!parent->GetAsSceneBodyNode());
		ndSceneNode* const sibling = parent->m_right;
		if (sibling != ptr)
		{
			SubmitPairs(bodyNode, sibling, threadId);
		}
	}
}

void ndScene::FindCollidingPairsForward(ndBodyKinematic* const body, ndInt32 threadId)
{
	ndSceneBodyNode* const bodyNode = body->GetSceneBodyNode();
	for (ndSceneNode* ptr = bodyNode; ptr->m_parent; ptr = ptr->m_parent)
	{
		ndSceneTreeNode* const parent = ptr->m_parent->GetAsSceneTreeNode();
		dAssert(!parent->GetAsSceneBodyNode());
		ndSceneNode* const sibling = parent->m_right;
		if (sibling != ptr)
		{
			SubmitPairs(bodyNode, sibling, threadId);
		}
	}
}

void ndScene::FindCollidingPairsBackward(ndBodyKinematic* const body, ndInt32 threadId)
{
	ndSceneBodyNode* const bodyNode = body->GetSceneBodyNode();
	for (ndSceneNode* ptr = bodyNode; ptr->m_parent; ptr = ptr->m_parent)
	{
		ndSceneTreeNode* const parent = ptr->m_parent->GetAsSceneTreeNode();
		dAssert(!parent->GetAsSceneBodyNode());
		ndSceneNode* const sibling = parent->m_left;
		if (sibling != ptr)
		{
			SubmitPairs(bodyNode, sibling, threadId);
		}
	}
}

void ndScene::FindCollidingPairs()
{
	D_TRACKTIME();
	auto FindPairs = ndMakeObject::ndFunction([this](ndInt32 threadIndex, ndInt32 threadCount)
	{
		D_TRACKTIME();
		const ndArray<ndBodyKinematic*>& bodyArray = GetActiveBodyArray();
		const ndStartEnd startEnd(bodyArray.GetCount() - 1, threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndBodyKinematic* const body = bodyArray[i];
			FindCollidingPairs(body, threadIndex);
		}
	});

	auto FindPairsForward = ndMakeObject::ndFunction([this](ndInt32 threadIndex, ndInt32 threadCount)
	{
		D_TRACKTIME();
		const ndArray<ndBodyKinematic*>& bodyArray = m_sceneBodyArray;
		const ndStartEnd startEnd(bodyArray.GetCount(), threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndBodyKinematic* const body = bodyArray[i];
			FindCollidingPairsForward(body, threadIndex);
		}
	});

	auto FindPairsBackward = ndMakeObject::ndFunction([this](ndInt32 threadIndex, ndInt32 threadCount)
	{
		D_TRACKTIME();
		const ndArray<ndBodyKinematic*>& bodyArray = m_sceneBodyArray;
		const ndStartEnd startEnd(bodyArray.GetCount(), threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndBodyKinematic* const body = bodyArray[i];
			FindCollidingPairsBackward(body, threadIndex);
		}
	});

	#ifdef D_NEW_SCENE
	for (ndInt32 i = GetThreadCount() - 1; i >= 0; --i)
	{
		m_particalNewPairs[i].SetCount(0);
	}
	#endif	

	const ndArray<ndBodyKinematic*>& activeBodies = GetActiveBodyArray();
	bool fullScan = (2 * m_sceneBodyArray.GetCount()) > activeBodies.GetCount();
	if (fullScan)
	{
		ParallelExecute(FindPairs);
		#ifdef D_NEW_SCENE
			dAssert(0);
		#endif	
	}
	else
	{
		ParallelExecute(FindPairsForward);
		ParallelExecute(FindPairsBackward);

		#ifdef D_NEW_SCENE
		dAssert(0);
		#endif	
	}
}

void ndScene::UpdateTransform()
{
	D_TRACKTIME();
	auto TransformUpdate = ndMakeObject::ndFunction([this](ndInt32 threadIndex, ndInt32 threadCount)
	{
		D_TRACKTIME();
		const ndArray<ndBodyKinematic*>& bodyArray = GetActiveBodyArray();
		const ndStartEnd startEnd(bodyArray.GetCount() - 1, threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndBodyKinematic* const body = bodyArray[i];
			UpdateTransformNotify(threadIndex, body);
		}
	});
	ParallelExecute(TransformUpdate);
}

void ndScene::CalculateContacts(ndInt32 threadIndex, ndContact* const contact)
{
	const ndUnsigned32 lru = m_lru - D_CONTACT_DELAY_FRAMES;

	ndVector deltaTime(m_timestep);
	ndBodyKinematic* const body0 = contact->GetBody0();
	ndBodyKinematic* const body1 = contact->GetBody1();

	dAssert(!contact->m_isDead);
	if (!(body0->m_equilibrium & body1->m_equilibrium))
	{
		bool active = contact->IsActive();
		if (ValidateContactCache(contact, deltaTime))
		{
			contact->m_sceneLru = m_lru;
			contact->m_timeOfImpact = ndFloat32(1.0e10f);
		}
		else
		{
			contact->SetActive(false);
			contact->m_positAcc = ndVector::m_zero;
			contact->m_rotationAcc = ndQuaternion();

			ndFloat32 distance = contact->m_separationDistance;
			if (distance >= D_NARROW_PHASE_DIST)
			{
				const ndVector veloc0(body0->GetVelocity());
				const ndVector veloc1(body1->GetVelocity());
				
				const ndVector veloc(veloc1 - veloc0);
				const ndVector omega0(body0->GetOmega());
				const ndVector omega1(body1->GetOmega());
				const ndShapeInstance* const collision0 = &body0->GetCollisionShape();
				const ndShapeInstance* const collision1 = &body1->GetCollisionShape();
				const ndVector scale(ndFloat32(1.0f), ndFloat32(3.5f) * collision0->GetBoxMaxRadius(), ndFloat32(3.5f) * collision1->GetBoxMaxRadius(), ndFloat32(0.0f));
				const ndVector velocMag2(veloc.DotProduct(veloc).GetScalar(), omega0.DotProduct(omega0).GetScalar(), omega1.DotProduct(omega1).GetScalar(), ndFloat32(0.0f));
				const ndVector velocMag(velocMag2.GetMax(ndVector::m_epsilon).InvSqrt() * velocMag2 * scale);
				const ndFloat32 speed = velocMag.AddHorizontal().GetScalar() + ndFloat32(0.5f);
				
				distance -= speed * m_timestep;
				contact->m_separationDistance = distance;
			}
			if (distance < D_NARROW_PHASE_DIST)
			{
				CalculateJointContacts(threadIndex, contact);
				if (contact->m_maxDOF || contact->m_isIntersetionTestOnly)
				{
					contact->SetActive(true);
					contact->m_timeOfImpact = ndFloat32(1.0e10f);
				}
				contact->m_sceneLru = m_lru;
			}
			else
			{
				const ndSceneBodyNode* const bodyNode0 = contact->GetBody0()->GetSceneBodyNode();
				const ndSceneBodyNode* const bodyNode1 = contact->GetBody1()->GetSceneBodyNode();
				if (dOverlapTest(bodyNode0->m_minBox, bodyNode0->m_maxBox, bodyNode1->m_minBox, bodyNode1->m_maxBox)) 
				{
					contact->m_sceneLru = m_lru;
				}
				else if (contact->m_sceneLru < lru) 
				{
					contact->m_isDead = 1;
				}
			}
		}

		if (active ^ contact->IsActive())
		{
			dAssert(body0->GetInvMass() > ndFloat32(0.0f));
			body0->m_equilibrium = 0;
			if (body1->GetInvMass() > ndFloat32(0.0f))
			{
				body1->m_equilibrium = 0;
			}
		}
	}
	else
	{
		contact->m_sceneLru = m_lru;
	}

	if (!contact->m_isDead && (body0->m_equilibrium & body1->m_equilibrium & !contact->IsActive()))
	{
		const ndSceneBodyNode* const bodyNode0 = contact->GetBody0()->GetSceneBodyNode();
		const ndSceneBodyNode* const bodyNode1 = contact->GetBody1()->GetSceneBodyNode();
		if (!dOverlapTest(bodyNode0->m_minBox, bodyNode0->m_maxBox, bodyNode1->m_minBox, bodyNode1->m_maxBox))
		{
			contact->m_isDead = 1;
		}
	}
}

void ndScene::UpdateSpecial()
{
	for (ndList<ndBodyKinematic*>::ndNode* node = m_specialUpdateList.GetFirst(); node; node = node->GetNext())
	{
		ndBodyKinematic* const body = node->GetInfo();
		body->SpecialUpdate(m_timestep);
	}
}

bool ndScene::ConvexCast(ndConvexCastNotify& callback, const ndSceneNode** stackPool, ndFloat32* const stackDistance, ndInt32 stack, const ndFastRay& ray, const ndShapeInstance& convexShape, const ndMatrix& globalOrigin, const ndVector& globalDest) const
{
	ndVector boxP0;
	ndVector boxP1;

	dAssert(globalOrigin.TestOrthogonal());
	convexShape.CalculateAabb(globalOrigin, boxP0, boxP1);

	callback.m_contacts.SetCount(0);
	callback.m_param = ndFloat32(1.2f);
	while (stack && (stack < (D_SCENE_MAX_STACK_DEPTH - 4)))
	{
		stack--;
		ndFloat32 dist = stackDistance[stack];
		
		if (dist > callback.m_param)
		{
			break;
		}
		else 
		{
			const ndSceneNode* const me = stackPool[stack];
		
			ndBody* const body = me->GetBody();
			if (body) 
			{
				if (callback.OnRayPrecastAction (body, &convexShape)) 
				{
					// save contacts and try new set
					ndConvexCastNotify savedNotification(callback);
					ndBodyKinematic* const kinBody = body->GetAsBodyKinematic();
					callback.m_contacts.SetCount(0);
					if (callback.CastShape(convexShape, globalOrigin, globalDest, kinBody))
					{
						// found new contacts, see how the are managed
						if (ndAbs(savedNotification.m_param - callback.m_param) < ndFloat32(-1.0e-3f))
						{
							// merge contact
							for (ndInt32 i = 0; i < savedNotification.m_contacts.GetCount(); ++i)
							{
								const ndContactPoint& contact = savedNotification.m_contacts[i];
								bool newPoint = true;
								for (ndInt32 j = callback.m_contacts.GetCount() - 1; j >= 0; ++j)
								{
									const ndVector diff(callback.m_contacts[j].m_point - contact.m_point);
									ndFloat32 mag2 = diff.DotProduct(diff & ndVector::m_triplexMask).GetScalar();
									newPoint = newPoint & (mag2 > ndFloat32(1.0e-5f));
								}
								if (newPoint && (callback.m_contacts.GetCount() < callback.m_contacts.GetCapacity()))
								{
									callback.m_contacts.PushBack(contact);
								}
							}
						}
						else if (callback.m_param > savedNotification.m_param)
						{
							// restore contacts
							callback.m_normal = savedNotification.m_normal;
							callback.m_closestPoint0 = savedNotification.m_closestPoint0;
							callback.m_closestPoint1 = savedNotification.m_closestPoint1;
							callback.m_param = savedNotification.m_param;
							for (ndInt32 i = 0; i < savedNotification.m_contacts.GetCount(); ++i)
							{
								callback.m_contacts[i] = savedNotification.m_contacts[i];
							}
						}
					}
					else
					{
						// no new contacts restore old ones,
						// in theory it should no copy, by the notification may change
						// the previous found contacts
						callback.m_normal = savedNotification.m_normal;
						callback.m_closestPoint0 = savedNotification.m_closestPoint0;
						callback.m_closestPoint1 = savedNotification.m_closestPoint1;
						callback.m_param = savedNotification.m_param;
						for (ndInt32 i = 0; i < savedNotification.m_contacts.GetCount(); ++i)
						{
							callback.m_contacts[i] = savedNotification.m_contacts[i];
						}
					}

					if (callback.m_param < ndFloat32 (1.0e-8f)) 
					{
						break;
					}
				}
			}
			else 
			{
				{
					const ndSceneNode* const left = me->GetLeft();
					dAssert(left);
					const ndVector minBox(left->m_minBox - boxP1);
					const ndVector maxBox(left->m_maxBox - boxP0);
					ndFloat32 dist1 = ray.BoxIntersect(minBox, maxBox);
					if (dist1 < callback.m_param)
					{
						ndInt32 j = stack;
						for (; j && (dist1 > stackDistance[j - 1]); j--)
						{
							stackPool[j] = stackPool[j - 1];
							stackDistance[j] = stackDistance[j - 1];
						}
						stackPool[j] = left;
						stackDistance[j] = dist1;
						stack++;
						dAssert(stack < D_SCENE_MAX_STACK_DEPTH);
					}
				}
		
				{
					const ndSceneNode* const right = me->GetRight();
					dAssert(right);
					const ndVector minBox(right->m_minBox - boxP1);
					const ndVector maxBox = right->m_maxBox - boxP0;
					ndFloat32 dist1 = ray.BoxIntersect(minBox, maxBox);
					if (dist1 < callback.m_param)
					{
						ndInt32 j = stack;
						for (; j && (dist1 > stackDistance[j - 1]); j--) 
						{
							stackPool[j] = stackPool[j - 1];
							stackDistance[j] = stackDistance[j - 1];
						}
						stackPool[j] = right;
						stackDistance[j] = dist1;
						stack++;
						dAssert(stack < D_SCENE_MAX_STACK_DEPTH);
					}
				}
			}
		}
	}
	return callback.m_contacts.GetCount() > 0;
}

bool ndScene::RayCast(ndRayCastNotify& callback, const ndSceneNode** stackPool, ndFloat32* const stackDistance, ndInt32 stack, const ndFastRay& ray) const
{
	bool state = false;
	while (stack && (stack < (D_SCENE_MAX_STACK_DEPTH - 4)))
	{
		stack--;
		ndFloat32 dist = stackDistance[stack];
		if (dist > callback.m_param)
		{
			break;
		}
		else
		{
			const ndSceneNode* const me = stackPool[stack];
			dAssert(me);
			ndBodyKinematic* const body = me->GetBody();
			if (body)
			{
				dAssert(!me->GetLeft());
				dAssert(!me->GetRight());

				//callback.TraceShape(ray.m_p0, ray.m_p1, body->GetCollisionShape(), body->GetMatrix());
				if (body->RayCast(callback, ray, callback.m_param))
				{
					state = true;
					if (callback.m_param < ndFloat32(1.0e-8f))
					{
						break;
					}
				}
			}
			else
			{
				const ndSceneNode* const left = me->GetLeft();
				dAssert(left);
				ndFloat32 dist1 = ray.BoxIntersect(left->m_minBox, left->m_maxBox);
				if (dist1 < callback.m_param)
				{
					ndInt32 j = stack;
					for (; j && (dist1 > stackDistance[j - 1]); j--)
					{
						stackPool[j] = stackPool[j - 1];
						stackDistance[j] = stackDistance[j - 1];
					}
					stackPool[j] = left;
					stackDistance[j] = dist1;
					stack++;
					dAssert(stack < D_SCENE_MAX_STACK_DEPTH);
				}
	
				const ndSceneNode* const right = me->GetRight();
				dAssert(right);
				dist1 = ray.BoxIntersect(right->m_minBox, right->m_maxBox);
				if (dist1 < callback.m_param)
				{
					ndInt32 j = stack;
					for (; j && (dist1 > stackDistance[j - 1]); j--)
					{
						stackPool[j] = stackPool[j - 1];
						stackDistance[j] = stackDistance[j - 1];
					}
					stackPool[j] = right;
					stackDistance[j] = dist1;
					stack++;
					dAssert(stack < D_SCENE_MAX_STACK_DEPTH);
				}
			}
		}
	}
	return state;
}

void ndScene::BodiesInAabb(ndBodiesInAabbNotify& callback, const ndSceneNode** stackPool, ndInt32 stack) const
{
	callback.m_bodyArray.SetCount(0);
	while (stack && (stack < (D_SCENE_MAX_STACK_DEPTH - 4)))
	{
		stack--;
		
		const ndSceneNode* const me = stackPool[stack];
		dAssert(me);
		ndBodyKinematic* const body = me->GetBody();
		if (body)
		{
			dAssert(!me->GetLeft());
			dAssert(!me->GetRight());
			if (callback.OnOverlap(body))
			{
				callback.m_bodyArray.PushBack(body);
			}
		}
		else
		{
			const ndSceneNode* const left = me->GetLeft();
			dAssert(left);
			stackPool[stack] = left;
			stack++;
			dAssert(stack < D_SCENE_MAX_STACK_DEPTH);

			const ndSceneNode* const right = me->GetRight();
			dAssert(right);
			stackPool[stack] = right;
			stack++;
			dAssert(stack < D_SCENE_MAX_STACK_DEPTH);
		}
	}
}

void ndScene::Cleanup()
{
	Sync();
	m_backgroundThread.Terminate();

	m_bodyListChanged = 1;
	while (m_bodyList.GetFirst())
	{
		ndBodyKinematic* const body = m_bodyList.GetFirst()->GetInfo();
		RemoveBody(body);
		delete body;
	}
	if (m_sentinelBody)
	{
		delete m_sentinelBody;
		m_sentinelBody = nullptr;
	}
	m_contactArray.DeleteAllContacts();

	ndFreeListAlloc::Flush();
	m_contactArray.Resize(1024);
	m_sceneBodyArray.Resize(1024);
	m_activeConstraintArray.Resize(1024);
	m_scratchBuffer.Resize(1024 * sizeof(void*));

	m_contactArray.SetCount(0);
	m_scratchBuffer.SetCount(0);
	m_sceneBodyArray.SetCount(0);
	m_activeConstraintArray.SetCount(0);
}

void ndScene::AddNode(ndSceneNode* const newNode)
{
	if (m_rootNode)
	{
		ndSceneTreeNode* const node = InsertNode(m_rootNode, newNode);
		m_fitness.AddNode(node);
		if (!node->m_parent)
		{
			m_rootNode = node;
		}
	}
	else
	{
		m_rootNode = newNode;
	}
}

void ndScene::RemoveNode(ndSceneNode* const node)
{
	if (node->m_parent)
	{
		ndSceneTreeNode* const parent = (ndSceneTreeNode*)node->m_parent;
		if (parent->m_parent)
		{
			ndSceneTreeNode* const grandParent = (ndSceneTreeNode*)parent->m_parent;
			if (grandParent->m_left == parent)
			{
				if (parent->m_right == node)
				{
					grandParent->m_left = parent->m_left;
					parent->m_left->m_parent = grandParent;
					parent->m_left = nullptr;
					parent->m_parent = nullptr;
				}
				else
				{
					grandParent->m_left = parent->m_right;
					parent->m_right->m_parent = grandParent;
					parent->m_right = nullptr;
					parent->m_parent = nullptr;
				}
			}
			else
			{
				if (parent->m_right == node)
				{
					grandParent->m_right = parent->m_left;
					parent->m_left->m_parent = grandParent;
					parent->m_left = nullptr;
					parent->m_parent = nullptr;
				}
				else
				{
					grandParent->m_right = parent->m_right;
					parent->m_right->m_parent = grandParent;
					parent->m_right = nullptr;
					parent->m_parent = nullptr;
				}
			}
		}
		else
		{
			dAssert(!node->m_parent->GetAsSceneBodyNode());
			ndSceneTreeNode* const parent1 = node->m_parent->GetAsSceneTreeNode();
			if (parent1->m_right == node)
			{
				m_rootNode = parent1->m_left;
				m_rootNode->m_parent = nullptr;
				parent1->m_left = nullptr;
			}
			else
			{
				m_rootNode = parent1->m_right;
				m_rootNode->m_parent = nullptr;
				parent1->m_right = nullptr;
			}
		}

		if (parent->m_fitnessNode)
		{
			m_fitness.RemoveNode(parent);
		}
		delete parent;
	}
	else
	{
		delete node;
		m_rootNode = nullptr;
	}
}

bool ndScene::RayCast(ndRayCastNotify& callback, const ndVector& globalOrigin, const ndVector& globalDest) const
{
	const ndVector p0(globalOrigin & ndVector::m_triplexMask);
	const ndVector p1(globalDest & ndVector::m_triplexMask);

	bool state = false;
	callback.m_param = ndFloat32(1.2f);
	if (m_rootNode)
	{
		const ndVector segment(p1 - p0);
		ndFloat32 dist2 = segment.DotProduct(segment).GetScalar();
		if (dist2 > ndFloat32(1.0e-8f))
		{
			ndFloat32 distance[D_SCENE_MAX_STACK_DEPTH];
			const ndSceneNode* stackPool[D_SCENE_MAX_STACK_DEPTH];

			ndFastRay ray(p0, p1);

			stackPool[0] = m_rootNode;
			distance[0] = ray.BoxIntersect(m_rootNode->m_minBox, m_rootNode->m_maxBox);
			state = RayCast(callback, stackPool, distance, 1, ray);
		}
	}
	return state;
}

bool ndScene::ConvexCast(ndConvexCastNotify& callback, const ndShapeInstance& convexShape, const ndMatrix& globalOrigin, const ndVector& globalDest) const
{
	bool state = false;
	callback.m_param = ndFloat32(1.2f);
	if (m_rootNode)
	{
		ndVector boxP0;
		ndVector boxP1;
		dAssert(globalOrigin.TestOrthogonal());
		convexShape.CalculateAabb(globalOrigin, boxP0, boxP1);

		ndFloat32 distance[D_SCENE_MAX_STACK_DEPTH];
		const ndSceneNode* stackPool[D_SCENE_MAX_STACK_DEPTH];

		const ndVector velocB(ndVector::m_zero);
		const ndVector velocA((globalDest - globalOrigin.m_posit) & ndVector::m_triplexMask);
		const ndVector minBox(m_rootNode->m_minBox - boxP1);
		const ndVector maxBox(m_rootNode->m_maxBox - boxP0);
		ndFastRay ray(ndVector::m_zero, velocA);

		stackPool[0] = m_rootNode;
		distance[0] = ray.BoxIntersect(minBox, maxBox);
		state = ConvexCast(callback, stackPool, distance, 1, ray, convexShape, globalOrigin, globalDest);
	}
	return state;
}

void ndScene::BodiesInAabb(ndBodiesInAabbNotify& callback) const
{
	callback.m_bodyArray.SetCount(0);

	if (m_rootNode)
	{
		const ndSceneNode* stackPool[D_SCENE_MAX_STACK_DEPTH];
		stackPool[0] = m_rootNode;
		ndScene::BodiesInAabb(callback, stackPool, 1);
	}
}

void ndScene::SendBackgroundTask(ndBackgroundTask* const job)
{
	m_backgroundThread.SendTask(job);
}

#ifndef D_NEW_SCENE
void ndScene::CalculateContacts()
{
	D_TRACKTIME();
	ndInt32 digitScan[D_MAX_THREADS_COUNT][4];

	auto CalculateNewContacts = ndMakeObject::ndFunction([this, &digitScan](ndInt32 threadIndex, ndInt32 threadCount)
	{
		D_TRACKTIME();
		ndContactArray& activeContacts = m_contactArray;
		ndContact** const dstContacts = (ndContact**)&m_scratchBuffer[0];
		ndInt32* const scan = &digitScan[threadIndex][0];

		ndInt32 keyLookUp[4];
		scan[0] = 0;
		scan[1] = 0;
		scan[2] = 0;
		scan[3] = 0;
		keyLookUp[0] = 0;
		keyLookUp[1] = 1;
		keyLookUp[2] = 2;
		keyLookUp[3] = 2;

		const ndStartEnd startEnd(activeContacts.GetCount(), threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndContact* const contact = activeContacts[i]->GetAsContact();
			dAssert(contact);
			if (!contact->m_isDead)
			{
				CalculateContacts(threadIndex, contact);
			}
			dstContacts[i] = contact;
			const ndInt32 entry = (!contact->IsActive() | !contact->m_maxDOF) + contact->m_isDead * 2;
			const ndInt32 key = keyLookUp[entry];
			scan[key] ++;
		}
	});

	auto CompactContacts = ndMakeObject::ndFunction([this, &digitScan](ndInt32 threadIndex, ndInt32 threadCount)
	{
		D_TRACKTIME();
		ndContactArray& dstContacts = m_contactArray;
		ndContact** const srcContacts = (ndContact**)&m_scratchBuffer[0];
		ndArray<ndConstraint*>& activeConstraintArray = m_activeConstraintArray;

		ndInt32 keyLookUp[4];
		keyLookUp[0] = 0;
		keyLookUp[1] = 1;
		keyLookUp[2] = 2;
		keyLookUp[3] = 2;
		ndInt32* const scan = &digitScan[threadIndex][0];

		const ndStartEnd startEnd(dstContacts.GetCount(), threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndContact* const contact = srcContacts[i]->GetAsContact();
			const ndInt32 entry = (!contact->IsActive() | !contact->m_maxDOF) + contact->m_isDead * 2;
			const ndInt32 key = keyLookUp[entry];
			const ndInt32 index = scan[key];
			dstContacts[index] = contact;
			activeConstraintArray[index] = contact;
			scan[key]++;
		}
	});

	m_activeConstraintArray.SetCount(0);
	if (m_contactArray.GetCount())
	{
		m_scratchBuffer.SetCount(m_contactArray.GetCount() * sizeof(ndContact*));
		m_activeConstraintArray.SetCount(m_contactArray.GetCount());

		ParallelExecute(CalculateNewContacts);

		ndInt32 sum = 0;
		ndInt32 threadCount = GetThreadCount();
		for (ndInt32 j = 0; j < 4; ++j)
		{
			for (ndInt32 i = 0; i < threadCount; ++i)
			{
				const ndInt32 count = digitScan[i][j];
				digitScan[i][j] = sum;
				sum += count;
			}
		}

		ndInt32 activeJoints = digitScan[0][1] - digitScan[0][0];
		ndInt32 inactiveJoints = digitScan[0][2] - digitScan[0][1];
		ndInt32 deadContacts = digitScan[0][3] - digitScan[0][2];

		ParallelExecute(CompactContacts);

		if (deadContacts)
		{
			D_TRACKTIME();
			// this could be parallelized, monitor it to see if is worth doing it.
			const ndInt32 start = activeJoints + inactiveJoints;
			for (ndInt32 i = 0; i < deadContacts; ++i)
			{
				ndContact* const contact = m_contactArray[start + i];
				m_contactArray.DeleteContact(contact);
				delete contact;
			}
		}

		m_activeConstraintArray.SetCount(activeJoints);
		m_contactArray.SetCount(activeJoints + inactiveJoints);
	}
}

void ndScene::AddPair(ndBodyKinematic* const body0, ndBodyKinematic* const body1, ndInt32)
{
	ndContact* const contact = (body0->GetInvMass() != ndFloat32(0.0f)) ? body0->FindContact(body1) : body1->FindContact(body0);;
	dAssert(!contact || (contact->m_body1->FindContact(contact->m_body0) == contact));

	if (!contact)
	{
		const ndJointBilateralConstraint* const bilateral = FindBilateralJoint(body0, body1);

		const bool isCollidable = bilateral ? bilateral->IsCollidable() : true;
		if (isCollidable)
		{
			ndContact* const newContact = m_contactArray.CreateContact(body0, body1);
			dAssert(newContact->m_body0->GetInvMass() != ndFloat32(0.0f));
			newContact->m_material = m_contactNotifyCallback->GetMaterial(newContact, body0->GetCollisionShape(), body1->GetCollisionShape());
		}
	}
}

#else

#if 0
void ndScene::AddPair(ndBodyKinematic* const body0, ndBodyKinematic* const body1, ndInt32 threadId)
{
	ndArray<ndContactPairs>& particalPairs = m_particalNewPairs[threadId];
	ndContactPairs pair(body0->m_index, body1->m_index);
	particalPairs.PushBack(pair);
}

void ndScene::CalculateContacts()
{
	D_TRACKTIME();
	enum ndPairGroup
	{
		m_existingJoint,
		m_newJoint,
		m_deadJoint,
		m_duplicateJoint,
	};

	class ndJointBody0_x
	{
		public:
		ndJointBody0_x(void* const)
		{
		}

		ndUnsigned32 GetKey(const ndContactPairs& pair) const
		{
			const ndUnsigned32 key = pair.m_body0 & 0xff;
			return key;
		}
	};

	class ndJointBody0_y
	{
		public:
		ndJointBody0_y(void* const)
		{
		}

		ndUnsigned32 GetKey(const ndContactPairs& pair) const
		{
			const ndUnsigned32 key = (pair.m_body0 >> 8) & 0xff;
			return key;
		}
	};

	class ndJointBody1_x
	{
		public:
		ndJointBody1_x(void* const)
		{
		}

		ndUnsigned32 GetKey(const ndContactPairs& pair) const
		{
			const ndUnsigned32 key = pair.m_body1 & 0xff;
			return key;
		}
	};

	class ndJointBody1_y
	{
		public:
		ndJointBody1_y(void* const)
		{
		}

		ndUnsigned32 GetKey(const ndContactPairs& pair) const
		{
			const ndUnsigned32 key = (pair.m_body1 >> 8) & 0xff;
			return key;
		}
	};

	class ndJointClassify
	{
		public:
		ndJointClassify(void* const scene)
			:m_scene((ndScene*)scene)
			,m_bodyArray(&m_scene->GetActiveBodyArray()[0])
		{
		}

		ndUnsigned32 GetKey(const ndContactPairs& pair) const
		{
			const ndContactPairs* const ptr = &pair;

			if ((pair.m_body0 == ptr[1].m_body0) && (pair.m_body1 == ptr[1].m_body1))
			{
				dAssert(!pair.m_contact);
				return m_duplicateJoint;
			}
			else if (!pair.m_contact)
			{
				ndBodyKinematic* const body0 = m_bodyArray[pair.m_body0];
				ndBodyKinematic* const body1 = m_bodyArray[pair.m_body1];
				dAssert(ndUnsigned32(body0->m_index) == pair.m_body0);
				dAssert(ndUnsigned32(body1->m_index) == pair.m_body1);
				const ndJointBilateralConstraint* const bilateral = m_scene->FindBilateralJoint(body0, body1);
				const bool isCollidable = bilateral ? bilateral->IsCollidable() : true;
				return isCollidable ? m_newJoint : m_duplicateJoint;
			}

			return pair.m_contact->m_isDead ? m_deadJoint : m_existingJoint;
		}

		ndScene* m_scene;
		ndBodyKinematic** m_bodyArray;
	};

	class ndJointActive
	{
		public:
		ndJointActive(void* const scene)
			:m_scene((ndScene*)scene)
			,m_bodyArray(&m_scene->GetActiveBodyArray()[0])
		{
		}

		ndUnsigned32 GetKey(const ndContactPairs& pair) const
		{
			const bool inactive = !pair.m_contact->IsActive() | !pair.m_contact->m_maxDOF;
			return inactive ? 1 : 0;
		}

		ndScene* m_scene;
		ndBodyKinematic** m_bodyArray;
	};

	ndInt32 newContactCount = 0;
	const ndInt32 threadCount = GetThreadCount();
	for (ndInt32 i = 0; i < threadCount; ++i)
	{
		newContactCount += m_particalNewPairs[i].GetCount();
	}

	ndInt32 contactCount = m_contactArray.GetCount();
	m_scratchBuffer.SetCount((2 * (contactCount + newContactCount) + 16) * sizeof(ndContactPairs));

	ndContactPairs* srcPtr = (ndContactPairs*)&m_scratchBuffer[0];
	ndContactPairs* dstPtr = &srcPtr[contactCount + newContactCount + 8];
	
	ndUnsigned32 scanCounts[D_MAX_THREADS_COUNT + 1];

	auto CopyPartialCounts = ndMakeObject::ndFunction([this, srcPtr, &scanCounts](ndInt32 threadIndex, ndInt32)
	{
		D_TRACKTIME();
		const ndArray<ndContactPairs>& newPairs = m_particalNewPairs[threadIndex];

		const ndUnsigned32 start = scanCounts[threadIndex];
		dAssert((scanCounts[threadIndex + 1] - start) == ndUnsigned32 (newPairs.GetCount()));
		for (ndInt32 i = 0; i < newPairs.GetCount(); ++i)
		{
			srcPtr[start + i] = newPairs[i];
		}
	});

	ndUnsigned32 sum = 0;
	auto CopyContactArray = ndMakeObject::ndFunction([this, srcPtr, &sum](ndInt32 threadIndex, ndInt32 threadCount)
	{
		D_TRACKTIME();
		ndContact** const contactArray = &m_contactArray[0];
		const ndUnsigned32 count = m_contactArray.GetCount();
		const ndUnsigned32 start = sum;
		const ndStartEnd startEnd(count, threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndContact* const contact = contactArray[i];
			ndInt32 i0 = contact->m_body0->m_index;
			ndInt32 i1 = contact->m_body1->m_index;
			srcPtr[start + i] = ndContactPairs(i0, i1, contact);
		}
	});

	for (ndInt32 i = 0; i < threadCount; ++i)
	{
		const ndArray<ndContactPairs>& newPairs = m_particalNewPairs[i];
		scanCounts[i] = sum;
		sum += newPairs.GetCount();
	}
	scanCounts[threadCount] = sum;
	ParallelExecute(CopyPartialCounts);
	if (contactCount)
	{
		ParallelExecute(CopyContactArray);
	}

	contactCount += sum;
	srcPtr[contactCount] = ndContactPairs(0x7fffffff, 0x7fffffff);
	dstPtr[contactCount] = ndContactPairs(0x7fffffff, 0x7fffffff);

	ndUnsigned32 prefixScan[8];
	//memset(prefixScan, 0, sizeof(prefixScan));

	ndCountingSort<ndContactPairs, ndJointBody0_x, 8>(*this, srcPtr, dstPtr, contactCount, nullptr, nullptr);
	ndSwap(srcPtr, dstPtr);

	ndCountingSort<ndContactPairs, ndJointBody0_y, 8>(*this, srcPtr, dstPtr, contactCount, nullptr, nullptr);
	ndSwap(srcPtr, dstPtr);

	ndCountingSort<ndContactPairs, ndJointBody1_x, 8>(*this, srcPtr, dstPtr, contactCount, nullptr, nullptr);
	ndSwap(srcPtr, dstPtr);

	ndCountingSort<ndContactPairs, ndJointBody1_y, 8>(*this, srcPtr, dstPtr, contactCount, nullptr, nullptr);
	ndSwap(srcPtr, dstPtr);

	ndCountingSort<ndContactPairs, ndJointClassify, 2>(*this, srcPtr, dstPtr, contactCount, prefixScan, this);
	ndSwap(srcPtr, dstPtr);

	if (prefixScan[m_deadJoint + 1] > prefixScan[m_deadJoint])
	{	
		// delete dead joints;
		auto DeleteContactArray = ndMakeObject::ndFunction([this, srcPtr, &prefixScan](ndInt32 threadIndex, ndInt32 threadCount)
		{
			D_TRACKTIME();
			const ndUnsigned32 start = prefixScan[m_deadJoint];
			const ndUnsigned32 count = prefixScan[m_deadJoint + 1] - start;

			const ndStartEnd startEnd(count, threadIndex, threadCount);
			for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
			{
				ndContactPairs& pair = srcPtr[start + i];
				dAssert(pair.m_contact);
				dAssert(pair.m_contact->m_isDead);
				if (pair.m_contact->m_isAttached)
				{
					pair.m_contact->DetachFromBodies();
				}
				pair.m_contact->m_isDead = 1;
				delete pair.m_contact;
			}
		});
		ParallelExecute(DeleteContactArray);
	}

	if (prefixScan[m_newJoint + 1] > prefixScan[m_newJoint])
	{
		// create new joints;
		auto CreateContact = ndMakeObject::ndFunction([this, srcPtr, &prefixScan](ndInt32 threadIndex, ndInt32 threadCount)
		{
			D_TRACKTIME();
			ndBodyKinematic** const bodyArray = &GetActiveBodyArray()[0];

			const ndUnsigned32 start = prefixScan[m_newJoint];
			const ndUnsigned32 count = prefixScan[m_newJoint + 1] - start;

			const ndStartEnd startEnd(count, threadIndex, threadCount);
			for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
			{
				ndContactPairs& pair = srcPtr[start + i];
				ndBodyKinematic* const body0 = bodyArray[pair.m_body0];
				ndBodyKinematic* const body1 = bodyArray[pair.m_body1];
				dAssert(!pair.m_contact);
				dAssert(ndUnsigned32(body0->m_index) == pair.m_body0);
				dAssert(ndUnsigned32(body1->m_index) == pair.m_body1);

				pair.m_contact = new ndContact;
				pair.m_contact->SetBodies(body0, body1);
				pair.m_contact->AttachToBodies();

				dAssert(pair.m_contact->m_body0->GetInvMass() != ndFloat32(0.0f));
				pair.m_contact->m_material = m_contactNotifyCallback->GetMaterial(pair.m_contact, body0->GetCollisionShape(), body1->GetCollisionShape());
			}
		});
		ParallelExecute(CreateContact);
	}

	if (prefixScan[m_newJoint + 1])
	{
		// calculate joint contacts;
		m_contactArray.SetCount(prefixScan[m_newJoint + 1]);
		auto CalculateContactPoints = ndMakeObject::ndFunction([this, srcPtr, &prefixScan](ndInt32 threadIndex, ndInt32 threadCount)
		{
			D_TRACKTIME();
			const ndUnsigned32 jointCount = prefixScan[m_newJoint + 1];

			const ndStartEnd startEnd(jointCount, threadIndex, threadCount);
			for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
			{
				ndContactPairs& pair = srcPtr[i];
				dAssert(pair.m_contact);
				CalculateContacts(threadIndex, pair.m_contact);
				m_contactArray[i] = pair.m_contact;
			}
		});
		ParallelExecute(CalculateContactPoints);

		ndCountingSort<ndContactPairs, ndJointActive, 1>(*this, srcPtr, dstPtr, m_contactArray.GetCount(), prefixScan, this);
		ndSwap(srcPtr, dstPtr);

		m_activeConstraintArray.SetCount(prefixScan[1]);
		auto CopyActiveContact = ndMakeObject::ndFunction([this, srcPtr, &prefixScan](ndInt32 threadIndex, ndInt32 threadCount)
		{
			const ndUnsigned32 activeJointCount = prefixScan[1];
			ndArray<ndConstraint*>& activeConstraintArray = m_activeConstraintArray;
			const ndStartEnd startEnd(activeJointCount, threadIndex, threadCount);
			for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
			{
				ndContactPairs& pair = srcPtr[i];
				activeConstraintArray[i] = pair.m_contact;
			}
		});
		ParallelExecute(CopyActiveContact);
	}
}

#else

void ndScene::AddPair(ndBodyKinematic* const body0, ndBodyKinematic* const body1, ndInt32 threadId)
{
	const ndBodyKinematic::ndContactMap& contactMap0 = body0->GetContactMap();
	const ndBodyKinematic::ndContactMap& contactMap1 = body1->GetContactMap();

	ndContact* const contact = (contactMap0.GetCount() <= contactMap1.GetCount()) ? contactMap0.FindContact(body0, body1) : contactMap1.FindContact(body1, body0);
	//dAssert(!contact || (contact->m_body1->FindContact(contact->m_body0) == contact));
	if (!contact)
	{
		const ndJointBilateralConstraint* const bilateral = FindBilateralJoint(body0, body1);
		const bool isCollidable = bilateral ? bilateral->IsCollidable() : true;
		if (isCollidable)
		{
			ndArray<ndContactPairs>& particalPairs = m_particalNewPairs[threadId];
			ndContactPairs pair(body0->m_index, body1->m_index);

int xxx0 = body0->m_uniqueId;
int xxx1 = body1->m_uniqueId;
if (((xxx0 == 29) && (xxx1 == 10)) || ((xxx0 == 10) && (xxx1 == 29)))
{
	dAssert(0);
}

			particalPairs.PushBack(pair);
		}
	}
}

void ndScene::CalculateContacts()
{
	D_TRACKTIME();
	ndInt32 newContactCount = 0;
	const ndInt32 threadCount = GetThreadCount();
	for (ndInt32 i = 0; i < threadCount; ++i)
	{
		newContactCount += m_particalNewPairs[i].GetCount();
	}

	ndInt32 contactCount = m_contactArray.GetCount();
	m_scratchBuffer.SetCount((contactCount + newContactCount + 16) * sizeof(ndContact*));

	ndContact** const tmpJointsArray = (ndContact**)&m_scratchBuffer[0];

	ndUnsigned32 sum = 0;
	ndUnsigned32 scanCounts[D_MAX_THREADS_COUNT + 1];
	for (ndInt32 i = 0; i < threadCount; ++i)
	{
		const ndArray<ndContactPairs>& newPairs = m_particalNewPairs[i];
		scanCounts[i] = sum;
		sum += newPairs.GetCount();
	}
	scanCounts[threadCount] = sum;
	
	auto CopyPartialCounts = ndMakeObject::ndFunction([this, tmpJointsArray, &scanCounts](ndInt32 threadIndex, ndInt32)
	{
		D_TRACKTIME();
		const ndArray<ndContactPairs>& newPairs = m_particalNewPairs[threadIndex];
		ndBodyKinematic** const bodyArray = &GetActiveBodyArray()[0];

		const ndUnsigned32 count = newPairs.GetCount();
		const ndUnsigned32 start = scanCounts[threadIndex];
		dAssert((scanCounts[threadIndex + 1] - start) == ndUnsigned32(newPairs.GetCount()));
		for (ndUnsigned32 i = 0; i < count; ++i)
		{
			const ndContactPairs& pair = newPairs[i];

			ndBodyKinematic* const body0 = bodyArray[pair.m_body0];
			ndBodyKinematic* const body1 = bodyArray[pair.m_body1];

int xxx0 = body0->m_uniqueId;
int xxx1 = body1->m_uniqueId;
if (((xxx0 == 29) && (xxx1 == 10)) || ((xxx0 == 10) && (xxx1 == 29)))
{
	dAssert(0);
}

			dAssert(ndUnsigned32(body0->m_index) == pair.m_body0);
			dAssert(ndUnsigned32(body1->m_index) == pair.m_body1);
	
			ndContact* const contact = new ndContact;
			contact->SetBodies(body0, body1);
			contact->AttachToBodies();
	
			dAssert(contact->m_body0->GetInvMass() != ndFloat32(0.0f));
			contact->m_material = m_contactNotifyCallback->GetMaterial(contact, body0->GetCollisionShape(), body1->GetCollisionShape());
			tmpJointsArray[start + i] = contact;
		}
	});
	
	auto CopyContactArray = ndMakeObject::ndFunction([this, tmpJointsArray, &sum](ndInt32 threadIndex, ndInt32 threadCount)
	{
		D_TRACKTIME();
		ndContact** const contactArray = &m_contactArray[0];

		const ndUnsigned32 start = sum;
		const ndUnsigned32 count = m_contactArray.GetCount();
		
		const ndStartEnd startEnd(count, threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndContact* const contact = contactArray[i];
			tmpJointsArray[start + i] = contact;
		}
	});
	
	ParallelExecute(CopyPartialCounts);
	if (contactCount)
	{
		ParallelExecute(CopyContactArray);
	}

	m_contactArray.SetCount(contactCount + sum);
	if (m_contactArray.GetCount())
	{
		auto CalculateContactPoints = ndMakeObject::ndFunction([this, tmpJointsArray, &contactCount](ndInt32 threadIndex, ndInt32 threadCount)
		{
			D_TRACKTIME();
			const ndUnsigned32 jointCount = m_contactArray.GetCount();

			const ndStartEnd startEnd(jointCount, threadIndex, threadCount);
			for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
			{
				ndContact* const contact = tmpJointsArray[i];
				dAssert(contact);
				if (!contact->m_isDead)
				{
					CalculateContacts(threadIndex, contact);
				}
			}
		});
		ParallelExecute(CalculateContactPoints);

		enum ndPairGroup
		{
			m_active,
			m_inactive,
			m_dead,
		};

		class ndJointActive
		{
			public:
			ndJointActive(void* const)
			{
				m_code[0] = m_active;
				m_code[1] = m_inactive;
				m_code[2] = m_dead;
				m_code[3] = m_dead;
			}

			ndUnsigned32 GetKey(const ndContact* const contact) const
			{
				const ndUnsigned32 inactive = !contact->IsActive() | (contact->m_maxDOF ? 0 : 1);
				const ndUnsigned32 idDead = contact->m_isDead;
				return m_code[idDead * 2 + inactive];
			}

			ndUnsigned32 m_code[4];
		};

		ndUnsigned32 prefixScan[5];
		ndCountingSort<ndContact*, ndJointActive, 2>(*this, tmpJointsArray, &m_contactArray[0], m_contactArray.GetCount(), prefixScan, nullptr);

		if (prefixScan[m_dead + 1] != prefixScan[m_dead])
		{
			auto DeleteContactArray = ndMakeObject::ndFunction([this, &prefixScan](ndInt32 threadIndex, ndInt32 threadCount)
			{
				D_TRACKTIME();
				ndArray<ndContact*>& contactArray = m_contactArray;
				const ndUnsigned32 start = prefixScan[m_dead];
				const ndUnsigned32 count = prefixScan[m_dead + 1] - start;

				const ndStartEnd startEnd(count, threadIndex, threadCount);
				for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
				{
					ndContact* const contact = contactArray[start + i];

					int xxx0 = contact->m_body0->m_uniqueId;
					int xxx1 = contact->m_body1->m_uniqueId;
					if (((xxx0 == 29) && (xxx1 == 10)) || ((xxx0 == 10) && (xxx1 == 29)))
					{
						dAssert(0);
					}

					dAssert(contact->m_isDead);
					if (contact->m_isAttached)
					{
						contact->DetachFromBodies();
					}
					contact->m_isDead = 1;
					delete contact;
				}
			});

			ParallelExecute(DeleteContactArray);
			m_contactArray.SetCount(prefixScan[m_inactive + 1]);
		}

		m_activeConstraintArray.SetCount(prefixScan[m_active + 1]);
		if (m_activeConstraintArray.GetCount())
		{
			auto CopyActiveContact = ndMakeObject::ndFunction([this](ndInt32 threadIndex, ndInt32 threadCount)
			{
				const ndArray<ndContact*>& constraintArray = m_contactArray;
				ndArray<ndConstraint*>& activeConstraintArray = m_activeConstraintArray;
				const ndUnsigned32 activeJointCount = activeConstraintArray.GetCount();

				const ndStartEnd startEnd(activeJointCount, threadIndex, threadCount);
				for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
				{
					activeConstraintArray[i] = constraintArray[i];
				}
			});
			ParallelExecute(CopyActiveContact);
		}
	}
}

#endif

#endif