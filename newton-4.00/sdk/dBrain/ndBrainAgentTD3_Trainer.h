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

#ifndef _ND_BRAIN_AGENT_TD3_TRAINER_H__
#define _ND_BRAIN_AGENT_TD3_TRAINER_H__

#include "ndBrainStdafx.h"
#include "ndBrain.h"
#include "ndBrainAgent.h"
#include "ndBrainTrainer.h"
#include "ndBrainReplayBuffer.h"
#include "ndBrainLossLeastSquaredError.h"

// this is an implementation of more stable policy gradient for
// continues action controller using deep re enforcement learning. 
// td3 algorithm as described here: 
// https://arxiv.org/pdf/1802.09477.pdf

template<ndInt32 statesDim, ndInt32 actionDim>
class ndBrainAgentTD3_Trainer : public ndBrainAgent, public ndBrainThreadPool
{
	public:

	class HyperParameters
	{
		public:
		HyperParameters()
		{
			m_numberOfHiddenLayers = 3;
			m_hiddenLayersNumberOfNeuron = 64;

			m_bashBufferSize = 32;
			m_replayBufferSize = 1024 * 512;
			m_replayBufferPrefill = 1024 * 16;

			m_discountFactor = ndBrainFloat(0.99f);
			m_regularizer = ndBrainFloat(1.0e-6f);
			m_actorLearnRate = ndBrainFloat(0.0002f);
			m_criticLearnRate = ndBrainFloat(0.001f);
			m_softTargetFactor = ndBrainFloat(1.0e-3f);
			m_actionNoiseVariance = ndBrainFloat(0.05f);
			m_threadsCount = ndMin(ndBrainThreadPool::GetMaxThreads(), m_bashBufferSize / 2);
		}

		ndBrainFloat m_discountFactor;
		ndBrainFloat m_regularizer;
		ndBrainFloat m_actorLearnRate;
		ndBrainFloat m_criticLearnRate;
		ndBrainFloat m_softTargetFactor;
		ndBrainFloat m_actionNoiseVariance;

		ndInt32 m_threadsCount;
		ndInt32 m_bashBufferSize;
		ndInt32 m_replayBufferSize;
		ndInt32 m_replayBufferPrefill;
		ndInt32 m_numberOfHiddenLayers;
		ndInt32 m_hiddenLayersNumberOfNeuron;
	};

	ndBrainAgentTD3_Trainer(const HyperParameters& hyperParameters);
	~ndBrainAgentTD3_Trainer();

	ndInt32 GetFramesCount() const;
	ndInt32 GetEposideCount() const;
	ndInt32 GetEpisodeFrames() const;
	ndBrainFloat GetCurrentValue() const;

	ndBrainFloat GetLearnRate() const;
	void SetLearnRate(ndBrainFloat learnRate);

	ndBrainFloat GetActionNoise() const;
	void SetActionNoise(ndBrainFloat  noiseVaraince);

	protected:
	void Step();
	void Optimize();
	void OptimizeStep();
	bool IsTrainer() const;
	bool IsSampling() const;
	bool IsTerminal() const;
	ndBrainFloat GetReward() const;
	void SetBufferSize(ndInt32 size);
	void Save(ndBrainSave* const loadSave) const;

	void BackPropagate();
	void AddExploration(ndBrainFloat* const actions);
	void BackPropagateActor(const ndUnsigned32* const bashIndex);
	void BackPropagateCritic(const ndUnsigned32* const bashIndex);
	void CalculateQvalue(const ndBrainVector& state, const ndBrainVector& actions);

	void InitWeights();
	void InitWeights(ndBrainFloat weighVariance, ndBrainFloat biasVariance);

	ndBrain m_actor;
	ndBrain m_critic0;
	ndBrain m_critic1;

	ndBrain m_targetActor;
	ndBrain m_targetCritic0;
	ndBrain m_targetCritic1;

	ndBrainOptimizerAdam* m_actorOptimizer;
	ndBrainOptimizerAdam* m_criticOptimizer0;
	ndBrainOptimizerAdam* m_criticOptimizer1;

