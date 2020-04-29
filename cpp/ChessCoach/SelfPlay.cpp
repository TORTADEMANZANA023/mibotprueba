#include "SelfPlay.h"

#include <limits>
#include <cmath>
#include <limits>
#include <chrono>
#include <iostream>
#include <numeric>

#include <Stockfish/thread.h>
#include <Stockfish/uci.h>

#include "Config.h"
#include "Pgn.h"

TerminalValue TerminalValue::NonTerminal()
{
    return {};
}

int TerminalValue::Draw()
{
    return 0;
}

// Mate in N fullmoves, not halfmoves/ply.
int TerminalValue::MateIn(int n)
{
    return n;
}

// Opponent mate in N fullmoves, not halfmoves/ply.
int TerminalValue::OpponentMateIn(int n)
{
    return -n;
}

TerminalValue::TerminalValue()
    : _value()
    , _mateScore([](float) { return 0.f; })
{
}

TerminalValue::TerminalValue(const int value)
{
    operator=(value);
}

TerminalValue& TerminalValue::operator=(const int value)
{
    _value = value;

    if (value > 0)
    {
        const int mateNSaturated = std::min(static_cast<int>(Game::UcbMateTerm.size() - 1), value);
        const float mateTerm = Game::UcbMateTerm[mateNSaturated];
        _mateScore = [mateTerm](float explorationRate) { return explorationRate * mateTerm; };
    }
    else
    {
        // No adjustment for opponent-mate-in-N. The goal of the search in that situation is already
        // to go wide rather than deep and find some paths with value. Adding disincentives (with some
        // variation of inverse exploration rate coefficient) can help exhaustive searches finish in
        // fewer nodes in opponent-mate-in-N trees; however, the calculations slow down the search to
        // more processing time overall despite fewer nodes, and worse principle variations are preferred
        // before the exhaustive search finishes, because better priors get searched and disincentivized
        // sooner. So, rely on every-other-step mate-in-N incentives to help guide search, and SelectMove
        // preferring slower opponent mates (in the worst case).
        //
        // Also, no adjustment for draws at the moment.
        _mateScore = [](float) { return 0.f; };
    }

    return *this;
}

bool TerminalValue::operator==(const int other) const
{
    return (_value == other);
}

bool TerminalValue::IsImmediate() const
{
    return (_value &&
        ((*_value == Draw()) || (*_value == MateIn<1>())));
}

float TerminalValue::ImmediateValue() const
{
    // Coalesce a draw for the (Ply >= MaxMoves) and other undetermined/unfinished cases.
    if (_value == TerminalValue::MateIn<1>())
    {
        return CHESSCOACH_VALUE_WIN;
    }
    return CHESSCOACH_VALUE_DRAW;
}

bool TerminalValue::IsMateInN() const
{
    return (_value && (*_value > 0));
}

bool TerminalValue::IsOpponentMateInN() const
{
    return (_value && (*_value < 0));
}

int TerminalValue::MateN() const
{
    return (_value ? std::max(0, *_value) : 0);
}

int TerminalValue::OpponentMateN() const
{
    return (_value ? std::max(0, -*_value) : 0);
}

int TerminalValue::EitherMateN() const
{
    return (_value ? *_value : 0);
}

float TerminalValue::MateScore(float explorationRate) const
{
    return _mateScore(explorationRate);
}

thread_local PoolAllocator<Node, Node::BlockSizeBytes> Node::Allocator;

std::atomic_uint SelfPlayWorker::ThreadSeed;
thread_local std::default_random_engine SelfPlayWorker::Random(
    std::random_device()() +
    static_cast<unsigned int>(std::chrono::system_clock::now().time_since_epoch().count()) +
    ++ThreadSeed);

Node::Node(float setPrior)
    : bestChild(MOVE_NONE, nullptr)
    , prior(setPrior)
    , visitCount(0)
    , visitingCount(0)
    , valueSum(0.f)
    , terminalValue{}
    , expanding(false)
{
    assert(!std::isnan(setPrior));
}

void* Node::operator new(size_t count)
{
    return Allocator.Allocate();
}

void Node::operator delete(void* ptr) noexcept
{
    Allocator.Free(ptr);
}

bool Node::IsExpanded() const
{
    return !children.empty();
}

float Node::Value() const
{
    // First-play urgency (FPU) is zero, a loss.
    if (visitCount <= 0)
    {
        return CHESSCOACH_VALUE_LOSS;
    }

    return (valueSum / visitCount);
}

// TODO: Write a custom allocator for nodes (work out very maximum, then do some kind of ring/tree - important thing is all same size, capped number)
// TODO: Also input/output planes? e.g. for StoredGames vector storage

// Fast default-constructor with no resource ownership, used to size out vectors.
SelfPlayGame::SelfPlayGame()
    : _root(nullptr)
    , _tryHard(false)
    , _image(nullptr)
    , _value(nullptr)
    , _policy(nullptr)
    , _searchRootPly(0)
    , _result(CHESSCOACH_VALUE_UNINITIALIZED)
{
}

SelfPlayGame::SelfPlayGame(INetwork::InputPlanes* image, float* value, INetwork::OutputPlanes* policy)
    : Game()
    , _root(new Node(0.f))
    , _tryHard(false)
    , _image(image)
    , _value(value)
    , _policy(policy)
    , _searchRootPly(0)
    , _result(CHESSCOACH_VALUE_UNINITIALIZED)
{
}

SelfPlayGame::SelfPlayGame(const std::string& fen, const std::vector<Move>& moves, bool tryHard,
    INetwork::InputPlanes* image, float* value, INetwork::OutputPlanes* policy)
    : Game(fen, moves)
    , _root(new Node(0.f))
    , _tryHard(tryHard)
    , _image(image)
    , _value(value)
    , _policy(policy)
    , _searchRootPly(0)
    , _result(CHESSCOACH_VALUE_UNINITIALIZED)
{
}

SelfPlayGame::SelfPlayGame(const SelfPlayGame& other)
    : Game(other)
    , _root(other._root)
    , _tryHard(other._tryHard)
    , _image(other._image)
    , _value(other._value)
    , _policy(other._policy)
    , _searchRootPly(other.Ply())
    , _result(other._result)
{
    assert(&other != this);
}

SelfPlayGame& SelfPlayGame::operator=(const SelfPlayGame& other)
{
    assert(&other != this);

    Game::operator=(other);

    _root = other._root;
    _tryHard = other._tryHard;
    _image = other._image;
    _value = other._value;
    _policy = other._policy;
    _searchRootPly = other.Ply();
    _result = other._result;

    return *this;
}

SelfPlayGame::SelfPlayGame(SelfPlayGame&& other) noexcept
    : Game(other)
    , _root(other._root)
    , _tryHard(other._tryHard)
    , _image(other._image)
    , _value(other._value)
    , _policy(other._policy)
    , _searchRootPly(other._searchRootPly)
    , _childVisits(std::move(other._childVisits))
    , _history(std::move(other._history))
    , _result(other._result)
{
    assert(&other != this);
}

SelfPlayGame& SelfPlayGame::operator=(SelfPlayGame&& other) noexcept
{
    assert(&other != this);

    Game::operator=(static_cast<Game&&>(other));

    _root = other._root;
    _tryHard = other._tryHard;
    _image = other._image;
    _value = other._value;
    _policy = other._policy;
    _searchRootPly = other._searchRootPly;
    _childVisits = std::move(other._childVisits);
    _history = std::move(other._history);
    _result = other._result;

    return *this;
}

