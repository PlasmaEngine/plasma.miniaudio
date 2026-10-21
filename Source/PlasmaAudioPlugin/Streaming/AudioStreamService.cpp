#include <PlasmaAudioPlugin/PlasmaAudioPluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <PlasmaAudioPlugin/Clips/AudioClipResource.h>
#include <PlasmaAudioPlugin/Streaming/AudioStreamService.h>

plCVarInt cvar_PlasmaAudioMaxStreams("PlasmaAudio.MaxStreams", 32, plCVarFlags::Default, "Number of streamed clip voices that can play at once");
plCVarInt cvar_PlasmaAudioStreamChunkKB("PlasmaAudio.StreamChunkKB", 64, plCVarFlags::Default, "Size of one disk read of a streamed clip in KB (power of two, 16 - 1024); the ring of a stream holds four chunks. Applies to streams opened after the change.");

namespace
{
  constexpr plUInt32 s_uiRingChunks = 4;

  plUInt32 GetChunkBytes()
  {
    const plUInt32 uiKB = static_cast<plUInt32>(plMath::Clamp(cvar_PlasmaAudioStreamChunkKB.GetValue(), 16, 1024));

    plUInt32 uiPow2 = 16;
    while (uiPow2 * 2 <= uiKB)
      uiPow2 *= 2;

    return uiPow2 * 1024;
  }
} // namespace

/// The IO thread: works while any stream has work, then sleeps until woken (or a short timeout as a safety net).
class plAudioStreamThread final : public plThread
{
public:
  explicit plAudioStreamThread(plAudioStreamService* pService)
    : plThread("PlasmaAudio Streaming")
    , m_pService(pService)
  {
  }

private:
  virtual plUInt32 Run() override
  {
    while (!m_pService->m_bStopThread)
    {
      m_pService->m_bWakePending = false;

      if (!m_pService->RunIoPass())
      {
        m_pService->m_WakeSemaphore.TryAcquireToken(plTime::MakeFromMilliseconds(50)).IgnoreResult();
      }
    }

    return 0;
  }

  plAudioStreamService* m_pService = nullptr;
};

// -------------------------------------------------------------------------------------------
// plAudioStream
// -------------------------------------------------------------------------------------------

plAudioStream::plAudioStream() = default;

plAudioStream::~plAudioStream()
{
  m_File.Close();
  m_FsFile.Close();
}

plUInt32 plAudioStream::Read(plUInt64 uiOffset, void* pOut, plUInt32 uiBytes)
{
  if (pOut == nullptr)
    return 0;

  return Serve(uiOffset, pOut, uiBytes) ? uiBytes : 0;
}

bool plAudioStream::IsAvailable(plUInt64 uiOffset, plUInt32 uiBytes)
{
  return Serve(uiOffset, nullptr, uiBytes);
}

bool plAudioStream::IsFilled() const
{
  if (m_bFailed)
    return true;

  if (m_iSeekGeneration != m_iReadyGeneration)
    return false;

  const plUInt64 uiRead = static_cast<plUInt64>(m_iReadPos);
  const plUInt64 uiWrite = static_cast<plUInt64>(m_iWritePos);
  // the same condition that stops the IO thread from reading another chunk
  return uiWrite >= m_uiPayloadBytes || m_Ring.GetCount() - (uiWrite - uiRead) < m_uiChunkBytes;
}

const plAudioStream::Span* plAudioStream::FindSpan(plUInt64 uiOffset) const
{
  const bool bStart = m_Priming.Contains(uiOffset);
  const bool bLoop = m_LoopPriming.Contains(uiOffset);

  if (bStart && bLoop)
    return m_LoopPriming.m_uiEnd > m_Priming.m_uiEnd ? &m_LoopPriming : &m_Priming;

  if (bStart)
    return &m_Priming;

  if (bLoop)
    return &m_LoopPriming;

  return nullptr;
}

bool plAudioStream::IsRingReady() const
{
  // the consumer is the only writer of m_iSeekGeneration
  return m_iReadyGeneration == m_iSeekGeneration;
}

