
#include "Trainer.hpp"
#include "common/Common.hpp"
#include "common/Timer.hpp"
#include "connectfour/GameAction.hpp"
#include "connectfour/GameRules.hpp"
#include "connectfour/GameState.hpp"
#include "learning/Constants.hpp"
#include "learning/ExperienceMemory.hpp"
#include "learning/LearningAgent.hpp"
#include "learning/RandomAgent.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <future>
#include <thread>
#include <vector>

using namespace learning;

static constexpr unsigned EXPERIENCE_MEMORY_SIZE = 100000;
static constexpr unsigned NUM_START_STATES = 10000000;

static constexpr float INITIAL_PRANDOM = 0.5f;
static constexpr float TARGET_PRANDOM = 0.05f;

static constexpr float INITIAL_TEMPERATURE = 1.0f;
static constexpr float TARGET_TEMPERATURE = 0.01f;

static constexpr float INITIAL_LEARN_RATE = 1.0f;
static constexpr float TARGET_LEARN_RATE = 0.5f;

static constexpr float WR_AVRG = 0.01f;

struct PlayoutAgent {
  LearningAgent *agent;
  ExperienceMemory *memory;

  vector<EVector> stateHistory;
  vector<GameAction> actionHistory;

  PlayoutAgent(LearningAgent *agent, ExperienceMemory *memory) : agent(agent), memory(memory) {}

  bool havePreviousState(void) { return stateHistory.size() > 0; }

  void addTransitionToMemory(EVector &curState, float reward, bool isTerminal) {
    if (!havePreviousState()) {
      return;
    }

    EVector &prevState = stateHistory[stateHistory.size() - 1];
    GameAction &performedAction = actionHistory[actionHistory.size() - 1];

    memory->AddExperience(
        ExperienceMoment(prevState, performedAction, curState, reward, isTerminal));
  }

  void addMoveToHistory(const EVector &state, const GameAction &action) {
    stateHistory.push_back(state);
    actionHistory.push_back(action);
  }
};

struct Trainer::TrainerImpl {
  std::vector<GameState> startStates;
  vector<ProgressCallback> callbacks;
  atomic<unsigned> numLearnIters;
  float winRateVsRandom = 0.0f;

  void AddProgressCallback(ProgressCallback callback) { callbacks.push_back(callback); }

  uptr<Agent> TrainAgent(unsigned iters) {
    generateStartStates();
    auto experienceMemory = make_unique<ExperienceMemory>(EXPERIENCE_MEMORY_SIZE);

    uptr<LearningAgent> agent = make_unique<LearningAgent>();
    trainAgent(agent.get(), experienceMemory.get(), iters, INITIAL_PRANDOM, TARGET_PRANDOM);

    return move(agent);
  }

  void trainAgent(LearningAgent *agent, ExperienceMemory *memory, unsigned iters,
                  float initialPRandom, float targetPRandom) {

    numLearnIters = 0;
    winRateVsRandom = 0.5f;

    std::thread playoutThread =
        startPlayoutThread(agent, memory, iters, initialPRandom, targetPRandom);
    std::thread learnThread = startLearnThread(agent, memory, iters);

    playoutThread.join();
    learnThread.join();
  }

  std::thread startPlayoutThread(LearningAgent *agent, ExperienceMemory *memory, unsigned iters,
                                 float initialPRandom, float targetPRandom) {

    return std::thread([this, agent, memory, iters, initialPRandom, targetPRandom]() {
      float pRandDecay = powf(targetPRandom / initialPRandom, 1.0f / iters);
      assert(pRandDecay > 0.0f && pRandDecay <= 1.0f);

      float tempDecay = powf(TARGET_TEMPERATURE / INITIAL_TEMPERATURE, 1.0f / iters);
      assert(tempDecay > 0.0f && tempDecay <= 1.0f);

      while (true) {
        unsigned doneIters = numLearnIters.load();
        if (doneIters >= iters) {
          break;
        }

        float prand = initialPRandom * powf(pRandDecay, doneIters);
        float temp = INITIAL_TEMPERATURE * powf(tempDecay, doneIters);

        agent->SetPRandom(prand);
        agent->SetTemperature(temp);

        if (rand() % 100 == 0) {
          this->playoutRoundVsRandom(agent, memory);
        } else {
          this->playoutRoundVsSelf(agent, memory);
        }

        if (doneIters % 1000 == 0) {
          std::cout << "win rate: " << winRateVsRandom << std::endl;
        }
      }
    });
  }