SelfPlayGame::~SelfPlayGame()
{
}

SelfPlayGame SelfPlayGame::SpawnShadow(INetwork::InputPlanes* image, float* value, INetwork::OutputPlanes* policy)
{
    SelfPlayGame shadow(*this);

    shadow._image = image;
    shadow._value = value;
    shadow._policy = policy;

    return shadow;
}

Node* SelfPlayGame::Root() const
{
    return _root;
}

float SelfPlayGame::Result() const
{
    // Require that the caller has called Complete() before calling Result().
    assert(_result != CHESSCOACH_VALUE_UNINITIALIZED);
    return _result;
}

bool SelfPlayGame::TryHard() const
{
    return _tryHard;
}

void SelfPlayGame::ApplyMoveWithRoot(Move move, Node* newRoot)
{
    ApplyMove(move);
    _root = newRoot;

    // Don't adjust visit counts here because this is a common path; e.g. for scratch games also.
}

void SelfPlayGame::ApplyMoveWithRootAndHistory(Move move, Node* newRoot)
{
    ApplyMoveWithRoot(move, newRoot);
    _history.push_back(move);

    // Adjust the visit count for the new root so that it matches the sum of child visits from now on.
    // If the new root is a terminal node, reset to zero.
    // Otherwise, decrement because the node was visited exactly once as a leaf before being expanded.
    if (_root->children.empty())
    {
        _root->visitCount = 0;
    }
    else
    {
        _root->visitCount--;
    }
    assert(_root->visitCount == std::transform_reduce(_root->children.begin(), _root->children.end(),
        0, std::plus<>(), [](auto pair) { return pair.second->visitCount; }));
}

float SelfPlayGame::ExpandAndEvaluate(SelfPlayState& state, PredictionCacheChunk*& cacheStore)
{
    Node* root = _root;
    assert(!root->IsExpanded());

    // A known-terminal leaf will remain a leaf, so be prepared to
    // quickly return its terminal value on repeated visits.
    if (root->terminalValue.IsImmediate())
    {
        state = SelfPlayState::Working;
        return root->terminalValue.ImmediateValue();
    }

    // It's very important in this method to always value a node from the parent's to-play perspective, so:
    // - flip network evaluations
    // - value checkmate as a win
    //
    // This seems a little counter-intuitive, but it's an artifact of storing priors/values/visits on
    // the child nodes themselves instead of on "edges" belonging to the parent.
    //
    // E.g. imagine it's white to play (game.ToPlay()) and white makes the move a4,
    // which results in a new position with black to play (scratchGame.ToPlay()).
    // The network values this position as very bad for black (say 0.1). This means
    // it's very good for white (0.9), so white should continue visiting this child node.
    //
    // Or, imagine it's white to play and they have a mate-in-one. From black's perspective,
    // in the resulting position, it's a loss (0.0) because they're in check and have no moves,
    // thus no child nodes. This is a win for white (1.0), so white should continue visiting this
    // child node.
    //
    // It's important to keep the following values in sign/direction parity, for a single child position
    // (all should tend to be high, or all should tend to be low):
    // - visits
    // - network policy prediction (prior)
    // - network value prediction (valueSum / visitCount, back-propagated)
    // - terminal valuation (valueSum / visitCount, back-propagated)

    if (state == SelfPlayState::Working)
    {
        // Try get a cached prediction. Only hit the cache up to a max ply for self-play since we
        // see enough unique positions/paths to fill the cache no matter what, and it saves on time
        // to evict less. However, in search (TryHard) it's better to keep everything recent.
        cacheStore = nullptr;
        float cachedValue;
        int cachedMoveCount;
        _imageKey = GenerateImageKey();
        bool hitCached = false;
        if (TryHard() || (Ply() <= Config::Misc.PredictionCache_MaxPly))
        {
            hitCached = PredictionCache::Instance.TryGetPrediction(_imageKey, &cacheStore, &cachedValue,
                &cachedMoveCount, _cachedMoves.data(), _cachedPriors.data());
        }
        if (hitCached)
        {
            // Expand child nodes with the cached priors.
            int i = 0;
            for (int i = 0; i < cachedMoveCount; i++)
            {
                root->children[Move(_cachedMoves[i])] = new Node(_cachedPriors[i]);
            }

            return cachedValue;
        }

        // Generate legal moves.
        _expandAndEvaluate_endMoves = generate<LEGAL>(_position, _expandAndEvaluate_moves);

        // Check for checkmate and stalemate.
        if (_expandAndEvaluate_moves == _expandAndEvaluate_endMoves)
        {
            // Value from the parent's perspective.
            root->terminalValue = (_position.checkers() ? TerminalValue::MateIn<1>() : TerminalValue::Draw());
            return root->terminalValue.ImmediateValue();
        }

        // Check for draw by 50-move or 3-repetition.
        //
        // Stockfish checks for (a) two-fold repetition strictly after the search root
        // (e.g. search-root, rep-0, rep-1) or (b) three-fold repetition anywhere
        // (e.g. rep-0, rep-1, search-root, rep-2) in order to terminate and prune efficiently.
        //
        // We can use the same logic safely because we're path-dependent: no post-search valuations
        // are hashed purely by position (only network-dependent predictions, potentially),
        // and nodes with identical positions reached differently are distinct in the tree.
        //
        // This saves time in the 800-simulation budget for more useful exploration.
        const int plyToSearchRoot = (Ply() - _searchRootPly);
        if (IsDrawByNoProgressOrRepetition(plyToSearchRoot))
        {
            // Value from the parent's perspective (easy, it's a draw).
            root->terminalValue = TerminalValue::Draw();
            return root->terminalValue.ImmediateValue();
        }

        // Prepare for a prediction from the network.
        *_image = GenerateImage();
        state = SelfPlayState::WaitingForPrediction;
        return std::numeric_limits<float>::quiet_NaN();
    }

    // Received a prediction from the network.

    // Value from the parent's perspective.
    float value = FlipValue(*_value);

    // Mix in the Stockfish evaluation when available.
    //
    // The important thing here is that this helps guide the MCTS search and thus the policy training,
    // but doesn't train the value head: that is still based purely on game result, so the network isn't
    // trying to learn a linear human evaluation function.
    if (!TryHard() && StockfishCanEvaluate())
    {
        // TODO: Lerp based on training progress.
        const float stockfishProbability01 = StockfishEvaluation();
        const float stockfishiness = 0.5f;
        value = (value * (1.f - stockfishiness)) + (stockfishProbability01 * stockfishiness);
    }

    // Index legal moves into the policy output planes to get logits,
    // then calculate softmax over them to get normalized probabilities for priors.
    int moveCount = 0;
    for (ExtMove* cur = _expandAndEvaluate_moves; cur != _expandAndEvaluate_endMoves; cur++)
    {
        _cachedMoves[moveCount] = cur->move;
        _cachedPriors[moveCount] = PolicyValue(*_policy, cur->move); // Logits
        moveCount++;
    }
    Softmax(moveCount, _cachedPriors.data()); // Logits -> priors

    // Store in the cache if appropriate. This may limit moveCount to the branch limit for caching.
    // In that case, better to also apply that limit now for consistency.
    if (cacheStore)
    {
        if (moveCount > Config::MaxBranchMoves)
        {
            LimitBranchingToBest(moveCount, _cachedMoves.data(), _cachedPriors.data());
            moveCount = Config::MaxBranchMoves;
        }
        cacheStore->Put(_imageKey, value, moveCount, _cachedMoves.data(), _cachedPriors.data());
    }

    // Expand child nodes with the calculated priors.
    for (int i = 0; i < moveCount; i++)
    {
        root->children[Move(_cachedMoves[i])] = new Node(_cachedPriors[i]);
    }

    state = SelfPlayState::Working;
    return value;
}

