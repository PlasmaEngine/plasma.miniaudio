#pragma once

#include <PlasmaAudioPlugin/PlasmaAudioPluginDLL.h>

#include <Foundation/Types/ArrayPtr.h>

/// \brief Random access to the encoded payload of a clip, read from the audio thread.
///
/// Implementations must not block: IsAvailable() reports whether a range can be read right now, and a decoder
/// that finds its data missing outputs silence instead of waiting. The resident payload of a clip is served by
/// plMemoryByteSource, a streamed payload by the ring buffer of the stream service; the decoders do not know
/// the difference.
class PL_PLASMAAUDIOPLUGIN_DLL plAudioByteSource
{
public:
  virtual ~plAudioByteSource() = default;

  /// \brief Copies up to uiBytes bytes starting at uiOffset (relative to the payload start). Returns the number of bytes copied.
  virtual plUInt32 Read(plUInt64 uiOffset, void* pOut, plUInt32 uiBytes) = 0;

  /// \brief Whether the whole range [uiOffset, uiOffset + uiBytes) can be read without waiting.
  virtual bool IsAvailable(plUInt64 uiOffset, plUInt32 uiBytes) = 0;
};

/// \brief A plAudioByteSource over a payload that is completely in memory.
class PL_PLASMAAUDIOPLUGIN_DLL plMemoryByteSource final : public plAudioByteSource
{
public:
  plMemoryByteSource() = default;
  explicit plMemoryByteSource(plArrayPtr<const plUInt8> data)
    : m_Data(data)
  {
  }

  void SetData(plArrayPtr<const plUInt8> data) { m_Data = data; }
  plArrayPtr<const plUInt8> GetData() const { return m_Data; }

  virtual plUInt32 Read(plUInt64 uiOffset, void* pOut, plUInt32 uiBytes) override
  {
    if (uiOffset >= m_Data.GetCount())
      return 0;

    const plUInt32 uiCount = static_cast<plUInt32>(plMath::Min<plUInt64>(uiBytes, m_Data.GetCount() - uiOffset));
    plMemoryUtils::Copy(static_cast<plUInt8*>(pOut), m_Data.GetPtr() + uiOffset, uiCount);
    return uiCount;
  }

  virtual bool IsAvailable(plUInt64 uiOffset, plUInt32 uiBytes) override
  {
    return uiOffset + uiBytes <= m_Data.GetCount();
  }

private:
  plArrayPtr<const plUInt8> m_Data;
};
