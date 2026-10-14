#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Threading/Thread.h>
#include <Foundation/Threading/ThreadSignal.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/UniquePtr.h>
#include <PlasmaAudioPlugin/Codecs/AudioByteSource.h>

class plAudioClipResource;
class plAudioStreamService;

/// \brief The payload of one playing streamed clip: priming data from the clip resource plus a ring buffer that the
/// IO thread of plAudioStreamService fills from disk.
///
/// All offsets are payload byte offsets. Read() and IsAvailable() are called by the decoder on the audio thread and
/// never block or allocate. A range is served from, in this order:
/// - the ring, when the ring holds it (reads skip forward inside the ring, the bytes before are dropped),
/// - the start priming span [0, priming bytes) or the loop priming span [loop priming offset, + loop priming bytes),
///   the one reaching further when both contain the offset.
/// Anything else is a miss: the call reports no data, counts an underrun and, if the ring is not already heading
/// for that offset, requests a seek so the IO thread refills the ring from there.
///
/// Whenever a range is served from a priming span, the ring must continue at the end of that span. If it cannot
/// (after a loop jump the ring is far past the loop priming data), a seek to the span end is requested, so the
/// ring refills while the decoder plays the priming bytes.
///
/// Ring protocol (single producer = IO thread, single consumer = audio thread): m_iWritePos and m_iReadPos are
/// absolute payload offsets, ring index = offset & mask. The consumer requests a seek by writing m_iSeekTarget and
/// incrementing m_iSeekGeneration. While m_iReadyGeneration differs from the requested generation the consumer
/// does not touch the ring; the IO thread repositions the file, sets both positions to the target and then
/// publishes the generation in m_iReadyGeneration.
///
/// The stream stores pointers into the priming arrays of the clip resource. Whoever creates the voice keeps the
/// resource acquired until the stream is released.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioStream final : public plAudioByteSource
{
  PL_DISALLOW_COPY_AND_ASSIGN(plAudioStream);

public:
  plAudioStream();
  ~plAudioStream();

  virtual plUInt32 Read(plUInt64 uiOffset, void* pOut, plUInt32 uiBytes) override;
  virtual bool IsAvailable(plUInt64 uiOffset, plUInt32 uiBytes) override;

  /// \brief Number of reads that found no data.
  plInt32 GetUnderruns() const { return m_iUnderruns; }

  /// \brief Number of seeks requested by the consumer (including the initial fill).
  plInt32 GetSeekCount() const { return m_iSeekGeneration; }

  /// \brief Whether the payload is read through plFileReader (see plAudioStreamService) rather than plOSFile.
  bool IsReadingThroughFileSystem() const { return m_sOsFilePath.IsEmpty(); }

  /// \brief Number of times the IO thread opened the file (plFileReader reopens it for every backward seek).
  plInt32 GetFileOpenCount() const { return m_iFileOpens; }

  plUInt64 GetPayloadBytes() const { return m_uiPayloadBytes; }
  plUInt32 GetRingBytes() const { return m_Ring.GetCount(); }
  plUInt32 GetChunkBytes() const { return m_uiChunkBytes; }

  /// \brief Whether the ring is positioned and the IO thread has nothing left to read for it right now (less than a
  /// chunk free, or filled up to the end of the payload). Any thread; used by tests to wait for the IO thread.
  bool IsFilled() const;

private:
  friend class plAudioStreamService;

  struct Span
  {
    const plUInt8* m_pData = nullptr;
    plUInt64 m_uiBegin = 0;
    plUInt64 m_uiEnd = 0;

    bool Contains(plUInt64 uiOffset) const { return uiOffset >= m_uiBegin && uiOffset < m_uiEnd; }
  };

  // audio thread
  bool Serve(plUInt64 uiOffset, void* pOut, plUInt32 uiBytes);
  const Span* FindSpan(plUInt64 uiOffset) const;
  bool IsRingReady() const;
  bool IsRingHeadingFor(plUInt64 uiOffset) const;
  void RequestSeek(plUInt64 uiOffset);
  void OnConsumed();

  // IO thread, service mutex held: handles a pending seek or reads one chunk; returns whether it did either
  bool UpdateIo();

  // IO thread: positions the file at payload offset uiOffset, opening it first if needed
  plResult SeekFile(plUInt64 uiOffset);

  // IO thread: reads from the current file position
  plUInt64 ReadFile(void* pBuffer, plUInt64 uiBytes);

  // closes the file and clears the per-clip state, keeping the ring memory; only for streams the IO thread no longer sees
  void ResetForReuse();

  plAudioStreamService* m_pService = nullptr;

  // set on acquire, constant while the stream is active
  plString m_sOsFilePath; ///< plain file on disk, read with plOSFile; empty: read through plFileReader on m_sResourceId
  plString m_sResourceId;
  plUInt64 m_uiPayloadFileOffset = 0;
  plUInt64 m_uiPayloadBytes = 0;
  Span m_Priming;
  Span m_LoopPriming;
  plDynamicArray<plUInt8> m_Ring;
  plUInt64 m_uiRingMask = 0;
  plUInt32 m_uiChunkBytes = 0;

  // shared between the audio thread and the IO thread
  plAtomicInteger64 m_iWritePos;
  plAtomicInteger64 m_iReadPos;
  plAtomicInteger64 m_iSeekTarget;
  plAtomicInteger32 m_iSeekGeneration;
  plAtomicInteger32 m_iReadyGeneration;
  plAtomicInteger32 m_iUnderruns;
  plAtomicInteger32 m_iFileOpens;
  plAtomicBool m_bFailed;

  // audio thread only
  plUInt64 m_uiRingOrigin = 0; ///< target of the last requested seek

  // IO thread only
  plOSFile m_File;         ///< used when m_sOsFilePath is set
  plFileReader m_FsFile;   ///< used otherwise
  plUInt64 m_uiFsFilePos = 0; ///< file offset m_FsFile is positioned at
  plUInt64 m_uiFilePos = 0; ///< payload offset the file is positioned at
};