void SelfPlayGame::LimitBranchingToBest(int moveCount, uint16_t* moves, float* priors)
{
    assert(moveCount > Config::MaxBranchMoves);

    for (int i = 0; i < Config::MaxBranchMoves; i++)
    {
        int max = i;
        for (int j = i + 1; j < moveCount; j++)
        {
            if (priors[j] > priors[max]) max = j;
        }
        if (max != i)
        {
            std::swap(moves[i], moves[max]);
            std::swap(priors[i], priors[max]);
        }
    }
}

// Avoid Position::is_draw because it regenerates legal moves.
// If we've already just checked for checkmate and stalemate then this works fine.
bool SelfPlayGame::IsDrawByNoProgressOrRepetition(int plyToSearchRoot)
{
    const StateInfo& stateInfo = _positionStates->back();

    return 
        // Omit "and not checkmate" from Position::is_draw.
        (stateInfo.rule50 > 99) ||
        // Return a draw score if a position repeats once earlier but strictly
        // after the root, or repeats twice before or at the root.
        (stateInfo.repetition && (stateInfo.repetition < plyToSearchRoot));
}

void SelfPlayGame::Softmax(int moveCount, float* distribution) const
{
    const float max = *std::max_element(distribution, distribution + moveCount);

    float expSum = 0.f;
    for (int i = 0; i < moveCount; i++)
    {
        expSum += std::expf(distribution[i] - max);
    }

    const float logSumExp = std::logf(expSum) + max;
    for (int i = 0; i < moveCount; i++)
    {
        distribution[i] = std::expf(distribution[i] - logSumExp);
    }
}

void SelfPlayGame::StoreSearchStatistics()
{
    std::map<Move, float> visits;
    const int sumChildVisits = _root->visitCount;
    for (const auto& pair : _root->children)
    {
        visits[pair.first] = static_cast<float>(pair.second->visitCount) / sumChildVisits;
    }
    _childVisits.emplace_back(std::move(visits));
}

void SelfPlayGame::Complete()
{
    // Save state that depends on nodes.
    // Terminal value is from the parent's perspective, so unconditionally flip (~)
    // from *parent* to *self* before flipping from ToPlay() to white's perspective.
    _result = FlipValue(~ToPlay(), _root->terminalValue.ImmediateValue());

    // Clear and detach from all nodes.
    PruneAll();
}

SavedGame SelfPlayGame::Save() const
{
    return SavedGame(Result(), _history, _childVisits);
}

void SelfPlayGame::PruneExcept(Node* root, Node* except)
{
    if (!root)
    {
        return;
    }

    // Rely on caller to already have updated the _root to the preserved subtree.
    assert(_root != root);
    assert(_root == except);

    for (auto& pair : root->children)
    {
        if (pair.second != except)
        {
            PruneAllInternal(pair.second);
        }
    }
    delete root;
}

void SelfPlayGame::PruneAll()
{
    if (!_root)
    {
        return;
    }

    PruneAllInternal(_root);

    // All nodes in the related tree are gone, so don't leave _root dangling.
    _root = nullptr;
}

void SelfPlayGame::PruneAllInternal(Node* root)
{
    for (auto& pair : root->children)
    {
        PruneAllInternal(pair.second);
    }
    delete root;
}

Move SelfPlayGame::ParseSan(const std::string& san)
{
    return Pgn::ParseSan(_position, san);
}

SelfPlayWorker::SelfPlayWorker(const NetworkConfig& networkConfig, Storage* storage)
    : _networkConfig(&networkConfig)
    , _storage(storage)
    , _states(networkConfig.SelfPlay.PredictionBatchSize)
    , _images(networkConfig.SelfPlay.PredictionBatchSize)
    , _values(networkConfig.SelfPlay.PredictionBatchSize)
    , _policies(networkConfig.SelfPlay.PredictionBatchSize)
    , _games(networkConfig.SelfPlay.PredictionBatchSize)
    , _scratchGames(networkConfig.SelfPlay.PredictionBatchSize)
    , _gameStarts(networkConfig.SelfPlay.PredictionBatchSize)
    , _mctsSimulations(networkConfig.SelfPlay.PredictionBatchSize, 0)
    , _searchPaths(networkConfig.SelfPlay.PredictionBatchSize)
    , _cacheStores(networkConfig.SelfPlay.PredictionBatchSize)
    , _searchConfig{}
    , _searchState{}
{
}

const NetworkConfig& SelfPlayWorker::Config() const
{
    return *_networkConfig;
}

void SelfPlayWorker::ResetGames()
{
    for (int i = 0; i < _networkConfig->SelfPlay.PredictionBatchSize; i++)
    {
        SetUpGame(i);
    }
}

void SelfPlayWorker::PlayGames(WorkCoordinator& workCoordinator, INetwork* network)
{
    while (true)
    {
        // Wait until games are required.
        workCoordinator.WaitForWorkItems();

        // Clear away old games in progress to ensure that new ones use the new network.
        ResetGames();

        // Play games until required.
        while (!workCoordinator.AllWorkItemsCompleted())
        {
            // CPU work
            for (int i = 0; i < _games.size(); i++)
            {
                Play(i);

                // In degenerate conditions whole games can finish in CPU via the prediction cache, so loop.
                while ((_states[i] == SelfPlayState::Finished) && !workCoordinator.AllWorkItemsCompleted())
                {
                    SaveToStorageAndLog(i);

                    workCoordinator.OnWorkItemCompleted();

                    SetUpGame(i);
                    Play(i);
                }
            }

            // GPU work
            network->PredictBatch(_networkConfig->SelfPlay.PredictionBatchSize, _images.data(), _values.data(), _policies.data());
        }
    }
}

void SelfPlayWorker::ClearGame(int index)
{
    _states[index] = SelfPlayState::Working;
    _gameStarts[index] = std::chrono::high_resolution_clock::now();
    _mctsSimulations[index] = 0;
    _searchPaths[index].clear();
    _cacheStores[index] = nullptr;
}

void SelfPlayWorker::SetUpGame(int index)
{
    ClearGame(index);
    _games[index] = SelfPlayGame(&_images[index], &_values[index], &_policies[index]);

}

void SelfPlayWorker::SetUpGame(int index, const std::string& fen, const std::vector<Move>& moves, bool tryHard)
{
    ClearGame(index);
    _games[index] = SelfPlayGame(fen, moves, tryHard, &_images[index], &_values[index], &_policies[index]);
}

