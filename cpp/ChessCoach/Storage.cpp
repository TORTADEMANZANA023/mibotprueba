#include "Storage.h"

#include <string>
#include <sstream>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <set>
#include <ctime>

#include <google/protobuf/io/gzip_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <protobuf/ChessCoach.pb.h>

#include <crc32c/include/crc32c/crc32c.h>

#include "Config.h"
#include "Pgn.h"
#include "Platform.h"
#include "Preprocessing.h"
#include "Random.h"

Storage::Storage(const NetworkConfig& networkConfig, const MiscConfig& miscConfig, int trainingChunkCount)
    : _trainingChunkCount(trainingChunkCount)
    , _trainingGameCount(0)
    , _gamesPerChunk(miscConfig.Storage_GamesPerChunk)
    , _pgnInterval(networkConfig.Training.PgnInterval)
    , _sessionNonce("UNINITIALIZED")
    , _sessionGameCount(0)
    , _sessionChunkCount(0)
{
    const std::filesystem::path rootPath = Platform::UserDataPath();

    _relativeTrainingGamePath = networkConfig.Training.GamesPathTraining;
    _localTrainingGamePath = MakeLocalPath(rootPath, _relativeTrainingGamePath);
    _localLogsPath = MakeLocalPath(rootPath, miscConfig.Paths_Logs);
    _relativePgnsPath = miscConfig.Paths_Pgns;
}

void Storage::InitializeLocalGamesChunks(INetwork* network)
{
    // Use a 32-bit session nonce to help differentiate this run from others. Still secondary to timestamp in ordering.
    const std::string alphabet = "0123456789ABCDEF";
    std::uniform_int_distribution<> distribution(0, static_cast<int>(alphabet.size()) - 1);
    _sessionNonce = std::string(8, '\0');
    for (char& c : _sessionNonce)
    {
        c = alphabet[distribution(Random::Engine)];
    }

    // Count training games previously played and saved locally without yet being chunked.
    _trainingGameCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(_localTrainingGamePath))
    {
        if (entry.path().extension().string() == ".game")
        {
            _trainingGameCount++;
        }
    }

    // Try to chunk now in case we already have enough games (so zero would be played)
    // but they failed to chunk previously.
    if (_trainingGameCount >= _gamesPerChunk)
    {
        TryChunkMultiple(network);
    }
}

// AddTrainingGame can be called from multiple self-play worker threads.
int Storage::AddTrainingGame(INetwork* network, SavedGame&& game)
{
    // Give this game a number and filename.
    const int gameNumber = ++_sessionGameCount;
    const std::string filenameStem = GenerateFilename(gameNumber);

    // Save locally for chunking later.
    const std::filesystem::path localGamePath = _localTrainingGamePath / (filenameStem + ".game");
    SaveChunk(localGamePath, { game });

    // Occasionally save PGNs to central storage.
    if ((gameNumber % _pgnInterval) == 0)
    {
        const std::filesystem::path relativePgnPath = _relativePgnsPath / (filenameStem + ".pgn");

        std::stringstream buffer;
        Pgn::GeneratePgn(buffer, game);

        network->SaveFile(relativePgnPath.string(), buffer.str());
    }

    // When enough individual games have been saved, chunk and store centrally.
    // Use the atomic_int to ensure that only one caller attempts to chunk, and
    // assume that _gamesPerChunk is large enough that the chunking will finish
    // well before it is time for the next one.
    const int newTrainingGameCount = ++_trainingGameCount;
    if ((newTrainingGameCount % _gamesPerChunk) == 0)
    {
        TryChunkMultiple(network);
    }

    return gameNumber;
}