/// \brief Reads the payload of streamed clips from disk on one IO thread.
///
/// A clip that is a plain file on disk (plAudioClipResource::GetOsFilePath()) is read with plOSFile, opened with
/// plFileShareMode::SharedReadsAndDelete so that the file can be replaced (re-transformed) while it plays. Any other
/// clip, e.g. one inside a package archive, is read through the file system with plFileReader on its resource id.
/// plFileReader only reads forward: a seek forward skips bytes, a seek backward reopens the file and skips from the
/// start. Skipping is cheap in plain folders and uncompressed archive entries (a memory-mapped archive), but inside a
/// compressed archive entry it decompresses everything skipped, so backward seeks (loop jumps) cost up to a
/// decompression of the file up to the target.
///
/// Owned by the middleware (Startup / Shutdown). AcquireStream() and ReleaseStream() are called when a voice of a
/// streamed clip starts and stops (ATL thread); they take the service mutex, which the IO thread holds only while
/// it works on one stream step (one seek or one chunk read). The audio thread never takes the mutex.
///
/// Released stream objects are kept in a pool and reused by later acquires; a reused stream keeps its ring memory
/// when the chunk size is unchanged, so starting a streamed voice does not allocate a stream or a ring once the pool
/// holds enough streams. The pool is freed at Shutdown().
///
/// CVars:
/// - PlasmaAudio.MaxStreams (32): streams that can be open at once; AcquireStream() fails beyond that.
/// - PlasmaAudio.StreamChunkKB (64): size of one disk read, rounded down to a power of two in [16, 1024]. The ring
///   of a stream holds four chunks. Applies to streams acquired after the change. The priming data baked into the
///   clips (plAudioClipConstants::PrimingBytes) does not depend on it.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioStreamService
{
  PL_DISALLOW_COPY_AND_ASSIGN(plAudioStreamService);

public:
  plAudioStreamService();
  ~plAudioStreamService();

  /// \brief Starts the IO thread.
  void Startup();

  /// \brief Stops the IO thread and releases all streams that are still open.
  void Shutdown();

  bool IsRunning() const { return m_pThread != nullptr; }

  /// \brief Opens a stream on the payload of a loaded streamed clip, or returns nullptr (service not running, stream
  /// limit reached, clip not streamed). The IO thread starts filling the ring right after the start priming data.
  plAudioStream* AcquireStream(const plAudioClipResource& clip);

  /// \brief Closes a stream. The decoder reading from it must not be used any more.
  void ReleaseStream(plAudioStream* pStream);

  plUInt32 GetActiveStreamCount() const;
  plUInt32 GetMaxStreams() const;

  /// \brief Number of open streams reading the clip with the given resource id.
  plUInt32 GetActiveStreamCount(plStringView sClipResourceId) const;

  /// \brief Number of released stream objects kept for reuse.
  plUInt32 GetPooledStreamCount() const;

  /// \brief Heap bytes of the rings of all stream objects the service holds, open and pooled.
  plUInt64 GetRingBytesInUse() const;

  /// \brief Test hook: while paused the IO thread does no work (no fills, no seeks), so reads past the priming data
  /// starve.
  void SetIoPaused(bool bPaused);

  /// \brief Test hook: streams acquired while set read through plFileReader even when the clip is a plain file on disk.
  void SetForceFileSystemReads(bool bForce) { m_bForceFileSystemReads = bForce; }

  /// \brief Test hook: blocks until every stream is filled (see plAudioStream::IsFilled) or failed. Offline renders
  /// consume faster than realtime; tests call this between render blocks. Returns false on timeout.
  bool WaitForIdle(plTime timeout = plTime::MakeFromSeconds(10));

  /// \brief Wakes the IO thread. Any thread; does not block beyond a short lock inside plThreadSignal.
  void WakeIo();

private:
  friend class plAudioStreamThread;

  /// \brief One pass over all streams; returns whether any stream did work.
  bool RunIoPass();

  mutable plMutex m_Mutex;
  plDynamicArray<plAudioStream*> m_Streams;
  plDynamicArray<plAudioStream*> m_FreeStreams; ///< released streams, reused by AcquireStream(); the IO thread never sees them

  plUniquePtr<plThread> m_pThread;
  plThreadSignal m_Signal;
  plAtomicBool m_bStopThread;
  plAtomicBool m_bIoPaused;
  plAtomicBool m_bWakePending;
  plAtomicBool m_bForceFileSystemReads;
};