	ndArray<ndBrainTrainer*> m_actorTrainers;
	ndArray<ndBrainTrainer*> m_criticTrainers0;
	ndArray<ndBrainTrainer*> m_criticTrainers1;

	ndArray<ndInt32> m_bashSamples;
	ndBrainReplayBuffer<statesDim, actionDim> m_replayBuffer;
	ndBrainReplayTransitionMemory<statesDim, actionDim> m_currentTransition;

	ndBrainFloat m_currentQvalue;
	ndBrainFloat m_discountFactor;
	ndBrainFloat m_actorLearnRate;
	ndBrainFloat m_criticLearnRate;
	ndBrainFloat m_softTargetFactor;
	ndBrainFloat m_actionNoiseVariance;
	ndInt32 m_frameCount;
	ndInt32 m_framesAlive;
	ndInt32 m_eposideCount;
	ndInt32 m_bashBufferSize;
	ndInt32 m_replayBufferPrefill;
	bool m_collectingSamples;
};

template<ndInt32 statesDim, ndInt32 actionDim>
ndBrainAgentTD3_Trainer<statesDim, actionDim>::ndBrainAgentTD3_Trainer(const HyperParameters& hyperParameters)
	:ndBrainAgent()
	,ndBrainThreadPool()
	,m_actor()
	,m_critic0()
	,m_critic1()
	,m_targetActor()
	,m_targetCritic0()
	,m_targetCritic1()
	,m_actorOptimizer(nullptr)
	,m_criticOptimizer0(nullptr)
	,m_criticOptimizer1(nullptr)
	,m_bashSamples()
	,m_replayBuffer()
	,m_currentTransition()
	,m_currentQvalue(ndBrainFloat(0.0f))
	,m_discountFactor(hyperParameters.m_discountFactor)
	,m_actorLearnRate(hyperParameters.m_actorLearnRate)
	,m_criticLearnRate(hyperParameters.m_criticLearnRate)
	,m_softTargetFactor(hyperParameters.m_softTargetFactor)
	,m_actionNoiseVariance(hyperParameters.m_actionNoiseVariance)
	,m_frameCount(0)
	,m_framesAlive(0)
	,m_eposideCount(0)
	,m_bashBufferSize(hyperParameters.m_bashBufferSize)
	,m_replayBufferPrefill(hyperParameters.m_replayBufferPrefill)
	,m_collectingSamples(true)
{

	ndFixSizeArray<ndBrainLayer*, 32> layers;
	layers.SetCount(0);
	layers.PushBack(new ndBrainLayerLinear(m_stateSize, hyperParameters.m_hiddenLayersNumberOfNeuron));
	layers.PushBack(new ndBrainLayerTanhActivation(layers[layers.GetCount() - 1]->GetOutputSize()));
	for (ndInt32 i = 1; i < hyperParameters.m_numberOfHiddenLayers; ++i)
	{
		ndAssert(layers[layers.GetCount() - 1]->GetOutputSize() == hyperParameters.m_hiddenLayersNumberOfNeuron);
		layers.PushBack(new ndBrainLayerLinear(hyperParameters.m_hiddenLayersNumberOfNeuron, hyperParameters.m_hiddenLayersNumberOfNeuron));
		layers.PushBack(new ndBrainLayerTanhActivation(hyperParameters.m_hiddenLayersNumberOfNeuron));
	}
	layers.PushBack(new ndBrainLayerLinear(hyperParameters.m_hiddenLayersNumberOfNeuron, m_actionsSize));
	layers.PushBack(new ndBrainLayerTanhActivation(m_actionsSize));
	for (ndInt32 i = 0; i < layers.GetCount(); ++i)
	{
		m_actor.AddLayer(layers[i]);
		m_targetActor.AddLayer(layers[i]->Clone());
	}

	// the critic is more complex since is deal with more complex inputs
	layers.SetCount(0);
	layers.PushBack(new ndBrainLayerLinear(m_stateSize + m_actionsSize, hyperParameters.m_hiddenLayersNumberOfNeuron * 2));
	layers.PushBack(new ndBrainLayerTanhActivation(layers[layers.GetCount() - 1]->GetOutputSize()));
	for (ndInt32 i = 1; i < hyperParameters.m_numberOfHiddenLayers; ++i)
	{
		ndAssert(layers[layers.GetCount() - 1]->GetOutputSize() == hyperParameters.m_hiddenLayersNumberOfNeuron);
		layers.PushBack(new ndBrainLayerLinear(hyperParameters.m_hiddenLayersNumberOfNeuron * 2, hyperParameters.m_hiddenLayersNumberOfNeuron * 2));
		layers.PushBack(new ndBrainLayerTanhActivation(hyperParameters.m_hiddenLayersNumberOfNeuron * 2));
	}
	layers.PushBack(new ndBrainLayerLinear(hyperParameters.m_hiddenLayersNumberOfNeuron * 2, 1));
	for (ndInt32 i = 0; i < layers.GetCount(); ++i)
	{
		m_critic0.AddLayer(layers[i]);
		m_critic1.AddLayer(layers[i]->Clone());
		m_targetCritic0.AddLayer(layers[i]->Clone());
		m_targetCritic1.AddLayer(layers[i]->Clone());
	}

	ndAssert(m_critic0.GetOutputSize() == 1);
	ndAssert(m_critic1.GetOutputSize() == 1);
	ndAssert(m_critic0.GetInputSize() == (m_actor.GetInputSize() + m_actor.GetOutputSize()));
	ndAssert(m_critic1.GetInputSize() == (m_actor.GetInputSize() + m_actor.GetOutputSize()));
	ndAssert(!strcmp((m_actor[m_actor.GetCount() - 1])->GetLabelId(), "ndBrainLayerTanhActivation"));

	m_actorTrainers.SetCount(0);
	m_criticTrainers0.SetCount(0);
	m_criticTrainers1.SetCount(0);
	SetThreadCount(hyperParameters.m_threadsCount);
	//SetThreadCount(1);

	for (ndInt32 i = 0; i < m_bashBufferSize; ++i)
	{
		m_actorTrainers.PushBack(new ndBrainTrainer(&m_actor));
		m_criticTrainers0.PushBack(new ndBrainTrainer(&m_critic0));
		m_criticTrainers1.PushBack(new ndBrainTrainer(&m_critic1));
	}
	
	SetBufferSize(hyperParameters.m_replayBufferSize);
	InitWeights();

	m_actorOptimizer = new ndBrainOptimizerAdam();
	m_criticOptimizer0 = new ndBrainOptimizerAdam();
	m_criticOptimizer1 = new ndBrainOptimizerAdam();
	
	m_actorOptimizer->SetRegularizer(hyperParameters.m_regularizer);
	m_criticOptimizer0->SetRegularizer(hyperParameters.m_regularizer);
	m_criticOptimizer1->SetRegularizer(hyperParameters.m_regularizer);
}