bool plAudioStream::IsRingHeadingFor(plUInt64 uiOffset) const
{
  if (m_bFailed)
    return true; // nothing the IO thread could do

  const plUInt64 uiBase = IsRingReady() ? static_cast<plUInt64>(m_iReadPos) : m_uiRingOrigin;
  return uiOffset >= uiBase && uiOffset < uiBase + m_Ring.GetCount();
}

void plAudioStream::RequestSeek(plUInt64 uiOffset)
{
  m_uiRingOrigin = uiOffset;
  m_iSeekTarget = static_cast<plInt64>(uiOffset);
  m_iSeekGeneration.Increment();
  m_pService->WakeIo();
}

void plAudioStream::OnConsumed()
{
  const plUInt64 uiFill = static_cast<plUInt64>(m_iWritePos) - static_cast<plUInt64>(m_iReadPos);
  if (uiFill < m_Ring.GetCount() / 2 && static_cast<plUInt64>(m_iWritePos) < m_uiPayloadBytes)
  {
    m_pService->WakeIo();
  }
}

bool plAudioStream::Serve(plUInt64 uiOffset, void* pOut, plUInt32 uiBytes)
{
  const plUInt64 uiEnd = uiOffset + uiBytes;
  if (uiEnd > m_uiPayloadBytes)
    return false;

  const bool bReady = IsRingReady();
  const plUInt64 uiReadPos = bReady ? static_cast<plUInt64>(m_iReadPos) : 0;
  const plUInt64 uiWritePos = bReady ? static_cast<plUInt64>(m_iWritePos) : 0;

  // first pass: is the whole range there?
  plUInt64 uiCursor = uiOffset;
  plUInt64 uiLastSpanEnd = 0;

  while (uiCursor < uiEnd)
  {
    if (bReady && uiCursor >= uiReadPos && uiCursor < uiWritePos)
    {
      uiCursor = plMath::Min(uiEnd, uiWritePos);
      uiLastSpanEnd = 0; // the read continues in the ring, which is therefore where it should be
      continue;
    }

    if (const Span* pSpan = FindSpan(uiCursor))
    {
      uiCursor = plMath::Min(uiEnd, pSpan->m_uiEnd);
      uiLastSpanEnd = pSpan->m_uiEnd;
      continue;
    }

    break;
  }

  if (uiCursor < uiEnd)
  {
    m_iUnderruns.Increment();
    m_pService->CountUnderrun();

    if (!IsRingHeadingFor(uiCursor))
    {
      RequestSeek(uiCursor);
    }
    else
    {
      m_pService->WakeIo();
    }

    return false;
  }

  if (pOut == nullptr)
    return true;

  // second pass: copy, with the same ring window as the first pass
  plUInt8* pTarget = static_cast<plUInt8*>(pOut);
  uiCursor = uiOffset;

  while (uiCursor < uiEnd)
  {
    if (bReady && uiCursor >= uiReadPos && uiCursor < uiWritePos)
    {
      plUInt64 uiCount = plMath::Min(uiEnd, uiWritePos) - uiCursor;

      while (uiCount > 0)
      {
        const plUInt64 uiIndex = uiCursor & m_uiRingMask;
        const plUInt64 uiPart = plMath::Min<plUInt64>(uiCount, m_Ring.GetCount() - uiIndex);
        plMemoryUtils::Copy(pTarget + (uiCursor - uiOffset), m_Ring.GetData() + uiIndex, static_cast<size_t>(uiPart));
        uiCursor += uiPart;
        uiCount -= uiPart;
      }

      continue;
    }

    const Span* pSpan = FindSpan(uiCursor);
    const plUInt64 uiPartEnd = plMath::Min(uiEnd, pSpan->m_uiEnd);
    plMemoryUtils::Copy(pTarget + (uiCursor - uiOffset), pSpan->m_pData + (uiCursor - pSpan->m_uiBegin), static_cast<size_t>(uiPartEnd - uiCursor));
    uiCursor = uiPartEnd;
  }

  // reads are sequential: everything in the ring before the end of this read is no longer needed
  if (bReady)
  {
    const plUInt64 uiNewReadPos = plMath::Min(uiEnd, uiWritePos);
    if (uiNewReadPos > uiReadPos)
    {
      m_iReadPos = static_cast<plInt64>(uiNewReadPos);
    }
  }

  // after priming data the ring has to continue at the end of the span
  if (uiLastSpanEnd > 0 && uiLastSpanEnd < m_uiPayloadBytes && !IsRingHeadingFor(uiLastSpanEnd))
  {
    RequestSeek(uiLastSpanEnd);
  }
  else if (IsRingReady())
  {
    OnConsumed();
  }

  return true;
}