// Just in case anything went wrong with file I/O, etc. previously, attempt to
// create as many chunks as we can here. This is still safe with the outer atomic_int
// check as long as we finish before the next _gamesPerChunk cycle.
void Storage::TryChunkMultiple(INetwork* network)
{
    std::vector<std::filesystem::path> gamePaths;
    for (const auto& entry : std::filesystem::directory_iterator(_localTrainingGamePath))
    {
        if (entry.path().extension().string() == ".game")
        {
            gamePaths.emplace_back(entry.path());
        }
        if (gamePaths.size() == _gamesPerChunk)
        {
            ChunkGames(network, gamePaths);
            gamePaths.clear();
        }
    }
}

void Storage::ChunkGames(INetwork* network, std::vector<std::filesystem::path>& gamePaths)
{
    // Set up a buffer for the TFRecord file contents, compressing with zlib.
    // Reserve 128 MB in advance, roughly enough to hold any chunk.
    std::string buffer;
    {
        buffer.reserve(128 * 1024 * 1024);
        google::protobuf::io::StringOutputStream chunkWrapped(&buffer);
        google::protobuf::io::GzipOutputStream::Options zipOptions{};
        zipOptions.format = google::protobuf::io::GzipOutputStream::ZLIB;
        google::protobuf::io::GzipOutputStream chunkZip(&chunkWrapped, zipOptions);

        // Just decompress each individual game chunk with zlib and append to the chunk.
        for (auto& path : gamePaths)
        {
            CFile gameFile(path, false /* write */);
            google::protobuf::io::FileInputStream gameWrapped(gameFile.FileDescriptor());
            google::protobuf::io::GzipInputStream gameZip(&gameWrapped, google::protobuf::io::GzipInputStream::ZLIB);

            void* chunkBuffer;
            const void* gameBuffer;
            int chunkSize;
            int gameSize;

            // Ordering is important here: check short-lived "gameZip" first (is there data to read)
            // then long-lived "chunkZip" (can we write it) so that "BackUp" is never missed.
            while (gameZip.Next(&gameBuffer, &gameSize) && chunkZip.Next(&chunkBuffer, &chunkSize))
            {
                const int copySize = std::min(chunkSize, gameSize);
                std::memcpy(chunkBuffer, gameBuffer, copySize);

                chunkZip.BackUp(chunkSize - copySize);
                gameZip.BackUp(gameSize - copySize);
            }
        }
    }

    // Write the chunk to central storage.
    const int chunkNumber = ++_sessionChunkCount;
    const std::string& filename = GenerateFilename(chunkNumber) + ".chunk";
    const auto& relativePath = (_relativeTrainingGamePath / filename);
    std::cout << "Chunking " << _gamesPerChunk << " games to " << filename << std::endl;
    network->SaveFile(relativePath.string(), buffer);

    // Delete the individual games.
    for (auto& path : gamePaths)
    {
        std::filesystem::remove(path);
    }

    // Update stats.
    _trainingChunkCount++;
    _trainingGameCount -= _gamesPerChunk;
}

// Training is only done on chunks, not individual games, so round the target up to the nearest chunk.
int Storage::TrainingGamesToPlay(int targetCount) const
{
    const int existingCount = ((_trainingChunkCount * _gamesPerChunk) + _trainingGameCount);
    targetCount = (((targetCount + _gamesPerChunk - 1) / _gamesPerChunk) * _gamesPerChunk);
    return std::max(0, targetCount - existingCount);
}

std::string Storage::GenerateSimpleChunkFilename(int chunkNumber) const
{
    std::stringstream suffix;
    suffix << std::setfill('0') << std::setw(9) << chunkNumber << ".chunk";
    return suffix.str();
}

std::string Storage::GenerateFilename(int number)
{
    std::stringstream filename;

    // Format as "YYmmdd_HHMMSS_milliseconds_sessionnonce_number"; e.g. "20201022_181546_008_24FFE8F502A72C8D_000000005".
    // Use UCT rather than local time for comparability across multiple machines, including a local/cloud mix.
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
#pragma warning(disable:4996) // Internal buffer is immediately consumed and detached.
    filename << std::put_time(std::gmtime(&time), "%Y%m%d_%H%M%S");
#pragma warning(disable:4996) // Internal buffer is immediately consumed and detached.
    filename << "_" << std::setfill('0') << std::setw(3) << milliseconds << "_" << _sessionNonce << "_"
        << std::setfill('0') << std::setw(9) << number;
    
    return filename.str();
}

