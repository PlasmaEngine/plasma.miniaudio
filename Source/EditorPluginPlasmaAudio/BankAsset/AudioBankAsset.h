#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>

/// \brief The events and clips an audio bank keeps loaded, as asset references.
class plAudioBankAssetProperties : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankAssetProperties, plReflectedClass);

public:
  plDynamicArray<plString> m_Events;
  plDynamicArray<plString> m_Clips;
};

class plAudioBankAssetDocument : public plSimpleAssetDocument<plAudioBankAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plAudioBankAssetDocument, plSimpleAssetDocument<plAudioBankAssetProperties>);

public:
  plAudioBankAssetDocument(plStringView sDocumentPath);

protected:
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile,
    const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
};