template<ndInt32 statesDim, ndInt32 actionDim>
ndBrainAgentTD3_Trainer<statesDim, actionDim>::~ndBrainAgentTD3_Trainer()
{
	for (ndInt32 i = 0; i < m_actorTrainers.GetCount(); ++i)
	{
		delete m_actorTrainers[i];
		delete m_criticTrainers0[i];
		delete m_criticTrainers1[i];
	}

	delete m_actorOptimizer;
	delete m_criticOptimizer0;
	delete m_criticOptimizer1;
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::Save(ndBrainSave* const loadSave) const
{
	loadSave->Save(&m_actor);
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::SetBufferSize(ndInt32 size)
{
	m_replayBuffer.SetSize(size);
}

template<ndInt32 statesDim, ndInt32 actionDim>
bool ndBrainAgentTD3_Trainer<statesDim, actionDim>::IsTrainer() const
{
	return true;
}

template<ndInt32 statesDim, ndInt32 actionDim>
bool ndBrainAgentTD3_Trainer<statesDim, actionDim>::IsSampling() const
{
	return m_collectingSamples;
}

template<ndInt32 statesDim, ndInt32 actionDim>
ndBrainFloat ndBrainAgentTD3_Trainer<statesDim, actionDim>::GetCurrentValue() const
{
	return m_currentQvalue;
}

template<ndInt32 statesDim, ndInt32 actionDim>
ndInt32 ndBrainAgentTD3_Trainer<statesDim, actionDim>::GetFramesCount() const
{
	return m_frameCount;
}

template<ndInt32 statesDim, ndInt32 actionDim>
ndInt32 ndBrainAgentTD3_Trainer<statesDim, actionDim>::GetEposideCount() const
{
	return m_eposideCount;
}

template<ndInt32 statesDim, ndInt32 actionDim>
ndInt32 ndBrainAgentTD3_Trainer<statesDim, actionDim>::GetEpisodeFrames() const
{
	return m_framesAlive;
}

template<ndInt32 statesDim, ndInt32 actionDim>
ndBrainFloat ndBrainAgentTD3_Trainer<statesDim, actionDim>::GetActionNoise() const
{
	return m_actionNoiseVariance;
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::SetActionNoise(ndBrainFloat noiseVariance)
{
	m_actionNoiseVariance = noiseVariance;
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::InitWeights()
{
	m_actor.InitWeightsXavierMethod();
	m_critic0.InitWeightsXavierMethod();
	m_critic1.InitWeightsXavierMethod();

	m_targetActor.CopyFrom(m_actor);
	m_targetCritic0.CopyFrom(m_critic0);
	m_targetCritic1.CopyFrom(m_critic1);
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::InitWeights(ndBrainFloat weighVariance, ndBrainFloat biasVariance)
{
	m_actor.InitWeights(weighVariance, biasVariance);
	m_critic0.InitWeights(weighVariance, biasVariance);
	m_critic1.InitWeights(weighVariance, biasVariance);

	m_targetActor.CopyFrom(m_actor);
	m_targetCritic0.CopyFrom(m_critic0);
	m_targetCritic1.CopyFrom(m_critic1);
}

template<ndInt32 statesDim, ndInt32 actionDim>
bool ndBrainAgentTD3_Trainer<statesDim, actionDim>::IsTerminal() const
{
	ndAssert(0);
	return false;
}

template<ndInt32 statesDim, ndInt32 actionDim>
ndBrainFloat ndBrainAgentTD3_Trainer<statesDim, actionDim>::GetReward() const
{
	ndAssert(0);
	return ndBrainFloat(0.0f);
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::AddExploration(ndBrainFloat* const actions)
{
	for (ndInt32 i = 0; i < actionDim; ++i)
	{
		ndBrainFloat actionNoise = ndBrainFloat(ndGaussianRandom(ndFloat32(actions[i]), ndFloat32(m_actionNoiseVariance)));
		actions[i] = actionNoise;
	}
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::BackPropagateCritic(const ndUnsigned32* const shuffleBuffer)
{
	auto PropagateBash = ndMakeObject::ndFunction([this, &shuffleBuffer](ndInt32 threadIndex, ndInt32 threadCount)
	{
		class Loss : public ndBrainLossLeastSquaredError
		{
			public:
			Loss(ndBrainTrainer& trainer, ndBrain* const targetCritic, const ndBrainReplayBuffer<statesDim, actionDim>& replayBuffer)
				:ndBrainLossLeastSquaredError(trainer.GetBrain()->GetOutputSize())
				,m_criticTrainer(trainer)
				,m_targetCritic(targetCritic)
				,m_replayBuffer(replayBuffer)
				,m_reward(0.0f)
			{
			}

			void GetLoss(const ndBrainVector& output, ndBrainVector& loss)
			{
				ndAssert(loss.GetCount() == 1);
				ndAssert(output.GetCount() == 1);
				ndAssert(m_truth.GetCount() == m_criticTrainer.GetBrain()->GetOutputSize());

				ndBrainFloat criticOutputBuffer[2];
				ndBrainMemVector criticOutput(criticOutputBuffer, 1);

				criticOutput[0] = m_reward;
				SetTruth(criticOutput);
				ndBrainLossLeastSquaredError::GetLoss(output, loss);
			}

			ndBrainTrainer& m_criticTrainer;
			ndBrain* m_targetCritic;
			const ndBrainReplayBuffer<statesDim, actionDim>& m_replayBuffer;
			ndBrainFloat m_reward;
		};

		ndBrainFloat criticOutputBuffer[2];
		ndBrainFloat targetInputBuffer[statesDim * 2];
		ndBrainFloat nextStateOutputBuffer[actionDim * 2];
		ndBrainFloat criticInputBuffer[(statesDim + actionDim) * 2];

		ndBrainMemVector criticOutput(criticOutputBuffer, 1);
		ndBrainMemVector targetInput(targetInputBuffer, statesDim);
		ndBrainMemVector nextStateOutput(nextStateOutputBuffer, actionDim);
		ndBrainMemVector criticInput(criticInputBuffer, statesDim + actionDim);

		const ndStartEnd startEnd(m_bashBufferSize, threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndBrainTrainer& trainer0 = *m_criticTrainers0[i];
			ndBrainTrainer& trainer1 = *m_criticTrainers1[i];
			Loss loss0(trainer0, &m_targetCritic0, m_replayBuffer);
			Loss loss1(trainer1, &m_targetCritic1, m_replayBuffer);
		
			ndInt32 index = ndInt32(shuffleBuffer[i]);
			const ndBrainReplayTransitionMemory<statesDim, actionDim>& transition = m_replayBuffer[index];
		
			m_targetActor.MakePrediction(transition.m_nextObservation, nextStateOutput);
			ndMemCpy(&criticInput[0], &transition.m_nextObservation[0], statesDim);
			ndMemCpy(&criticInput[statesDim], &nextStateOutput[0], actionDim);
		
			ndBrainFloat targetValue = transition.m_reward;
			if (!transition.m_terminalState)
			{
				m_targetCritic1.MakePrediction(criticInput, criticOutput);
				ndBrainFloat value1 = criticOutput[0];
		
				m_targetCritic0.MakePrediction(criticInput, criticOutput);
				ndBrainFloat value0 = criticOutput[0];
		
				targetValue = transition.m_reward + m_discountFactor * ndMin (value0, value1);
			}
			loss0.m_reward = targetValue;
			loss1.m_reward = targetValue;
		
			ndMemCpy(&criticInput[0], &transition.m_observation[0], statesDim);
			ndMemCpy(&criticInput[statesDim], &transition.m_action[0], actionDim);
		
			trainer0.BackPropagate(criticInput, loss0);
			trainer1.BackPropagate(criticInput, loss1);
		}
	});

	ndBrainThreadPool::ParallelExecute(PropagateBash);
	m_criticOptimizer0->Update(this, m_criticTrainers0, m_criticLearnRate);
	m_criticOptimizer1->Update(this, m_criticTrainers1, m_criticLearnRate);

	m_targetCritic0.SoftCopy(m_critic0, m_softTargetFactor);
	m_targetCritic1.SoftCopy(m_critic1, m_softTargetFactor);
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::BackPropagateActor(const ndUnsigned32* const shuffleBuffer)
{
	auto PropagateBash = ndMakeObject::ndFunction([this, &shuffleBuffer](ndInt32 threadIndex, ndInt32 threadCount)
	{
		class ActorLoss : public ndBrainLoss
		{
			public:
			ActorLoss(ndBrainTrainer& actorTrainer, ndBrainAgentTD3_Trainer<statesDim, actionDim>* const agent)
				:ndBrainLoss()
				,m_actorTrainer(actorTrainer)
				,m_agent(agent)
				,m_index(0)
			{
			}

			void GetLoss(const ndBrainVector& output, ndBrainVector& loss)
			{
				ndAssert(loss.GetCount() == actionDim);
				ndAssert(output.GetCount() == actionDim);
				const ndBrainReplayTransitionMemory<statesDim, actionDim>& transition = m_agent->m_replayBuffer[m_index];
				
				ndBrainFloat inputGradientBuffer[(statesDim + actionDim) * 2];
				ndBrainMemVector inputGradient(inputGradientBuffer, statesDim + actionDim);
				ndMemCpy(&inputGradient[0], &transition.m_observation[0], statesDim);
				ndMemCpy(&inputGradient[statesDim], &output[0], actionDim);
				m_agent->m_critic0.CalculateInputGradient(inputGradient, inputGradient);
				ndMemCpy(&loss[0], &inputGradient[statesDim], actionDim);
			}

			ndBrainTrainer& m_actorTrainer;
			ndBrainAgentTD3_Trainer<statesDim, actionDim>* m_agent;
			ndInt32 m_index;
		};

		const ndStartEnd startEnd(m_bashBufferSize, threadIndex, threadCount);
		for (ndInt32 i = startEnd.m_start; i < startEnd.m_end; ++i)
		{
			ndBrainTrainer& actorTrainer = *m_actorTrainers[i];
			ActorLoss loss(actorTrainer, this);
		
			ndInt32 index = ndInt32(shuffleBuffer[i]);
			const ndBrainReplayTransitionMemory<statesDim, actionDim>& transition = m_replayBuffer[index];
		
			loss.m_index = index;
			actorTrainer.BackPropagate(transition.m_observation, loss);
		}
	});

	ParallelExecute(PropagateBash);
	m_actorOptimizer->Update(this, m_actorTrainers, -m_actorLearnRate);
	m_targetActor.SoftCopy(m_actor, m_softTargetFactor);
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::BackPropagate()
{
	ndFixSizeArray<ndUnsigned32, 1024> shuffleBuffer;
	for (ndInt32 i = 0; i < m_bashBufferSize; ++i)
	{
		shuffleBuffer.PushBack(ndRandInt() % m_replayBuffer.GetCount());
	}

	BackPropagateCritic(&shuffleBuffer[0]);

#if 0
	BackPropagateActor(&shuffleBuffer[0]);
#else
	if (m_frameCount & 1)
	{
		BackPropagateActor(&shuffleBuffer[0]);
	}
#endif
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::Optimize()
{
	BackPropagate();
	if (IsSampling())
	{
		ndExpandTraceMessage("%d start training: episode %d\n", m_frameCount, m_eposideCount);
	}
	m_collectingSamples = false;
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::CalculateQvalue(const ndBrainVector& state, const ndBrainVector& actions)
{
	ndBrainFloat currentQValueBuffer[2];
	ndBrainFloat buffer[(statesDim + actionDim) * 2];

	ndBrainMemVector currentQValue(currentQValueBuffer, 1);
	ndBrainMemVector criticInput(buffer, statesDim + actionDim);

	ndMemCpy(&criticInput[0], &state[0], statesDim);
	ndMemCpy(&criticInput[statesDim], &actions[0], actionDim);

	m_critic1.MakePrediction(criticInput, currentQValue);
	ndBrainFloat reward1 = currentQValue[0];

	m_critic0.MakePrediction(criticInput, currentQValue);
	ndBrainFloat reward0 = currentQValue[0];

	m_currentQvalue = ndMin(reward0, reward1);
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::Step()
{
	GetObservation(&m_currentTransition.m_observation[0]);
	m_actor.MakePrediction(m_currentTransition.m_observation, m_currentTransition.m_action);

	// explore environment
	AddExploration(&m_currentTransition.m_action[0]);
	ApplyActions(&m_currentTransition.m_action[0]);

	m_currentTransition.m_reward = GetReward();
	if (!IsSampling())
	{
		// Get Q vale from Critic
		CalculateQvalue(m_currentTransition.m_observation, m_currentTransition.m_action);
	}
}

template<ndInt32 statesDim, ndInt32 actionDim>
void ndBrainAgentTD3_Trainer<statesDim, actionDim>::OptimizeStep()
{
	if (!m_frameCount)
	{
		ResetModel();
	}

	m_currentTransition.m_terminalState = IsTerminal();
	GetObservation(&m_currentTransition.m_nextObservation[0]);
	m_replayBuffer.AddTransition(m_currentTransition);

	if (m_frameCount > m_replayBufferPrefill)
	{
		Optimize();
	}

	if (m_currentTransition.m_terminalState)
	{
		ResetModel();
		if (IsSampling() && (m_eposideCount % 100 == 0))
		{
			ndExpandTraceMessage("collecting samples: frame %d out of %d, episode %d \n", m_frameCount, m_replayBuffer.GetCapacity(), m_eposideCount);
		}
		m_eposideCount++;
		m_framesAlive = 0;
	}

	m_frameCount++;
	m_framesAlive++;
}

#endif 