bool plAudioStream::UpdateIo()
{
  const plInt32 iGeneration = m_iSeekGeneration;

  if (iGeneration != m_iReadyGeneration)
  {
    const plUInt64 uiTarget = static_cast<plUInt64>(static_cast<plInt64>(m_iSeekTarget));

    if (uiTarget < m_uiPayloadBytes && !m_bFailed && SeekFile(uiTarget).Failed())
    {
      m_bFailed = true;
    }

    m_iReadPos = static_cast<plInt64>(uiTarget);
    m_iWritePos = static_cast<plInt64>(uiTarget);
    m_iReadyGeneration = iGeneration;
    return true;
  }

  if (m_bFailed)
    return false;

  const plUInt64 uiWrite = static_cast<plUInt64>(m_iWritePos);
  const plUInt64 uiRead = static_cast<plUInt64>(m_iReadPos);

  if (uiWrite >= m_uiPayloadBytes || m_Ring.GetCount() - (uiWrite - uiRead) < m_uiChunkBytes)
    return false;

  const plUInt64 uiCount = plMath::Min<plUInt64>(m_uiChunkBytes, m_uiPayloadBytes - uiWrite);

  if (m_uiFilePos != uiWrite && SeekFile(uiWrite).Failed())
  {
    m_bFailed = true;
    return true;
  }

  plUInt64 uiDone = 0;
  while (uiDone < uiCount)
  {
    const plUInt64 uiIndex = (uiWrite + uiDone) & m_uiRingMask;
    const plUInt64 uiPart = plMath::Min<plUInt64>(uiCount - uiDone, m_Ring.GetCount() - uiIndex);

    const plUInt64 uiRead = ReadFile(m_Ring.GetData() + uiIndex, uiPart);
    m_uiFilePos += uiRead;

    if (uiRead != uiPart)
    {
      plLog::Error("PlasmaAudio: the file of audio clip '{}' ended before its payload", m_sResourceId);
      m_bFailed = true;
      return true;
    }

    uiDone += uiPart;
  }

  m_iWritePos = static_cast<plInt64>(uiWrite + uiCount);
  return true;
}

plResult plAudioStream::SeekFile(plUInt64 uiOffset)
{
  const plUInt64 uiFileOffset = m_uiPayloadFileOffset + uiOffset;

  if (!m_sOsFilePath.IsEmpty())
  {
    if (!m_File.IsOpen())
    {
      if (m_File.Open(m_sOsFilePath, plFileOpenMode::Read, plFileShareMode::SharedReadsAndDelete).Failed())
      {
        plLog::Error("PlasmaAudio: cannot open '{}' to stream audio clip '{}'", m_sOsFilePath, m_sResourceId);
        return PL_FAILURE;
      }

      m_iFileOpens.Increment();
    }

    m_File.SetFilePosition(static_cast<plInt64>(uiFileOffset), plFileSeekMode::FromStart);
    m_uiFilePos = uiOffset;
    return PL_SUCCESS;
  }

  // plFileReader reads forward only: a backward seek starts over from the beginning of the file
  if (!m_FsFile.IsOpen() || uiFileOffset < m_uiFsFilePos)
  {
    m_FsFile.Close();

    // a small cache: chunk reads bypass it and go straight to the data directory reader
    if (m_FsFile.Open(m_sResourceId, 4 * 1024, plFileShareMode::SharedReadsAndDelete).Failed())
    {
      plLog::Error("PlasmaAudio: cannot open '{}' to stream it", m_sResourceId);
      return PL_FAILURE;
    }

    m_iFileOpens.Increment();
    m_uiFsFilePos = 0;
  }

  const plUInt64 uiSkip = uiFileOffset - m_uiFsFilePos;
  const plUInt64 uiSkipped = (uiSkip > 0) ? m_FsFile.SkipBytes(uiSkip) : 0;
  m_uiFsFilePos += uiSkipped;

  if (uiSkipped != uiSkip)
  {
    plLog::Error("PlasmaAudio: the file of audio clip '{}' ended before its payload", m_sResourceId);
    return PL_FAILURE;
  }

  m_uiFilePos = uiOffset;
  return PL_SUCCESS;
}