void SelfPlayWorker::SetUpGameExisting(int index, const std::vector<Move>& moves, int applyNewMovesOffset, bool tryHard)
{
    ClearGame(index);

    SelfPlayGame& game = _games[index];

    for (int i = applyNewMovesOffset; i < moves.size(); i++)
    {
        const Move move = moves[i];
        Node* root = game.Root();

        std::map<Move, Node*>::iterator child;
        if (root && ((child = root->children.find(move)) != root->children.end()))
        {
            // Preserve the existing sub-tree.
            game.ApplyMoveWithRoot(move, child->second);
            game.PruneExcept(root, child->second);
        }
        else
        {
            Node* newRoot = ((i == (moves.size() - 1)) ? new Node(0.f) : nullptr);
            game.PruneAll();
            game.ApplyMoveWithRoot(move, newRoot);
        }
    }
}

void SelfPlayWorker::DebugGame(INetwork* network, int index, const SavedGame& saved, int startingPly)
{
    SetUpGame(index);

    SelfPlayGame& game = _games[index];
    for (int i = 0; i < startingPly; i++)
    {
        game.ApplyMove(Move(saved.moves[i]));
    }

    while (true)
    {
        Play(index);
        assert(_states[index] == SelfPlayState::WaitingForPrediction);
        network->PredictBatch(index + 1, _images.data(), _values.data(), _policies.data());
    }
}

void SelfPlayWorker::TrainNetwork(INetwork* network, int stepCount, int checkpoint)
{
    // Train for "stepCount" steps.
    auto startTrain = std::chrono::high_resolution_clock::now();
    const int startStep = (checkpoint - stepCount + 1);
    for (int step = startStep; step <= checkpoint; step++)
    {
        TrainingBatch* batch = _storage->SampleBatch(GameType_Training, Config());
        network->TrainBatch(step, _networkConfig->Training.BatchSize, batch->images.data(), batch->values.data(), batch->policies.data());

        // Validate the network every "ValidationInterval" steps.
        if ((step % _networkConfig->Training.ValidationInterval) == 0)
        {
            ValidateNetwork(network, step);
        }
    }
    const float trainTime = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - startTrain).count();
    const float trainTimePerStep = (trainTime / stepCount);
    std::cout << "Trained steps " << startStep << "-" << checkpoint << ", total time " << trainTime << ", step time " << trainTimePerStep << std::endl;

    // Save the network and reload it for predictions.
    network->SaveNetwork(checkpoint);

    // Strength-test the engine every "StrengthTestInterval" steps.
    assert(_networkConfig->Training.StrengthTestInterval > _networkConfig->Training.CheckpointInterval);
    assert((_networkConfig->Training.StrengthTestInterval % _networkConfig->Training.CheckpointInterval) == 0);
    if ((checkpoint % _networkConfig->Training.StrengthTestInterval) == 0)
    {
        StrengthTest(network, checkpoint);
    }
}

void SelfPlayWorker::ValidateNetwork(INetwork* network, int step)
{
    // Measure validation loss/accuracy using one batch.
    if (_storage->GamesPlayed(GameType_Validation) > 0)
    {
        TrainingBatch* validationBatch = _storage->SampleBatch(GameType_Validation, Config());
        network->ValidateBatch(step, _networkConfig->Training.BatchSize, validationBatch->images.data(), validationBatch->values.data(), validationBatch->policies.data());
    }
}

void SelfPlayWorker::StrengthTest(INetwork* network, int step)
{
    std::map<std::string, int> testResults;
    std::map<std::string, int> testPositions;

    std::cout << "Running strength tests..." << std::endl;

    // STS gets special treatment.
    const std::string stsName = "STS";

    // Find strength test .epd files.
    const std::filesystem::path testPath = (std::filesystem::current_path() / "StrengthTests");
    for (const auto& entry : std::filesystem::directory_iterator(testPath))
    {
        if (entry.path().extension().string() == ".epd")
        {
            // Hard-coding move times in an ugly way here. They should really be 10-15 seconds for ERET and Arasan20,
            // not 1 second, but this can show some level of progress during training without taking forever.
            // However, only STS results will be comparable to other tested engines.
            const std::string testName = entry.path().stem().string();
            const int moveTimeMs = ((testName == stsName) ? 200 : 1000);

            const auto [score, total, positions] = StrengthTest(network, entry.path(), moveTimeMs);
            testResults[testName] = score;
            testPositions[testName] = positions;
        }
    }

    // Estimate an Elo rating using logic here: https://github.com/fsmosca/STS-Rating/blob/master/sts_rating.py
    const float slope = 445.23f;
    const float intercept = -242.85f;
    const float stsRating = (slope * testResults[stsName] / testPositions[stsName]) + intercept;

    // Log to TensorBoard.
    std::vector<std::string> names;
    std::vector<float> values;
    for (const auto& [testName, score] : testResults)
    {
        names.emplace_back("strength/" + testName + "_score");
        values.push_back(static_cast<float>(score));
        std::cout << names.back() << ": " << values.back() << std::endl;
    }
    names.emplace_back("strength/" + stsName + "_rating");
    values.push_back(stsRating);
    std::cout << names.back() << ": " << values.back() << std::endl;
    network->LogScalars(step, static_cast<int>(names.size()), names.data(), values.data());
}

// Returns (score, total, positions).
std::tuple<int, int, int> SelfPlayWorker::StrengthTest(INetwork* network, const std::filesystem::path& epdPath, int moveTimeMs)
{
    int score = 0;
    int total = 0;
    int positions = 0;

    // Clear the prediction cache for consistent results.
    PredictionCache::Instance.Clear();

    // Warm up the GIL and predictions.
    WarmUpPredictions(network, 1);

    const std::vector<StrengthTestSpec> specs = Epd::ParseEpds(epdPath);
    positions = static_cast<int>(specs.size());

    for (const StrengthTestSpec& spec : specs)
    {
        const int points = StrengthTestPosition(network, spec, moveTimeMs);
        score += points;
        total += (spec.points.empty() ? 1 : *std::max_element(spec.points.begin(), spec.points.end()));
    }

    return std::tuple(score, total, positions);
}

// For best-move tests returns 1 if correct or 0 if incorrect.
// For points/alternative tests returns N points or 0 if incorrect.
int SelfPlayWorker::StrengthTestPosition(INetwork* network, const StrengthTestSpec& spec, int moveTimeMs)
{
    // Set up the position.
    _games[0].PruneAll();
    SetUpGame(0, spec.fen, {}, true /* tryHard */);

    // Set up search and time control.
    TimeControl timeControl = {};
    timeControl.moveTimeMs = moveTimeMs;

    _searchState.searching = true;
    _searchState.searchStart = std::chrono::high_resolution_clock::now();
    _searchState.lastPrincipleVariationPrint = _searchState.searchStart;
    _searchState.timeControl = timeControl;
    _searchState.nodeCount = 0;
    _searchState.failedNodeCount = 0;
    _searchState.principleVariationChanged = false;

    // Initialize the search.
    const int mctsParallelism = std::min(static_cast<int>(_games.size()), Config::Misc.Search_MctsParallelism);
    SearchInitialize(mctsParallelism);

    // Run the search.
    while (_searchState.searching)
    {
        SearchPlay(mctsParallelism);
        network->PredictBatch(mctsParallelism, _images.data(), _values.data(), _policies.data());

        // TODO: Only check every N times
        CheckTimeControl();
    }

    // Pick a best move and judge points.
    const auto [bestMove, _] = SelectMove(_games[0]);
    return JudgeStrengthTestPosition(spec, bestMove);
}