void Storage::SaveChunk(const std::filesystem::path& path, const std::vector<SavedGame>& games) const
{
    // Compress the TFRecord file using zlib.
    CFile file(path, true /* write */);
    google::protobuf::io::FileOutputStream wrapped(file.FileDescriptor());
    google::protobuf::io::GzipOutputStream::Options zipOptions{};
    zipOptions.format = google::protobuf::io::GzipOutputStream::ZLIB;
    google::protobuf::io::GzipOutputStream zip(&wrapped, zipOptions);

    // Write a "tf.train.Example" protobuf for each game as a TFRecord.
    message::Example storeGame;
    std::string buffer;
    for (const SavedGame& game : games)
    {
        PopulateGame(_startingPosition, game, storeGame);
        WriteTfRecord(zip, buffer, storeGame);
    }
}

void Storage::PopulateGame(Game scratchGame, const SavedGame& game, message::Example& gameOut)
{
    auto& features = *gameOut.mutable_features()->mutable_feature();

    // Write result directly.
    auto& result = *features["result"].mutable_float_list()->mutable_value();
    result.Clear();
    result.Add(game.result);

    // Write MCTS values directly.
    auto& mctsValues = *features["mcts_values"].mutable_float_list()->mutable_value();
    mctsValues.Clear();
    mctsValues.Reserve(game.moveCount);
    mctsValues.AddNAlreadyReserved(game.moveCount);
    std::copy(game.mctsValues.begin(), game.mctsValues.end(), mctsValues.mutable_data());

    // Fix up result and MCTS value.
    // MCTS deals with probabilities in [0, 1]. Network deals with tanh outputs/targets in (-1, 1)/[-1, 1].
    INetwork::MapProbabilities01To11(result.size(), result.mutable_data());
    INetwork::MapProbabilities01To11(mctsValues.size(), mctsValues.mutable_data());

    // Image and policy require applying moves to a scratch game, so process a move-at-once.
    // Policy indices/values are ragged, so reserve for each move.
    auto& imagePiecesAuxiliary = *features["image_pieces_auxiliary"].mutable_int64_list()->mutable_value();
    imagePiecesAuxiliary.Clear();
    const int imagePiecesAuxiliaryStride = (INetwork::InputPiecePlanesPerPosition + INetwork::InputAuxiliaryPlaneCount);
    const int imagePiecesAuxiliaryTotalSize = (game.moveCount * imagePiecesAuxiliaryStride);
    imagePiecesAuxiliary.Reserve(imagePiecesAuxiliaryTotalSize);
    imagePiecesAuxiliary.AddNAlreadyReserved(imagePiecesAuxiliaryTotalSize);

    auto& policyRowLengths = *features["policy_row_lengths"].mutable_int64_list()->mutable_value();
    policyRowLengths.Clear();
    policyRowLengths.Reserve(game.moveCount);
    policyRowLengths.AddNAlreadyReserved(game.moveCount);

    auto& policyIndices = *features["policy_indices"].mutable_int64_list()->mutable_value();
    policyIndices.Clear();
    auto& policyValues = *features["policy_values"].mutable_float_list()->mutable_value();
    policyValues.Clear();

    for (int m = 0; m < game.moveCount; m++)
    {
        INetwork::PackedPlane* imagePiecesOut = reinterpret_cast<INetwork::PackedPlane*>(imagePiecesAuxiliary.mutable_data()) + (m * imagePiecesAuxiliaryStride);
        INetwork::PackedPlane* imageAuxiliaryOut = (imagePiecesOut + INetwork::InputPiecePlanesPerPosition);
        scratchGame.GenerateImageCompressed(imagePiecesOut, imageAuxiliaryOut);

        const int movePolicyIndexCount = static_cast<int>(game.childVisits[m].size());
        policyRowLengths[m] = movePolicyIndexCount;

        const int cumulativePolicyIndexCountOld = policyIndices.size();
        const int cumulativePolicyIndexCountNew = (cumulativePolicyIndexCountOld + movePolicyIndexCount);
        policyIndices.Reserve(cumulativePolicyIndexCountNew);
        policyIndices.AddNAlreadyReserved(movePolicyIndexCount);
        policyValues.Reserve(cumulativePolicyIndexCountNew);
        policyValues.AddNAlreadyReserved(movePolicyIndexCount);
        scratchGame.GeneratePolicyCompressed(game.childVisits[m],
            policyIndices.mutable_data() + cumulativePolicyIndexCountOld,
            policyValues.mutable_data() + cumulativePolicyIndexCountOld);

        scratchGame.ApplyMove(Move(game.moves[m]));
    }
}