  std::thread startLearnThread(LearningAgent *agent, ExperienceMemory *memory, unsigned iters) {
    return std::thread([this, agent, memory, iters]() {
      while (memory->NumMemories() < 10 * MOMENTS_BATCH_SIZE) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }

      float learnRateDecay = powf(TARGET_LEARN_RATE / INITIAL_LEARN_RATE, 1.0f / iters);
      assert(learnRateDecay > 0.0f && learnRateDecay < 1.0f);

      for (unsigned i = 0; i < iters; i++) {
        float learnRate = INITIAL_LEARN_RATE * powf(learnRateDecay, i);
        agent->Learn(memory->Sample(MOMENTS_BATCH_SIZE), learnRate);
        this->numLearnIters++;

        if (i % 10000 == 0) {
          unsigned percentDone = (i * 100) / iters;
          std::cout << percentDone << "% ..." << std::endl;
        }
      }
    });
  }

  void playoutRoundVsSelf(LearningAgent *agent, ExperienceMemory *memory) {
    GameRules *rules = GameRules::Instance();
    GameState curState = startStates[rand() % startStates.size()];

    std::vector<PlayoutAgent> playoutAgents = {PlayoutAgent(agent, memory),
                                               PlayoutAgent(agent, memory)};
    unsigned curPlayerIndex = 0;
    while (true) {
      PlayoutAgent &curPlayer = playoutAgents[curPlayerIndex];
      PlayoutAgent &otherPlayer = playoutAgents[(curPlayerIndex + 1) % 2];

      EVector encodedState = LearningAgent::EncodeGameState(&curState);
      GameAction action = curPlayer.agent->SelectLearningAction(&curState, encodedState);

      curPlayer.addTransitionToMemory(encodedState, 0.0f, false);
      curPlayer.addMoveToHistory(encodedState, action);

      curState = curState.SuccessorState(action);

      switch (rules->GameCompletionState(curState)) {
      case CompletionState::WIN:
        encodedState = LearningAgent::EncodeGameState(&curState);
        curPlayer.addTransitionToMemory(encodedState, 1.0f, true);
        otherPlayer.addTransitionToMemory(encodedState, -1.0f, true);
        return;
      case CompletionState::LOSS:
        assert(false); // This actually shouldn't be possible.
        return;
      case CompletionState::DRAW:
        encodedState = LearningAgent::EncodeGameState(&curState);
        curPlayer.addTransitionToMemory(encodedState, 0.0f, true);
        otherPlayer.addTransitionToMemory(encodedState, 0.0f, true);
        return;
      case CompletionState::UNFINISHED:
        curState.FlipState();
        curPlayerIndex = (curPlayerIndex + 1) % 2;
        break;
      }
    }
  }

  void playoutRoundVsRandom(LearningAgent *agent, ExperienceMemory *memory) {
    GameRules *rules = GameRules::Instance();
    GameState curState(rules->InitialState());

    RandomAgent opponent;
    PlayoutAgent pagent(agent, memory);
    unsigned curPlayerIndex = rand() % 2;

    while (true) {

      EVector encodedState;
      GameAction action;
      if (curPlayerIndex == 0) {
        encodedState = LearningAgent::EncodeGameState(&curState);
        action = pagent.agent->SelectLearningAction(&curState, encodedState);

        if (pagent.havePreviousState()) {
          pagent.addTransitionToMemory(encodedState, 0.0f, false);
        }

        pagent.addMoveToHistory(encodedState, action);
      } else {
        action = opponent.SelectAction(&curState);
      }
      curState = curState.SuccessorState(action);

      switch (rules->GameCompletionState(curState)) {
      case CompletionState::WIN:
        if (curPlayerIndex == 0) {
          winRateVsRandom = (1.0f - WR_AVRG) * winRateVsRandom + WR_AVRG * 1.0f;
        } else {
          winRateVsRandom = (1.0f - WR_AVRG) * winRateVsRandom + WR_AVRG * 0.0f;
        }

        encodedState = LearningAgent::EncodeGameState(&curState);
        pagent.addTransitionToMemory(encodedState, curPlayerIndex == 0 ? 1.0f : -1.0f, true);

        return;
      case CompletionState::LOSS:
        assert(false); // This actually shouldn't be possible.
        return;
      case CompletionState::DRAW:
        encodedState = LearningAgent::EncodeGameState(&curState);
        pagent.addTransitionToMemory(encodedState, 0.0f, true);
        return;
      case CompletionState::UNFINISHED:
        curState.FlipState();
        curPlayerIndex = (curPlayerIndex + 1) % 2;
      }
    }
  }

  void generateStartStates(void) {
    startStates.reserve(NUM_START_STATES);
    GameRules *rules = GameRules::Instance();

    RandomAgent agent;
    while (true) {
      std::vector<GameState> states;

      GameState curState(rules->InitialState());
      bool isFinished = false;

      while (!isFinished) {
        states.push_back(curState);
        GameAction action = agent.SelectAction(&curState);
        curState = curState.SuccessorState(action);

        switch (rules->GameCompletionState(curState)) {
        case CompletionState::WIN:
        case CompletionState::LOSS:
        case CompletionState::DRAW:
          isFinished = true;
          break;
        case CompletionState::UNFINISHED:
          curState.FlipState();
          break;
        }
      }

      for (int j = 0; j < static_cast<int>(states.size()) - 2; j++) {
        startStates.push_back(states[j]);
        if (startStates.size() >= NUM_START_STATES) {
          return;
        }
      }
    }
  }
};

Trainer::Trainer() : impl(new TrainerImpl()) {}
Trainer::~Trainer() = default;

void Trainer::AddProgressCallback(ProgressCallback callback) {
  impl->AddProgressCallback(callback);
}

uptr<Agent> Trainer::TrainAgent(unsigned iters) { return impl->TrainAgent(iters); }