int SelfPlayWorker::JudgeStrengthTestPosition(const StrengthTestSpec& spec, Move move)
{
    assert(spec.pointSans.empty() ^ spec.avoidSans.empty());
    assert(spec.pointSans.size() == spec.points.size());

    for (const std::string avoidSan : spec.avoidSans)
    {
        const Move avoid = _games[0].ParseSan(avoidSan);
        assert(avoid != MOVE_NONE);
        if (avoid == move)
        {
            return 0;
        }
    }

    for (int i = 0; i < spec.pointSans.size(); i++)
    {
        const Move bestOrAlternative = _games[0].ParseSan(spec.pointSans[i]);
        assert(bestOrAlternative != MOVE_NONE);
        if (bestOrAlternative == move)
        {
            return spec.points[i];
        }
    }

    if (spec.pointSans.empty() && !spec.avoidSans.empty())
    {
        return 1;
    }
    return 0;
}

void SelfPlayWorker::Play(int index)
{
    SelfPlayState& state = _states[index];
    SelfPlayGame& game = _games[index];

    if (!game.Root()->IsExpanded())
    {
        game.ExpandAndEvaluate(state, _cacheStores[index]);
        if (state == SelfPlayState::WaitingForPrediction)
        {
            return;
        }
    }

    while (!IsTerminal(game))
    {
        Node* root = game.Root();
        std::pair<Move, Node*> selected = RunMcts(game, _scratchGames[index], _states[index], _mctsSimulations[index], _searchPaths[index], _cacheStores[index]);
        if (state == SelfPlayState::WaitingForPrediction)
        {
            return;
        }

        assert(selected.second != nullptr);
        game.StoreSearchStatistics();
        game.ApplyMoveWithRootAndHistory(selected.first, selected.second);
        game.PruneExcept(root, selected.second /* == game.Root() */);
        _searchState.principleVariationChanged = true; // First move in PV is now gone.
    }

    // Clean up resources in use and save the result.
    game.Complete();

    state = SelfPlayState::Finished;
}

bool SelfPlayWorker::IsTerminal(const SelfPlayGame& game) const
{
    return (game.Root()->terminalValue.IsImmediate() || (game.Ply() >= Config().SelfPlay.MaxMoves));
}

void SelfPlayWorker::SaveToStorageAndLog(int index)
{
    const SelfPlayGame& game = _games[index];

    const int ply = game.Ply();
    const float result = game.Result();
    const int gameNumber = _storage->AddGame(GameType_Training, game.Save(), Config());

    const float gameTime = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - _gameStarts[index]).count();
    const float mctsTime = (gameTime / ply);
    std::cout << "Game " << gameNumber << ", ply " << ply << ", time " << gameTime << ", mcts time " << mctsTime << ", result " << result << std::endl;
    //PredictionCache::Instance.PrintDebugInfo();
}

std::pair<Move, Node*> SelfPlayWorker::RunMcts(SelfPlayGame& game, SelfPlayGame& scratchGame, SelfPlayState& state, int& mctsSimulation,
    std::vector<std::pair<Move, Node*>>& searchPath, PredictionCacheChunk*& cacheStore)
{
    // Don't get stuck in here forever during search (TryHard) looping on cache hits or terminal nodes.
    // We need to break out and check for PV changes, search stopping, etc. However, need to keep number
    // high enough to get good speed-up from prediction cache hits. Go with 1000 for now.
    const int numSimulations = (game.TryHard() ? (mctsSimulation + 1000) : Config().SelfPlay.NumSimulations);
    for (; mctsSimulation < numSimulations; mctsSimulation++)
    {
        if (state == SelfPlayState::Working)
        {
            if (mctsSimulation == 0)
            {
#if !DEBUG_MCTS
                if (!game.TryHard())
                {
                    AddExplorationNoise(game);
                }
#endif

#if DEBUG_MCTS
                std::cout << "(Ready for ply " << game.Ply() << "...)" << std::endl;
                std::string _;
                std::getline(std::cin, _);
#endif
            }

            // MCTS tree parallelism - enabled when searching, not when training - needs some guidance
            // to avoid repeating the same deterministic child selections:
            // - Avoid branches + leaves by incrementing "visitingCount" while selecting a search path,
            //   lowering the exploration incentive in the UCB score.
            // - However, let searches override this when it's important enough; e.g. going down the
            //   same deep line to explore sibling leaves, or revisiting a checkmate.

            scratchGame = game;
            searchPath.clear();
            searchPath.push_back(std::pair(MOVE_NONE, scratchGame.Root()));
            scratchGame.Root()->visitingCount++;

            while (scratchGame.Root()->IsExpanded())
            {
                // If we can't select a child it's because parallel MCTS is already expanding all
                // children. Give up on this one until next iteration, just fix up visitingCounts.
                std::pair<Move, Node*> selected = SelectChild(scratchGame.Root());
                if (!selected.second)
                {
                    assert(game.TryHard());
                    for (auto& [move, node] : searchPath)
                    {
                        node->visitingCount--;
                    }
                    _searchState.failedNodeCount++;
                    return std::pair(MOVE_NONE, nullptr);
                }

                scratchGame.ApplyMoveWithRoot(selected.first, selected.second);
                searchPath.push_back(selected /* == scratchGame.Root() */);
                selected.second->visitingCount++;
#if DEBUG_MCTS
                std::cout << Game::SquareName[from_sq(selected.first)] << Game::SquareName[to_sq(selected.first)] << "(" << selected.second->visitCount << "), ";
#endif
            }
        }

        const bool wasImmediateMate = (scratchGame.Root()->terminalValue == TerminalValue::MateIn<1>());
        float value = scratchGame.ExpandAndEvaluate(state, cacheStore);
        if (state == SelfPlayState::WaitingForPrediction)
        {
            // This is now a dangerous time when searching because this leaf is going to be expanded
            // once the network evaluation/priors come back, but is not yet seen as expanded by
            // parallel searches. Set "expanding" to mark it off-limits.
            scratchGame.Root()->expanding = true;
            return std::pair(MOVE_NONE, nullptr);
        }
        else
        {
            // Finished actually expanding children, or never needed to wait for an evaluation/priors
            // (e.g. prediction cache hit) or no children possible (terminal node).
            scratchGame.Root()->expanding = false;
        }

        // The value we get is from the final node of the scratch game (could be WHITE or BLACK),
        // from its parent's perspective, and we start applying it at the current position of
        // the actual game (could again be WHITE or BLACK), again from its parent's perspective,
        // so flip it if they differ (the ^). This seems a little strange for the root node, because
        // it doesn't really have a parent in the game, but that is why its value doesn't really matter.
        assert(!std::isnan(value));
        value = SelfPlayGame::FlipValue(Color(game.ToPlay() ^ scratchGame.ToPlay()), value);
        Backpropagate(searchPath, value);
        _searchState.nodeCount++;

        // If we *just found out* that this leaf is a checkmate, prove it backwards as far as possible.
        if (!wasImmediateMate && scratchGame.Root()->terminalValue.IsMateInN())
        {
            BackpropagateMate(searchPath);
        }

        // Adjust best-child pointers (principle variation) now that visits and mates have propagated.
        UpdatePrincipleVariation(searchPath);
        ValidatePrincipleVariation(scratchGame.Root());

#if DEBUG_MCTS
        std::cout << "prior " << scratchGame.Root()->prior << ", prediction " << value << std::endl;
#endif
    }

    mctsSimulation = 0;
    return SelectMove(game);
}