// https://www.tensorflow.org/tutorials/load_data/tfrecord
// https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/example/example.proto
// https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/example/feature.proto
//
// TFRecords format details
//
// A TFRecord file contains a sequence of records. The file can only be read sequentially.
//
// Each record contains a byte-string, for the data-payload, plus the data-length, and CRC32C (32-bit CRC using the Castagnoli polynomial) hashes for integrity checking.
//
// Each record is stored in the following formats:
//
// uint64 length
// uint32 masked_crc32_of_length
// byte   data[length]
// uint32 masked_crc32_of_data
//
// The records are concatenated together to produce the file. CRCs are described here, and the mask of a CRC is:
//
// masked_crc = ((crc >> 15) | (crc << 17)) + 0xa282ead8ul
//
void Storage::WriteTfRecord(google::protobuf::io::ZeroCopyOutputStream& stream, std::string& buffer, const google::protobuf::Message& message)
{
    // Prepare a stream wrapper that can do efficient size-ensuring for writes.
    uint8_t* target;
    google::protobuf::io::EpsCopyOutputStream epsCopy(&stream,
        google::protobuf::io::CodedOutputStream::IsDefaultSerializationDeterministic(),
        &target);

    // Serialize the message.
    message.SerializeToString(&buffer);

    // Write the header: length + masked_crc32_of_length
    const uint64_t length = buffer.size();
    const uint32_t lengthCrc = MaskCrc32cForTfRecord(crc32c::Crc32c(reinterpret_cast<const uint8_t*>(&length), sizeof(length)));

    // Just assume we're running on little-endian.
    target = epsCopy.EnsureSpace(target);
    std::memcpy(target, &length, sizeof(length));
    target += sizeof(length);
    std::memcpy(target, &lengthCrc, sizeof(lengthCrc));
    target += sizeof(lengthCrc);

    // Write the payload: data[length]
    target = epsCopy.WriteRaw(buffer.data(), static_cast<int>(buffer.size()), target);

    // Write the footer: masked_crc32_of_data
    const uint32_t dataCrc = MaskCrc32cForTfRecord(crc32c::Crc32c(buffer.data(), buffer.size()));

    // Only writing one datum, same cost as EnsureSpace. Again, just assume we're running on little-endian.
    target = epsCopy.WriteRaw(&dataCrc, sizeof(dataCrc), target);
    epsCopy.Trim(target);
}

uint32_t Storage::MaskCrc32cForTfRecord(uint32_t crc32c)
{
    return ((crc32c >> 15) | (crc32c << 17)) + 0xa282ead8ul;
}

std::filesystem::path Storage::LocalLogPath() const
{
    return _localLogsPath;
}