plUInt64 plAudioStream::ReadFile(void* pBuffer, plUInt64 uiBytes)
{
  if (!m_sOsFilePath.IsEmpty())
    return m_File.Read(pBuffer, uiBytes);

  const plUInt64 uiRead = m_FsFile.ReadBytes(pBuffer, uiBytes);
  m_uiFsFilePos += uiRead;
  return uiRead;
}

void plAudioStream::ResetForReuse()
{
  m_File.Close();
  m_FsFile.Close();

  m_Priming = Span();
  m_LoopPriming = Span();
  m_uiPayloadFileOffset = 0;
  m_uiPayloadBytes = 0;
  m_uiRingOrigin = 0;
  m_uiFsFilePos = 0;
  m_uiFilePos = 0;

  m_iWritePos = 0;
  m_iReadPos = 0;
  m_iSeekTarget = 0;
  m_iSeekGeneration = 0;
  m_iReadyGeneration = 0;
  m_iUnderruns = 0;
  m_iFileOpens = 0;
  m_bFailed = false;
}

// -------------------------------------------------------------------------------------------
// plAudioStreamService
// -------------------------------------------------------------------------------------------

plAudioStreamService::plAudioStreamService() = default;

plAudioStreamService::~plAudioStreamService()
{
  Shutdown();
}

void plAudioStreamService::Startup()
{
  if (m_pThread != nullptr)
    return;

  m_bStopThread = false;
  m_bIoPaused = false;
  m_bWakePending = false;

  if (m_WakeSemaphore.Create(0).Failed())
  {
    plLog::Error("PlasmaAudio: the streaming wake semaphore could not be created; streamed clips are not available.");
    return;
  }

  // the lists only hold pointers; reserved up front, so acquiring, releasing and retiring streams never grows them
  {
    PL_LOCK(m_Mutex);
    const plUInt32 uiCapacity = GetMaxStreams() + 4;
    m_Streams.Reserve(uiCapacity);
    m_RetiredStreams.Reserve(uiCapacity);
    m_FreeStreams.Reserve(uiCapacity);
  }

  m_pThread = PL_DEFAULT_NEW(plAudioStreamThread, this);
  m_pThread->Start();
}

void plAudioStreamService::Shutdown()
{
  if (m_pThread != nullptr)
  {
    m_bStopThread = true;
    m_WakeSemaphore.ReturnToken();
    m_pThread->Join();
    m_pThread = nullptr;
  }

  plDynamicArray<plAudioStream*> streams;
  plDynamicArray<plAudioStream*> freeStreams;
  {
    PL_LOCK(m_Mutex);
    streams = m_Streams;
    m_Streams.Clear();
    m_Streams.Compact();
    freeStreams = m_FreeStreams;
    freeStreams.PushBackRange(m_RetiredStreams);
    m_FreeStreams.Clear();
    m_FreeStreams.Compact();
    m_RetiredStreams.Clear();
    m_RetiredStreams.Compact();
  }

  if (!streams.IsEmpty())
  {
    plLog::Warning("PlasmaAudio: {} audio streams were still open at shutdown", streams.GetCount());
  }

  for (plAudioStream* pStream : streams)
  {
    PL_DEFAULT_DELETE(pStream);
  }

  for (plAudioStream* pStream : freeStreams)
  {
    PL_DEFAULT_DELETE(pStream);
  }
}