void SelfPlayWorker::AddExplorationNoise(SelfPlayGame& game) const
{
    std::gamma_distribution<float> gamma(Config().SelfPlay.RootDirichletAlpha, 1.f);
    std::vector<float> noise(game.Root()->children.size());

    float noiseSum = 0.f;
    for (int i = 0; i < noise.size(); i++)
    {
        noise[i] = gamma(Random);
        noiseSum += noise[i];
    }

    int childIndex = 0;
    for (auto& [move, child] : game.Root()->children)
    {
        const float normalized = (noise[childIndex++] / noiseSum);
        assert(!std::isnan(normalized));
        assert(!std::isinf(normalized));
        child->prior = (child->prior * (1 - Config().SelfPlay.RootExplorationFraction) + normalized * Config().SelfPlay.RootExplorationFraction);
    }
}

std::pair<Move, Node*> SelfPlayWorker::SelectMove(const SelfPlayGame& game) const
{
    if (!game.TryHard() && (game.Ply() < Config().SelfPlay.NumSampingMoves))
    {
        // Use temperature=1; i.e., no need to exponentiate, just use visit counts as the distribution.
        const int sumChildVisits = game.Root()->visitCount;
        int sample = std::uniform_int_distribution<int>(0, sumChildVisits - 1)(Random);
        for (const auto& pair : game.Root()->children)
        {
            const int visitCount = pair.second->visitCount;
            if (sample < visitCount)
            {
                return pair;
            }
            sample -= visitCount;
        }
        assert(false);
        return std::pair(MOVE_NONE, nullptr);
    }
    else
    {
        // Use temperature=inf; i.e., just select the best (most-visited, overridden by mates).
        assert(game.Root()->bestChild.second);
        return game.Root()->bestChild;
    }
}

// It's possible because of nodes marked off-limits via "expanding"
// that this method cannot select a child, instead returning NONE/nullptr.
std::pair<Move, Node*> SelfPlayWorker::SelectChild(const Node* parent) const
{
    float maxUcbScore = -std::numeric_limits<float>::infinity();
    std::pair<Move, Node*> max = std::pair(MOVE_NONE, nullptr);
    for (const auto& pair : parent->children)
    {
        if (!pair.second->expanding)
        {
            const float ucbScore = CalculateUcbScore(parent, pair.second);
            if (ucbScore > maxUcbScore)
            {
                maxUcbScore = ucbScore;
                max = pair;
            }
        }
    }
    return max;
}

// TODO: Profile, see if significant, whether vectorizing is viable/worth it
float SelfPlayWorker::CalculateUcbScore(const Node* parent, const Node* child) const
{
    // Calculate the exploration rate, which is multiplied by (a) the prior to incentivize exploration,
    // and (b) a mate-in-N lookup to incentivize sufficient exploitation of forced mates, dependent on depth.
    // Include "visitingCount" to help parallel searches diverge.
    const float parentVirtualExploration = static_cast<float>(parent->visitCount + parent->visitingCount);
    const float childVirtualExploration = static_cast<float>(child->visitCount + child->visitingCount);
    const float explorationRate =
        (std::logf((parentVirtualExploration + Config().SelfPlay.ExplorationRateBase + 1.f) / Config().SelfPlay.ExplorationRateBase) + Config().SelfPlay.ExplorationRateInit) *
        std::sqrtf(parentVirtualExploration) / (childVirtualExploration + 1.f);

    // (a) prior score
    const float priorScore = explorationRate * child->prior;

    // (b) mate-in-N score
    const float mateScore = child->terminalValue.MateScore(explorationRate);

    return (child->Value() + priorScore + mateScore);
}

void SelfPlayWorker::Backpropagate(const std::vector<std::pair<Move, Node*>>& searchPath, float value)
{
    // Each ply has a different player, so flip each time.
    for (auto& [move, node] : searchPath)
    {
        node->visitingCount--;
        node->visitCount++;
        node->valueSum += value;
        value = SelfPlayGame::FlipValue(value);
    }
}

void SelfPlayWorker::BackpropagateMate(const std::vector<std::pair<Move, Node*>>& searchPath)
{
    // To calculate mate values for the tree from scratch we'd need to follow two rules:
    // - If *any* children are a MateIn<N...M> then the parent is an OpponentMateIn<N> (prefer to mate faster).
    // - If *all* children are an OpponentMateIn<N...M> then the parent is a MateIn<M+1> (prefer to get mated slower).
    //
    // However, knowing that values were already correct before, we can just do odd/even checks and stop when nothing changes.
    bool childIsMate = true;
    for (int i = static_cast<int>(searchPath.size()) - 2; i >= 0; i--)
    {
        Node* parent = searchPath[i].second;

        if (childIsMate)
        {
            // The child in the searchPath just became a mate, or a faster mate.
            // Does this make the parent an opponent mate or faster opponent mate?
            const Node* child = searchPath[i + 1].second;
            const int newMateN = child->terminalValue.MateN();
            assert(newMateN > 0);
            if (!parent->terminalValue.IsOpponentMateInN() ||
                (newMateN < parent->terminalValue.OpponentMateN()))
            {
                parent->terminalValue = TerminalValue::OpponentMateIn(newMateN);

                // The parent just became worse, so the grandparent may need a different best-child.
                // The regular principle variation update isn't sufficient because it assumes that
                // the search path can only become better than it was.
                const int grandparentIndex = (i - 1);
                if (grandparentIndex >= 0)
                {
                    // It's tempting to try validate the principle variation after this fix, but we
                    // may still be waiting to update it after backpropagating visit counts and mates.
                    // This is only a local fix that ensures that the overall update will be valid.
                    FixPrincipleVariation(searchPath, searchPath[grandparentIndex].second);
                }
            }
            else
            {
                return;
            }
        }
        else
        {
            // The child in the searchPath just became an opponent mate or faster opponent mate.
            // Always check all children. This could do nothing, make the parent a new mate, or
            // make the parent a faster mate, depending on which child just got updated.
            int longestChildOpponentMateN = std::numeric_limits<int>::min();
            for (const auto& [move, child] : parent->children)
            {
                const int childOpponentMateN = child->terminalValue.OpponentMateN();
                if (childOpponentMateN <= 0)
                {
                    return;
                }

                longestChildOpponentMateN = std::max(longestChildOpponentMateN, childOpponentMateN);
            }

            assert(longestChildOpponentMateN > 0);
            parent->terminalValue = TerminalValue::MateIn(longestChildOpponentMateN + 1);
        }

        childIsMate = !childIsMate;
    }
}

void SelfPlayWorker::FixPrincipleVariation(const std::vector<std::pair<Move, Node*>>& searchPath, Node* parent)
{
    bool updatedBestChild = false;
    for (const auto& pair : parent->children)
    {
        if (WorseThan(parent->bestChild.second, pair.second))
        {
            parent->bestChild = pair;
            updatedBestChild = true;
        }
    }

    // We updated a best-child, but that only changed the principle variation if this parent was part of it.
    if (updatedBestChild)
    {
        for (int i = 0; i < searchPath.size() - 1; i++)
        {
            if (searchPath[i].second == parent)
            {
                _searchState.principleVariationChanged = true;
                break;
            }
            if (searchPath[i].second->bestChild.second != searchPath[i + 1].second)
            {
                break;
            }
        }
    }
}

