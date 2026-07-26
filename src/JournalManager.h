#pragma once

#include "Common.h"
#include "FileSystemEngine.h"
#include <atomic>
#include <vector>

enum class TransactionState
{
    None,
    Pending,        // Copying files is currently in progress
    CopyCompleted,  // All files copied, flushed, and verified. Ready for pointer swap.
    PointerSwapped, // Link created, cleanup/backup deletion is pending or in progress
    Aborted         // Operation failed/canceled, pending complete rollback
};

struct JournalData
{
    std::wstring uuid;
    std::wstring sourcePath;
    std::wstring physicalSourcePath;
    std::wstring destPath;
    TransactionState state = TransactionState::None;
    RedirectStrategy strategy = RedirectStrategy::Junction;
};

class JournalManager
{
private:
    std::wstring m_journalDirectory;
    std::wstring m_currentJournalPath;
    JournalData m_currentData;

    std::wstring GetJournalPathForUuid(const std::wstring &uuid) const;
    std::wstring GetTemporaryJournalPathForUuid(const std::wstring &uuid) const;
    bool WriteJournalAtomically(const JournalData &data);

public:
    explicit JournalManager(const std::wstring &appDataPath);

    // Dynamic scanning to collect any active or un-cleared journals in the system
    bool FindActiveJournals(std::vector<JournalData> &outJournals);

    // Scaffolds a new active transaction log
    bool StartTransaction(const std::wstring &sourcePath, const std::wstring &physicalSourcePath, const std::wstring &destPath, RedirectStrategy strategy, std::wstring &outUuid);

    // Updates state of current active journal atomically
    bool UpdateState(TransactionState newState);

    // Finalizes transaction, closing buffers and purging the journal entry file
    bool CommitTransaction();

    // Aborts transaction, setting the journal state to Aborted
    bool AbortTransaction();

    // Loads a journal from file by its unique identifier
    bool LoadJournal(const std::wstring &uuid, JournalData &outData);

    // Returns currently active transaction info
    JournalData GetCurrentTransaction() const { return m_currentData; }

    // --- The Decoupled 4-Stage Safety Pipeline ---

    // Stage 1: Allocation
    // Pre-allocates file size using SetFilePointerEx and SetEndOfFile to ensure contiguous space
    static bool PreallocateFile(const std::wstring &destPath, uint64_t fileSize);

    // Stage 2: Stream & Flush
    // Copies blocks while checking for cancellation, followed by forced commits to PLP drive cache
    static bool StreamAndFlushFile(
        const std::wstring &srcPath,
        const std::wstring &destPath,
        std::atomic<uint64_t> &progressBytes,
        std::atomic<bool> &cancelFlag);

    // Stage 3: Verification
    // Performs independent 128-bit block-by-block MurmurHash3 pass of source and dest to check integrity
    static bool VerifyFileIntegrity(const std::wstring &srcPath, const std::wstring &destPath);

    // Stage 4: Atomic Swap
    // Atomically renames source directory to backup, establishes redirection, and validates redirection loop
    static bool AtomicSwapDirectory(
        const std::wstring &srcPath,
        const std::wstring &destPath,
        RedirectStrategy strategy,
        std::wstring &outBackupPath,
        std::wstring &outErrorMsg);

    // --- Automated Crash Recovery Mechanics ---

    // Rollback recovery routine (cleans up destination files, restores original folders)
    bool RecoverRollback(const JournalData &journal, void (*progressCallback)(const std::wstring &));

    // Resume recovery routine (swaps pointer, verifies links, clears backup)
    bool RecoverResume(const JournalData &journal, void (*progressCallback)(const std::wstring &));
};