plAudioStream* plAudioStreamService::AcquireStream(const plAudioClipResource& clip)
{
  if (m_pThread == nullptr || clip.GetLoadMode() != plAudioClipLoadMode::Streamed)
    return nullptr;

  if (GetActiveStreamCount() >= GetMaxStreams())
  {
    plLog::Warning("PlasmaAudio: cannot stream '{}', {} streams are open (PlasmaAudio.MaxStreams)", clip.GetResourceIdOrDescription(), GetMaxStreams());
    return nullptr;
  }

  plAudioStream* pStream = nullptr;
  {
    PL_LOCK(m_Mutex);
    if (!m_FreeStreams.IsEmpty())
    {
      pStream = m_FreeStreams.PeekBack();
      m_FreeStreams.PopBack();
    }
  }

  if (pStream == nullptr)
  {
    pStream = PL_DEFAULT_NEW(plAudioStream);
  }

  // not visible to the IO thread until it is added to m_Streams below
  pStream->m_pService = this;
  // assigned in place, so a reused stream keeps the string memory
  if (m_bForceFileSystemReads)
    pStream->m_sOsFilePath.Clear();
  else
    pStream->m_sOsFilePath = clip.GetOsFilePath();
  pStream->m_sResourceId = clip.GetResourceID();
  pStream->m_uiPayloadFileOffset = clip.GetPayloadFileOffset();
  pStream->m_uiPayloadBytes = clip.GetPayloadBytes();

  const plArrayPtr<const plUInt8> priming = clip.GetPriming();
  pStream->m_Priming.m_pData = priming.GetPtr();
  pStream->m_Priming.m_uiBegin = 0;
  pStream->m_Priming.m_uiEnd = plMath::Min<plUInt64>(priming.GetCount(), pStream->m_uiPayloadBytes);

  const plArrayPtr<const plUInt8> loopPriming = clip.GetLoopPriming();
  if (!loopPriming.IsEmpty())
  {
    pStream->m_LoopPriming.m_pData = loopPriming.GetPtr();
    pStream->m_LoopPriming.m_uiBegin = clip.GetLoopPrimingOffset();
    pStream->m_LoopPriming.m_uiEnd = plMath::Min<plUInt64>(clip.GetLoopPrimingOffset() + loopPriming.GetCount(), pStream->m_uiPayloadBytes);
  }

  // a reused stream keeps its ring memory unless the chunk size grew
  pStream->m_uiChunkBytes = GetChunkBytes();
  pStream->m_Ring.SetCountUninitialized(pStream->m_uiChunkBytes * s_uiRingChunks);
  pStream->m_uiRingMask = pStream->m_Ring.GetCount() - 1;

  // the IO thread starts filling right after the start priming data
  const plUInt64 uiStart = pStream->m_Priming.m_uiEnd;
  pStream->m_uiRingOrigin = uiStart;
  pStream->m_iSeekTarget = static_cast<plInt64>(uiStart);
  pStream->m_iReadPos = static_cast<plInt64>(uiStart);
  pStream->m_iWritePos = static_cast<plInt64>(uiStart);
  pStream->m_iReadyGeneration = 0;
  pStream->m_iSeekGeneration = 1;

  {
    PL_LOCK(m_Mutex);
    m_Streams.PushBack(pStream);
  }

  WakeIo();
  return pStream;
}

void plAudioStreamService::ReleaseStream(plAudioStream* pStream)
{
  if (pStream == nullptr)
    return;

  PL_LOCK(m_Mutex);

  if (!m_Streams.RemoveAndSwap(pStream))
  {
    PL_REPORT_FAILURE("Audio stream released twice or not owned by this service");
    return;
  }

  // The IO thread may be reading for exactly this stream right now: then it is retired, and the IO thread resets and
  // pools it at the start of its next pass. Otherwise the IO thread no longer sees it and it is pooled right away.
  if (pStream == m_pIoStream)
  {
    m_RetiredStreams.PushBack(pStream);
    WakeIo();
    return;
  }

  pStream->ResetForReuse();
  m_FreeStreams.PushBack(pStream);
}