void SelfPlayWorker::UpdatePrincipleVariation(const std::vector<std::pair<Move, Node*>>& searchPath)
{
    bool isPrincipleVariation = true;
    for (int i = 0; i < searchPath.size() - 1; i++)
    {
        if (WorseThan(searchPath[i].second->bestChild.second, searchPath[i + 1].second))
        {
            searchPath[i].second->bestChild = searchPath[i + 1];
            _searchState.principleVariationChanged |= isPrincipleVariation;
        }
        else
        {
            isPrincipleVariation &= (searchPath[i].second->bestChild.second == searchPath[i + 1].second);
        }
    }
}

void SelfPlayWorker::ValidatePrincipleVariation(const Node* root)
{
    while (root)
    {
        for (const auto& pair : root->children)
        {
            if (pair.second->visitCount > 0)
            {
                assert(!WorseThan(root->bestChild.second, pair.second));
            }
        }
        root = root->bestChild.second;
    }
}

bool SelfPlayWorker::WorseThan(const Node* lhs, const Node* rhs) const
{
    // Expect RHS to be defined, so if no LHS then it's better.
    assert(rhs);
    if (!lhs)
    {
        return true;
    }

    // Prefer faster mates and slower opponent mates.
    int lhsEitherMateN = lhs->terminalValue.EitherMateN();
    int rhsEitherMateN = rhs->terminalValue.EitherMateN();
    if (lhsEitherMateN != rhsEitherMateN)
    {
        // For categories (>0, 0, <0), bigger is better.
        // Within categories (1 vs. 3, -2 vs. -4), smaller is better.
        // Add a large term opposing the category sign, then say smaller is better overall.
        lhsEitherMateN += ((lhsEitherMateN < 0) - (lhsEitherMateN > 0)) * 2 * Config().SelfPlay.MaxMoves;
        rhsEitherMateN += ((rhsEitherMateN < 0) - (rhsEitherMateN > 0)) * 2 * Config().SelfPlay.MaxMoves;
        return (lhsEitherMateN > rhsEitherMateN);
    }

    // Prefer more visits.
    return (lhs->visitCount < rhs->visitCount);
}

void SelfPlayWorker::DebugGame(int index, SelfPlayGame** gameOut, SelfPlayState** stateOut, float** valuesOut, INetwork::OutputPlanes** policiesOut)
{
    if (gameOut) *gameOut = &_games[index];
    if (stateOut) *stateOut = &_states[index];
    if (valuesOut) *valuesOut = &_values[index];
    if (policiesOut) *policiesOut = &_policies[index];
}

SearchState& SelfPlayWorker::DebugSearchState()
{
    return _searchState;
}

void SelfPlayWorker::Search(std::function<INetwork*()> networkFactory)
{
    // Create the network on the worker thread (slow).
    std::unique_ptr<INetwork> network(networkFactory());

    // Warm up the GIL and predictions.
    WarmUpPredictions(network.get(), 1);

    // Start with the position "updated" to the starting position in case of a naked "go" command.
    {
        std::unique_lock lock(_searchConfig.mutexUci);

        if (!_searchConfig.positionUpdated)
        {
            _searchConfig.positionUpdated = true;
            _searchConfig.positionFen = Config::StartingPosition;
            _searchConfig.positionMoves = {};
        }
    }

    // Determine config.
    const int mctsParallelism = std::min(static_cast<int>(_games.size()), Config::Misc.Search_MctsParallelism);

    while (!_searchConfig.quit)
    {
        {
            std::unique_lock lock(_searchConfig.mutexUci);

            // Let UCI know we're ready.
            if (!_searchConfig.ready)
            {
                _searchConfig.ready = true;
                _searchConfig.signalReady.notify_all();
            }

            // Wait until told to search.
            while (!_searchConfig.quit && !_searchConfig.search)
            {
                _searchConfig.signalUci.wait(lock);
            }
        }

        UpdatePosition();
        UpdateSearch();
        if (_searchState.searching)
        {
            // Initialize the search.
            SearchInitialize(mctsParallelism);

            // Run the search.
            while (!_searchConfig.quit && !_searchConfig.positionUpdated && _searchState.searching)
            {
                SearchPlay(mctsParallelism);
                network->PredictBatch(mctsParallelism, _images.data(), _values.data(), _policies.data());

                CheckPrintInfo();

                // TODO: Only check every N times
                CheckTimeControl();

                UpdateSearch();
            }
            OnSearchFinished();
        }
    }

    // Clean up.
    _games[0].PruneAll();
}

void SelfPlayWorker::WarmUpPredictions(INetwork* network, int batchSize)
{
    network->PredictBatch(batchSize, _images.data(), _values.data(), _policies.data());
}

void SelfPlayWorker::UpdatePosition()
{
    assert(!_searchState.searching);

    if (_searchConfig.positionUpdated)
    {
        std::lock_guard lock(_searchConfig.mutexUci);

        // Lock around both (a) using the position info, and (b) clearing the flag.
        // If the GUI does two updates very quickly, either (i) we grabbed the second one's
        // position info and cleared, or (ii) the flag gets set again after we unlock. Either way
        // we're good.

        // If the new position is the previous position plus some number of moves,
        // just play out the moves rather than throwing away search results.
        if ((_searchState.positionFen == _searchConfig.positionFen) &&
            (_searchConfig.positionMoves.size() >= _searchState.positionMoves.size()) &&
            (std::equal(_searchState.positionMoves.begin(), _searchState.positionMoves.end(), _searchConfig.positionMoves.begin())))
        {
            if (_searchConfig.debug)
            {
                std::cout << "info string [position] Reusing existing position with "
                    << (_searchConfig.positionMoves.size() - _searchState.positionMoves.size()) << " additional moves" << std::endl;
            }
            SetUpGameExisting(0, _searchConfig.positionMoves, static_cast<int>(_searchState.positionMoves.size()), true /* tryHard */);
        }
        else
        {
            if (_searchConfig.debug)
            {
                std::cout << "info string [position] Creating new position" << std::endl;
            }
            _games[0].PruneAll();
            SetUpGame(0, _searchConfig.positionFen, _searchConfig.positionMoves, true /* tryHard */);
        }

        _searchState.positionFen = std::move(_searchConfig.positionFen);
        _searchConfig.positionFen = "";

        _searchState.positionMoves = std::move(_searchConfig.positionMoves);
        _searchConfig.positionMoves = {};

        _searchConfig.positionUpdated = false;
    }
}

void SelfPlayWorker::UpdateSearch()
{
    if (_searchConfig.searchUpdated)
    {
        std::lock_guard lock(_searchConfig.mutexUci);

        // Lock around both (a) using the search/time control info, and (b) clearing the flag.
        // If the GUI does two updates very quickly, either (i) we grabbed the second one's
        // search/time control info and cleared, or (ii) the flag gets set again after we unlock.
        // Either way we're good.

        _searchState.searching = _searchConfig.search;

        if (_searchState.searching)
        {
            _searchState.searchStart = std::chrono::high_resolution_clock::now();
            _searchState.lastPrincipleVariationPrint = _searchState.searchStart;
            _searchState.timeControl = _searchConfig.searchTimeControl;
            _searchState.nodeCount = 0;
            _searchState.failedNodeCount = 0;
            _searchState.principleVariationChanged = true; // Print out initial PV.
        }

        // Set the "search" instruction to false now so that when this search finishes
        // the worker can go back to sleep, unless instructed to search again.
        // A stop command will still cause the "searchUpdated" flag to call in here and
        // set the "searching" state to false.
        _searchConfig.search = false;

        _searchConfig.searchUpdated = false;
    }
}