// We can only write to local paths from C++ and need to call in to Python to write to gs:// locations
// when running on Google Cloud with TPUs. That's okay for local gathering/chunking of games, and UCI logging.
std::filesystem::path Storage::MakeLocalPath(const std::filesystem::path& root, const std::filesystem::path& path)
{
    // Empty paths have special meaning as N/A.
    if (path.empty())
    {
        return path;
    }

    // Root any relative paths at ChessCoach's appdata directory.
    if (path.is_absolute())
    {
        std::filesystem::create_directories(path);
        return path;
    }

    const std::filesystem::path rooted = (root / path);
    std::filesystem::create_directories(rooted);
    return rooted;
}

void Storage::SaveCommentary(const std::filesystem::path& path, const std::vector<SavedGame>& games,
    std::vector<SavedCommentary>& gameCommentary, Vocabulary& vocabulary) const
{
    // Compress the TFRecord file using zlib.
    CFile file(path, true /* write */);
    google::protobuf::io::FileOutputStream wrapped(file.FileDescriptor());
    google::protobuf::io::GzipOutputStream::Options zipOptions{};
    zipOptions.format = google::protobuf::io::GzipOutputStream::ZLIB;
    google::protobuf::io::GzipOutputStream zip(&wrapped, zipOptions);

    // Write a single "tf.train.Example" protobuf for all images/comments as a TFRecord.
    message::Example store;
    std::string buffer;
    auto& features = *store.mutable_features()->mutable_feature();
    auto& images = *features["images"].mutable_int64_list()->mutable_value();
    auto& comments = *features["comments"].mutable_bytes_list()->mutable_value();
    const int imageStride = INetwork::InputPlaneCount;

    const Preprocessor preprocessor;

    for (int i = 0; i < games.size(); i++)
    {
        const SavedGame& game = games[i];
        SavedCommentary commentary = gameCommentary[i];

        // Require that commentary for this game refers to positions in order
        // and play out a single base scratch game, then branch off variations.
        //
        // Variations "override" the last real move and so will regress the move index,
        // so sort comments by move index here.
        Game scratchGame = _startingPosition;
        struct {
            bool operator()(const SavedComment& a, const SavedComment& b) const
            {
                return (a.moveIndex < b.moveIndex);
            }
        } compareMoveIndex;
        std::sort(commentary.comments.begin(), commentary.comments.end(), compareMoveIndex);

        for (SavedComment& comment : commentary.comments)
        {
            preprocessor.PreprocessComment(comment.comment);
            if (!comment.comment.empty())
            {
                // Update vocabulary.
                vocabulary.commentCount++;
                vocabulary.vocabulary.insert(comment.comment);

                // Write the comment directly (don't touch "comment.comment" after this).
                comments.Add(std::move(comment.comment));

                // Prepare the image for writing.
                const int imageSizeOld = images.size();
                const int imageSizeNew = (imageSizeOld + imageStride);
                images.Reserve(imageSizeNew);
                images.AddNAlreadyReserved(imageStride);
                INetwork::PackedPlane* imageOut = (reinterpret_cast<INetwork::PackedPlane*>(images.mutable_data()) + imageSizeOld);

                // Find the position for the chosen comment and populate the image.
                //
                // For now interpret the comment as refering to the position after playing the move,
                // so play moves up to *and including* the stored moveIndex.
                for (int m = scratchGame.Ply(); m <= comment.moveIndex; m++)
                {
                    // Some commentary games include null moves in the actual game, not just variations
                    // (e.g. at the very end to add a summary comment) so allow them here too.
                    scratchGame.ApplyMoveMaybeNull(Move(game.moves[m]));
                }

                // Also play out the variation.
                Game variation = scratchGame;
                for (uint16_t move : comment.variationMoves)
                {
                    variation.ApplyMoveMaybeNull(Move(move));
                }

                // Write the full image: no compression for commentary because of branching variation structure.
                variation.GenerateImage(imageOut);
            }
        }
    }

    // Write the "tf.train.Example" protobuf.
    WriteTfRecord(zip, buffer, store);
}