void plAudioStreamService::ReserveStreams(plUInt32 uiCount)
{
  const plUInt32 uiChunkBytes = GetChunkBytes();

  while (GetPooledStreamCount() < uiCount)
  {
    plAudioStream* pStream = PL_DEFAULT_NEW(plAudioStream);
    pStream->m_uiChunkBytes = uiChunkBytes;
    pStream->m_Ring.SetCountUninitialized(uiChunkBytes * s_uiRingChunks);

    PL_LOCK(m_Mutex);
    m_FreeStreams.PushBack(pStream);
  }
}

plUInt32 plAudioStreamService::GetActiveStreamCount() const
{
  PL_LOCK(m_Mutex);
  return m_Streams.GetCount();
}

plUInt32 plAudioStreamService::GetActiveStreamCount(plStringView sClipResourceId) const
{
  PL_LOCK(m_Mutex);

  plUInt32 uiCount = 0;
  for (const plAudioStream* pStream : m_Streams)
  {
    if (pStream->m_sResourceId == sClipResourceId)
      ++uiCount;
  }
  return uiCount;
}

plUInt32 plAudioStreamService::GetPooledStreamCount() const
{
  PL_LOCK(m_Mutex);
  return m_FreeStreams.GetCount();
}

plUInt64 plAudioStreamService::GetRingBytesInUse() const
{
  PL_LOCK(m_Mutex);

  plUInt64 uiBytes = 0;
  for (const plAudioStream* pStream : m_Streams)
  {
    uiBytes += pStream->m_Ring.GetHeapMemoryUsage();
  }
  for (const plAudioStream* pStream : m_FreeStreams)
  {
    uiBytes += pStream->m_Ring.GetHeapMemoryUsage();
  }
  for (const plAudioStream* pStream : m_RetiredStreams)
  {
    uiBytes += pStream->m_Ring.GetHeapMemoryUsage();
  }
  return uiBytes;
}

plUInt32 plAudioStreamService::GetMaxStreams() const
{
  return static_cast<plUInt32>(plMath::Max(cvar_PlasmaAudioMaxStreams.GetValue(), 0));
}

void plAudioStreamService::SetIoPaused(bool bPaused)
{
  m_bIoPaused = bPaused;

  if (!bPaused)
  {
    WakeIo();
  }
}

bool plAudioStreamService::WaitForIdle(plTime timeout)
{
  const plTime tEnd = plTime::Now() + timeout;

  while (true)
  {
    bool bIdle = true;
    {
      PL_LOCK(m_Mutex);
      for (const plAudioStream* pStream : m_Streams)
      {
        if (!pStream->IsFilled())
        {
          bIdle = false;
          break;
        }
      }
    }

    if (bIdle)
      return true;

    if (plTime::Now() >= tEnd)
      return false;

    WakeIo();
    plThreadUtils::Sleep(plTime::MakeFromMilliseconds(1));
  }
}

void plAudioStreamService::WakeIo()
{
  // a semaphore token: no lock is taken, so the device thread can call this
  if (!m_bWakePending.Set(true) && m_pThread != nullptr)
  {
    m_WakeSemaphore.ReturnToken();
  }
}

bool plAudioStreamService::RunIoPass()
{
  // retired streams are not read for by this thread right now, so they can be reset and pooled
  {
    PL_LOCK(m_Mutex);

    for (plAudioStream* pStream : m_RetiredStreams)
    {
      pStream->ResetForReuse();
      m_FreeStreams.PushBack(pStream);
    }
    m_RetiredStreams.Clear();
  }

  if (m_bIoPaused)
    return false;

  PL_PROFILE_SCOPE("PlasmaAudio.StreamIo");

  bool bWork = false;

  // the step of each stream (one seek or one chunk read) runs without the mutex; a stream released meanwhile is
  // retired, not reset, until the next pass
  for (plUInt32 i = 0;; ++i)
  {
    plAudioStream* pStream = nullptr;
    {
      PL_LOCK(m_Mutex);

      if (i >= m_Streams.GetCount())
        break;

      pStream = m_Streams[i];
      m_pIoStream = pStream;
    }

    bWork |= pStream->UpdateIo();

    PL_LOCK(m_Mutex);
    m_pIoStream = nullptr;
  }

  return bWork;
}

PL_STATICLINK_FILE(PlasmaAudioPlugin, PlasmaAudioPlugin_Streaming_AudioStreamService);