void SelfPlayWorker::OnSearchFinished()
{
    // We may have finished via position update or quit, so update our state.
    _searchState.searching = false;

    // Print the final PV info and bestmove.
    auto [move, node] = SelectMove(_games[0]);
    PrintPrincipleVariation();
    std::cout << "bestmove " << UCI::move(move, false /* chess960 */) << std::endl;

    // Lock around (a) checking "searchUpdated" and (b) clearing "search". We want to clear
    // "search" in order to go back to sleep but only if it's still the existing search.
    {
        std::lock_guard lock(_searchConfig.mutexUci);

        if (!_searchConfig.searchUpdated)
        {
            _searchConfig.search = false;
        }
    }
}

void SelfPlayWorker::CheckPrintInfo()
{
    // Print principle variation when it changes, or at least every 5 seconds.
    if (_searchState.principleVariationChanged ||
        (std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - _searchState.lastPrincipleVariationPrint).count() >= 5.f))
    {
        PrintPrincipleVariation();
        _searchState.principleVariationChanged = false;
    }
}

void SelfPlayWorker::CheckTimeControl()
{
    // Always do at least 1-2 simulations so that a "best" move exists.
    if (!_games[0].Root()->bestChild.second)
    {
        return;
    }

    // Infinite think takes first priority.
    if (_searchState.timeControl.infinite)
    {
        return;
    }

    const std::chrono::duration sinceSearchStart = (std::chrono::high_resolution_clock::now() - _searchState.searchStart);
    const int64_t searchTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(sinceSearchStart).count();

    // Specified think time takes second priority.
    if (_searchState.timeControl.moveTimeMs > 0)
    {
        if (searchTimeMs >= _searchState.timeControl.moveTimeMs)
        {
            _searchState.searching = false;
        }
        return;
    }

    // Game clock takes third priority. Use a simple strategy like AlphaZero for now.
    const Color toPlay = _games[0].ToPlay();
    const int64_t timeAllowed =
        (_searchState.timeControl.timeRemainingMs[toPlay] / Config::Misc.TimeControl_FractionOfRemaining)
        + _searchState.timeControl.incrementMs[toPlay]
        - Config::Misc.TimeControl_SafetyBufferMs;
    if (timeAllowed > 0)
    {
        if (searchTimeMs >= timeAllowed)
        {
            _searchState.searching = false;
        }
        return;
    }

    // No time allowed at all: defy the system and just make a quick training-style move.
    if (_mctsSimulations[0] >= Config().SelfPlay.NumSimulations)
    {
        _searchState.searching = false;
    }
}

void SelfPlayWorker::PrintPrincipleVariation()
{
    Node* node = _games[0].Root();
    std::vector<Move> principleVariation;

    if (!node->bestChild.second)
    {
        return;
    }

    while (node->bestChild.second)
    {
        principleVariation.push_back(node->bestChild.first);
        node = node->bestChild.second;
    }

    auto now = std::chrono::high_resolution_clock::now();
    const std::chrono::duration sinceSearchStart = (now - _searchState.searchStart);
    _searchState.lastPrincipleVariationPrint = now;

    // Value is from the parent's perspective, so that's already correct for the root perspective
    const Node* pvFirst = _games[0].Root()->bestChild.second;
    const int eitherMateN = pvFirst->terminalValue.EitherMateN();
    const float value = pvFirst->Value();
    const int depth = static_cast<int>(principleVariation.size());
    const int64_t searchTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(sinceSearchStart).count();
    const int nodeCount = _searchState.nodeCount;
    const int nodesPerSecond = static_cast<int>(nodeCount / std::chrono::duration<float>(sinceSearchStart).count());
    const int hashfullPermille = PredictionCache::Instance.PermilleFull();

    std::cout << "info depth " << depth;

    if (eitherMateN != 0)
    {
        std::cout << " score mate " << eitherMateN;
    }
    else
    {
        const int score = static_cast<int>(Game::ProbabilityToCentipawns(value));
        std::cout << " score cp " << score;
    }

    std::cout << " nodes " << nodeCount << " nps " << nodesPerSecond << " time " << searchTimeMs
        << " hashfull " << hashfullPermille << " pv";
    for (Move move : principleVariation)
    {
        std::cout << " " << UCI::move(move, false /* chess960 */);
    }
    std::cout << std::endl;

    // Debug: print cache info.
    if (_searchConfig.debug)
    {
        std::cout << "info string [cache] hitrate " << PredictionCache::Instance.PermilleHits() <<
            " evictionrate " << PredictionCache::Instance.PermilleEvictions() << std::endl;
    }
}

void SelfPlayWorker::SignalDebug(bool debug)
{
    std::lock_guard lock(_searchConfig.mutexUci);

    _searchConfig.debug = debug;
}

void SelfPlayWorker::SignalPosition(std::string&& fen, std::vector<Move>&& moves)
{
    std::lock_guard lock(_searchConfig.mutexUci);

    _searchConfig.positionUpdated = true;
    _searchConfig.positionFen = std::move(fen);
    _searchConfig.positionMoves = std::move(moves);
}

void SelfPlayWorker::SignalSearchGo(const TimeControl& timeControl)
{
    std::lock_guard lock(_searchConfig.mutexUci);

    _searchConfig.searchUpdated = true;
    _searchConfig.search = true;
    _searchConfig.searchTimeControl = timeControl;

    _searchConfig.signalUci.notify_all();
}

void SelfPlayWorker::SignalSearchStop()
{
    std::lock_guard lock(_searchConfig.mutexUci);

    _searchConfig.searchUpdated = true;
    _searchConfig.search = false;
}

void SelfPlayWorker::SignalQuit()
{
    std::lock_guard lock(_searchConfig.mutexUci);

    _searchConfig.quit = true;

    _searchConfig.signalUci.notify_all();
}

void SelfPlayWorker::WaitUntilReady()
{
    std::unique_lock lock(_searchConfig.mutexUci);

    while (!_searchConfig.ready)
    {
        _searchConfig.signalReady.wait(lock);
    }
}

void SelfPlayWorker::SearchInitialize(int mctsParallelism)
{
    ClearGame(0);

    // Set up parallelism. Make N games share a tree but have their own image/value/policy slots.
    for (int i = 1; i < mctsParallelism; i++)
    {
        ClearGame(i);
        _states[i] = _states[0];
        _gameStarts[i] = _gameStarts[0];
        _games[i] = _games[0].SpawnShadow(&_images[i], &_values[i], &_policies[i]);
    }

    PredictionCache::Instance.ResetProbeMetrics();
}

void SelfPlayWorker::SearchPlay(int mctsParallelism)
{
    // Get an initial expansion of moves/children.
    SelfPlayState& primaryState = _states[0];
    SelfPlayGame& primaryGame = _games[0];

    if (!primaryGame.Root()->IsExpanded())
    {
        primaryGame.ExpandAndEvaluate(primaryState, _cacheStores[0]);
        if (primaryState == SelfPlayState::WaitingForPrediction)
        {
            return;
        }
    }

    for (int i = 0; i < mctsParallelism; i++)
    {
        RunMcts(_games[i], _scratchGames[i], _states[i], _mctsSimulations[i], _searchPaths[i], _cacheStores[i]);
    }
}