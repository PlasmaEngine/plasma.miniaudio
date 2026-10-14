#include <EditorPluginPlasmaAudio/EditorPluginPlasmaAudioPCH.h>

#include <Core/Configuration/PlatformProfile.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Assets/AssetStatusIndicator.moc.h>
#include <EditorPluginPlasmaAudio/ClipAsset/AudioClipAssetWindow.moc.h>
#include <EditorPluginPlasmaAudio/Preview/AudioPreviewService.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Math/ColorScheme.h>
#include <Foundation/Threading/DelegateTask.h>
#include <GuiFoundation/ActionViews/MenuBarActionMapView.moc.h>
#include <GuiFoundation/ActionViews/ToolBarActionMapView.moc.h>
#include <GuiFoundation/DockPanels/DocumentPanel.moc.h>
#include <PlasmaAudioPlugin/Codecs/ImaAdpcm.h>
#include <PlasmaAudioPlugin/Codecs/OpusCodec.h>
#include <ToolsFoundation/Object/ObjectCommandAccessor.h>

#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>

namespace
{
  constexpr int kRulerHeight = 18;
  constexpr int kMarkerGrab = 6;

  /// Waveform bars: width and distance from one bar to the next, in pixels.
  constexpr int kBarWidth = 3;
  constexpr int kBarPitch = 5;
  constexpr plUInt32 kPreviewCacheSize = 8;

  /// Audition ring buffer of a streamed clip: four disk chunks.
  constexpr plUInt64 kStreamRingBytes = 4 * plAudioClipConstants::StreamChunkBytes;

  QColor AccentColor(const plDocument* pDocument)
  {
    plStringView sCategory = "Audio";
    if (pDocument != nullptr && pDocument->GetDocumentTypeDescriptor() != nullptr && !pDocument->GetDocumentTypeDescriptor()->m_sAssetCategory.IsEmpty())
    {
      sCategory = pDocument->GetDocumentTypeDescriptor()->m_sAssetCategory;
    }
    return plToQtColor(plColorGammaUB(plColorScheme::GetCategoryColor(sCategory, plColorScheme::CategoryColorUsage::BorderColor)));
  }

  // Theme tokens as in the other audio tools: chrome #1e1e1e, panels #202020, headers #191919, controls #333333,
  // borders #3c3c3c, text #cccccc, dim #8e8e8e.
  QString ClipToolStyle(const QColor& accent)
  {
    return QString(
      "QWidget#ClipButtonRow { background: #202020; border-bottom: 1px solid #303030; }"
      "QWidget#ClipButtonRow QToolButton { background: #333333; border: 1px solid #3c3c3c; border-radius: 3px; padding: 3px 10px; color: #cccccc; }"
      "QWidget#ClipButtonRow QToolButton:hover { border-color: %1; color: #ffffff; }"
      "QWidget#ClipButtonRow QToolButton:checked { background: #2a2a2a; border-color: %1; color: #ffffff; }"
      "QWidget#ClipButtonRow QToolButton#PlayButton { color: #9fe8c9; }"
      "QWidget#ClipButtonRow QLabel { color: #8e8e8e; }"
      "QLabel#ClipInfo { color: #cccccc; }"
      "QWidget#ClipTabRow { background: #191919; border-bottom: 1px solid #303030; }"
      "QWidget#ClipTabRow QToolButton { background: transparent; border: 1px dashed #3c3c3c; border-radius: 3px; padding: 1px 8px; color: %1; }"
      "QWidget#ClipTabRow QToolButton:hover { border-color: %1; color: #ffffff; }"
      "QWidget#ClipTabRow QToolButton::menu-indicator { image: none; }"
      "QWidget#ClipStatLine { background: #191919; border-top: 1px solid #303030; }"
      "QWidget#ClipStatLine QLabel { color: #8e8e8e; }")
      .arg(accent.name());
  }

  QString FormatBytes(plUInt64 uiBytes)
  {
    if (uiBytes >= 1024 * 1024)
      return QString::asprintf("%.2f MB", uiBytes / (1024.0 * 1024.0));
    if (uiBytes >= 1024)
      return QString::asprintf("%.1f KB", uiBytes / 1024.0);
    return QString::asprintf("%u B", static_cast<plUInt32>(uiBytes));
  }

  const char* CodecName(plAudioClipCodec::Enum eCodec)
  {
    switch (eCodec)
    {
      case plAudioClipCodec::Pcm16:
        return "PCM";
      case plAudioClipCodec::ImaAdpcm:
        return "ADPCM";
      case plAudioClipCodec::Opus:
        return "Opus";
    }
    return "?";
  }

  const char* LoadModeName(plAudioClipLoadMode::Enum eMode)
  {
    switch (eMode)
    {
      case plAudioClipLoadMode::DecompressOnLoad:
        return "Decompress on load";
      case plAudioClipLoadMode::CompressedInMemory:
        return "Compressed in memory";
      case plAudioClipLoadMode::Streamed:
        return "Streamed";
    }
    return "?";
  }

  plUInt64 HashSettings(const plAudioClipBuildSettings& s, plStringView sPath, plInt64 iModificationTime)
  {
    plUInt64 uiHash = plHashingUtils::xxHash64String(sPath);
    auto Add = [&](const auto& value)
    { uiHash = plHashingUtils::xxHash64(&value, sizeof(value), uiHash); };

    Add(iModificationTime);
    Add(static_cast<plUInt8>(s.m_eCodec));
    Add(static_cast<plUInt8>(s.m_eLoadMode));
    Add(s.m_uiOpusBitrateKbps);
    Add(s.m_uiTargetSampleRate);
    Add(static_cast<plUInt8>(s.m_bForceMono ? 1 : 0));
    Add(s.m_fNormaliseLufs);
    Add(s.m_fPeakFallbackDb);
    Add(s.m_fTrimSilenceDb);
    Add(s.m_iLoopStart);
    Add(s.m_iLoopEnd);
    return uiHash;
  }
} // namespace

//////////////////////////////////////////////////////////////////////////
// plAudioClipPreviewJob
//////////////////////////////////////////////////////////////////////////

void plAudioClipPreviewJob::Run()
{
  plAudioClipPreview& res = *m_pResult;

  plAudioClipSourcePcm source;
  if (plAudioClipBuilder::DecodeSourceFile(m_sAbsSourceFile, source).Failed())
  {
    res.m_sError = "Could not decode the source file.";
    m_bDone = true;
    return;
  }

  // the builder trims leading silence; measure it on the source so loop markers can be mapped back to source time
  if (plMath::IsFinite(m_Settings.m_fTrimSilenceDb))
  {
    const float fThreshold = plMath::Pow(10.0f, m_Settings.m_fTrimSilenceDb / 20.0f);
    const plUInt32 uiChannels = source.m_uiChannels;
    const plUInt64 uiFrames = source.GetFrames();
    const bool bMono = m_Settings.m_bForceMono && uiChannels > 1;

    plUInt64 uiFirst = 0;
    for (; uiFirst < uiFrames; ++uiFirst)
    {
      const float* pFrame = source.m_Samples.GetData() + uiFirst * uiChannels;
      bool bLoud = false;

      if (bMono)
      {
        float fSum = 0.0f;
        for (plUInt32 c = 0; c < uiChannels; ++c)
          fSum += pFrame[c];
        bLoud = plMath::Abs(fSum / uiChannels) > fThreshold;
      }
      else
      {
        for (plUInt32 c = 0; c < uiChannels && !bLoud; ++c)
          bLoud = plMath::Abs(pFrame[c]) > fThreshold;
      }

      if (bLoud)
        break;
    }

    // never trimmed into the loop
    const plInt64 iLoopStart = m_Settings.m_iLoopStart >= 0 ? m_Settings.m_iLoopStart : (source.m_bHasLoop ? static_cast<plInt64>(source.m_uiLoopStart) : -1);
    if (iLoopStart >= 0)
      uiFirst = plMath::Min(uiFirst, static_cast<plUInt64>(iLoopStart));

    if (uiFirst < uiFrames)
      res.m_fTrimOffset = static_cast<double>(uiFirst) / source.m_uiSampleRate;
  }

  plDefaultMemoryStreamStorage storage;
  {
    plMemoryStreamWriter writer(&storage);
    if (plAudioClipBuilder::Build(source, m_Settings, writer).Failed())
    {
      res.m_sError = "Building the clip failed, see the log for details.";
      m_bDone = true;
      return;
    }
  }

  res.m_uiFileBytes = storage.GetStorageSize64();

  plMemoryStreamReader reader(&storage);
  if (res.m_Header.Read(reader).Failed() || res.m_Header.m_Languages.IsEmpty() || res.m_Header.m_uiChannels == 0)
  {
    res.m_sError = "The built clip could not be read back.";
    m_bDone = true;
    return;
  }

  const plAudioClipHeader& header = res.m_Header;
  const plAudioClipLanguageEntry& lang = header.m_Languages[0];
  reader.SkipBytes(static_cast<plUInt64>(lang.m_uiPrimingBytes) + lang.m_uiLoopPrimingBytes);

  plDynamicArray<plUInt8> payload;
  payload.SetCountUninitialized(static_cast<plUInt32>(lang.m_uiPayloadBytes));
  if (reader.ReadBytes(payload.GetData(), payload.GetCount()) != payload.GetCount())
  {
    res.m_sError = "The built clip is truncated.";
    m_bDone = true;
    return;
  }

  // decode the payload again, so auditioning plays what the runtime plays
  const plUInt8 uiChannels = header.m_uiChannels;
  const plUInt64 uiFrames = header.m_uiFrames;
  res.m_Pcm.SetCount(static_cast<plUInt32>(uiFrames * uiChannels));

  if (header.m_eCodec == plAudioClipCodec::Pcm16)
  {
    plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(res.m_Pcm.GetData()), payload.GetData(), plMath::Min<size_t>(payload.GetCount(), res.m_Pcm.GetCount() * sizeof(plInt16)));
  }
  else if (header.m_eCodec == plAudioClipCodec::ImaAdpcm)
  {
    const plUInt32 uiBlockBytes = plImaAdpcmCodec::GetBlockBytes(uiChannels);
    const plUInt64 uiBlocks = plImaAdpcmCodec::GetBlockCount(uiFrames);

    plDynamicArray<plInt16> block;
    block.SetCountUninitialized(plImaAdpcmCodec::FramesPerBlock * uiChannels);

    for (plUInt64 b = 0; b < uiBlocks && (b + 1) * uiBlockBytes <= payload.GetCount(); ++b)
    {
      plImaAdpcmCodec::DecodeBlock(payload.GetData() + b * uiBlockBytes, uiChannels, block.GetData());

      const plUInt64 uiFirst = b * plImaAdpcmCodec::FramesPerBlock;
      const plUInt64 uiCount = plMath::Min<plUInt64>(plImaAdpcmCodec::FramesPerBlock, uiFrames - uiFirst);
      plMemoryUtils::Copy(res.m_Pcm.GetData() + uiFirst * uiChannels, block.GetData(), static_cast<size_t>(uiCount * uiChannels));
    }
  }
  else if (header.m_eCodec == plAudioClipCodec::Opus)
  {
    if (plOpusCodec::Decode(payload, uiChannels, header.m_uiPreSkip, uiFrames, res.m_Pcm).Failed())
    {
      res.m_sError = "The Opus payload of the built clip could not be decoded.";
      m_bDone = true;
      return;
    }
  }

  res.m_bSuccess = true;
  m_bDone = true;
}

//////////////////////////////////////////////////////////////////////////
// plQtAudioWaveformWidget
//////////////////////////////////////////////////////////////////////////

plQtAudioWaveformWidget::plQtAudioWaveformWidget(QWidget* pParent, plDocument* pDocument, const QColor& accent)
  : QWidget(pParent)
  , m_pDocument(pDocument)
  , m_Accent(accent)
{
  setMouseTracking(true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void plQtAudioWaveformWidget::SetPreview(const plSharedPtr<plAudioClipPreview>& pPreview)
{
  m_pPreview = pPreview;
  m_Drag = Drag::None;
  update();
}

void plQtAudioWaveformWidget::UpdateBars(int iPlotWidth)
{
  if (m_pPreview == nullptr || !m_pPreview->m_bSuccess)
  {
    m_Bars.Clear();
    m_uiBarCount = 0;
    m_pBarsPreview = nullptr;
    return;
  }

  // keyed on the pointer and the build hash, so a new preview allocated at a freed one's address is not mistaken for it
  if (m_pBarsPreview == m_pPreview.Borrow() && m_uiBarsHash == m_pPreview->m_uiHash && m_iBarsWidth == iPlotWidth)
    return;

  m_pBarsPreview = m_pPreview.Borrow();
  m_uiBarsHash = m_pPreview->m_uiHash;
  m_iBarsWidth = iPlotWidth;

  const plAudioClipHeader& header = m_pPreview->m_Header;
  const plUInt32 uiChannels = header.m_uiChannels;
  const plUInt64 uiFrames = header.m_uiFrames;

  m_uiBarCount = (iPlotWidth > 0) ? static_cast<plUInt32>((iPlotWidth + kBarPitch - kBarWidth) / kBarPitch) : 0;
  m_Bars.SetCount(m_uiBarCount * uiChannels);

  if (m_uiBarCount == 0 || uiChannels == 0 || uiFrames == 0)
    return;

  const plDynamicArray<plInt16>& pcm = m_pPreview->m_Pcm;
  const bool bHasPcm = pcm.GetCount() >= uiFrames * uiChannels;
  const plUInt32 uiBuckets = header.m_uiPeakBuckets;

  for (plUInt32 b = 0; b < m_uiBarCount; ++b)
  {
    for (plUInt32 c = 0; c < uiChannels; ++c)
    {
      float fPeak = 0.0f;

      if (bHasPcm)
      {
        const plUInt64 uiBegin = uiFrames * b / m_uiBarCount;
        const plUInt64 uiEnd = plMath::Max(uiFrames * (b + 1) / m_uiBarCount, uiBegin + 1);

        plInt32 iPeak = 0;
        for (plUInt64 f = uiBegin; f < uiEnd && f < uiFrames; ++f)
        {
          iPeak = plMath::Max(iPeak, plMath::Abs(static_cast<plInt32>(pcm[static_cast<plUInt32>(f * uiChannels + c)])));
        }
        fPeak = iPeak / 32768.0f;
      }
      else if (uiBuckets > 0 && header.m_Peaks.GetCount() >= uiBuckets * uiChannels * 2)
      {
        // no decoded audio: fall back to the stored min/max peaks
        const plUInt32 uiB0 = static_cast<plUInt32>(static_cast<plUInt64>(uiBuckets) * b / m_uiBarCount);
        const plUInt32 uiB1 = plMath::Max(uiB0 + 1, static_cast<plUInt32>(static_cast<plUInt64>(uiBuckets) * (b + 1) / m_uiBarCount));

        plInt32 iPeak = 0;
        for (plUInt32 k = uiB0; k < uiB1 && k < uiBuckets; ++k)
        {
          iPeak = plMath::Max(iPeak, plMath::Abs(static_cast<plInt32>(header.m_Peaks[(k * uiChannels + c) * 2 + 0])));
          iPeak = plMath::Max(iPeak, plMath::Abs(static_cast<plInt32>(header.m_Peaks[(k * uiChannels + c) * 2 + 1])));
        }
        fPeak = iPeak / 127.0f;
      }

      m_Bars[c * m_uiBarCount + b] = plMath::Clamp(fPeak, 0.0f, 1.0f);
    }
  }
}

void plQtAudioWaveformWidget::SetMessage(const QString& sMessage)
{
  if (m_sMessage == sMessage)
    return;

  m_sMessage = sMessage;
  update();
}

void plQtAudioWaveformWidget::SetPlayCursor(plInt64 iFrame)
{
  if (m_iPlayCursor == iFrame)
    return;

  m_iPlayCursor = iFrame;
  update();
}

QRect plQtAudioWaveformWidget::GetPlotRect() const
{
  return rect().adjusted(8, kRulerHeight, -8, -6);
}

double plQtAudioWaveformWidget::FrameToX(plUInt64 uiFrame) const
{
  const QRect plot = GetPlotRect();
  const plUInt64 uiFrames = m_pPreview != nullptr ? m_pPreview->m_Header.m_uiFrames : 0;
  if (uiFrames == 0)
    return plot.left();

  return plot.left() + static_cast<double>(uiFrame) / uiFrames * plot.width();
}

plUInt64 plQtAudioWaveformWidget::XToFrame(int x) const
{
  const QRect plot = GetPlotRect();
  const plUInt64 uiFrames = m_pPreview != nullptr ? m_pPreview->m_Header.m_uiFrames : 0;
  if (uiFrames == 0 || plot.width() <= 0)
    return 0;

  const double fRel = plMath::Clamp(static_cast<double>(x - plot.left()) / plot.width(), 0.0, 1.0);
  return static_cast<plUInt64>(fRel * uiFrames + 0.5);
}

bool plQtAudioWaveformWidget::GetLoop(plUInt64& out_uiStart, plUInt64& out_uiEnd) const
{
  if (m_Drag != Drag::None)
  {
    out_uiStart = m_uiDragStart;
    out_uiEnd = m_uiDragEnd;
    return out_uiEnd > out_uiStart;
  }

  if (m_pPreview == nullptr || !m_pPreview->m_bSuccess || !m_pPreview->m_Header.m_bHasLoop)
    return false;

  out_uiStart = m_pPreview->m_Header.m_uiLoopStart;
  out_uiEnd = m_pPreview->m_Header.m_uiLoopEnd;
  return true;
}

void plQtAudioWaveformWidget::paintEvent(QPaintEvent* pEvent)
{
  PL_IGNORE_UNUSED(pEvent);

  QPainter p(this);
  p.fillRect(rect(), QColor(0x1a, 0x1a, 0x1a));

  const QRect plot = GetPlotRect();

  if (m_pPreview == nullptr || !m_pPreview->m_bSuccess)
  {
    p.setPen(QColor(0x8e, 0x8e, 0x8e));
    const QString sText = (m_pPreview != nullptr && !m_pPreview->m_sError.IsEmpty()) ? QString::fromUtf8(m_pPreview->m_sError.GetData()) : m_sMessage;
    p.drawText(rect(), Qt::AlignCenter, sText);
    return;
  }

  const plAudioClipHeader& header = m_pPreview->m_Header;
  const plUInt32 uiChannels = header.m_uiChannels;
  const double fDuration = header.m_uiSampleRate > 0 ? static_cast<double>(header.m_uiFrames) / header.m_uiSampleRate : 0.0;

  // ruler
  {
    p.fillRect(QRect(0, 0, width(), kRulerHeight - 2), QColor(0x19, 0x19, 0x19));

    static const double steps[] = {0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0};
    double fStep = steps[PL_ARRAY_SIZE(steps) - 1];
    for (double s : steps)
    {
      if (fDuration > 0.0 && s / fDuration * plot.width() >= 70.0)
      {
        fStep = s;
        break;
      }
    }

    QFont font = p.font();
    font.setPointSizeF(7.5);
    p.setFont(font);

    for (double t = 0.0; fDuration > 0.0 && t <= fDuration + 1e-9; t += fStep)
    {
      const int x = plot.left() + static_cast<int>(t / fDuration * plot.width());
      p.setPen(QColor(0x3c, 0x3c, 0x3c));
      p.drawLine(x, kRulerHeight - 6, x, kRulerHeight - 2);
      p.setPen(QColor(0x8e, 0x8e, 0x8e));
      p.drawText(x + 3, kRulerHeight - 6, fStep < 1.0 ? QString::asprintf("%.2f s", t) : QString::asprintf("%.0f s", t));
    }
  }

  // loop region
  plUInt64 uiLoopStart = 0;
  plUInt64 uiLoopEnd = 0;
  const bool bHasLoop = GetLoop(uiLoopStart, uiLoopEnd);

  if (bHasLoop)
  {
    const int x0 = static_cast<int>(FrameToX(uiLoopStart));
    const int x1 = static_cast<int>(FrameToX(uiLoopEnd));
    QColor region = m_Accent;
    region.setAlpha(40);
    p.fillRect(QRect(x0, plot.top(), plMath::Max(1, x1 - x0), plot.height()), region);
  }

  // bar waveform, one lane per channel: evenly spaced bars mirrored around the centre line, each as tall as the peak
  // level of its slice of the clip
  UpdateBars(plot.width());

  if (uiChannels > 0 && m_uiBarCount > 0 && m_Bars.GetCount() >= m_uiBarCount * uiChannels)
  {
    const int iLaneHeight = plot.height() / static_cast<int>(uiChannels);
    const QColor wave = m_Accent.lighter(130);

    for (plUInt32 c = 0; c < uiChannels; ++c)
    {
      const int iTop = plot.top() + static_cast<int>(c) * iLaneHeight;
      const double fMid = iTop + iLaneHeight * 0.5;
      const double fHalf = iLaneHeight * 0.46;

      for (plUInt32 b = 0; b < m_uiBarCount; ++b)
      {
        const float fLevel = m_Bars[c * m_uiBarCount + b];
        const double fBarHalf = plMath::Max(0.5, fLevel * fHalf);
        const int x = plot.left() + static_cast<int>(b) * kBarPitch;

        p.fillRect(QRectF(x, fMid - fBarHalf, kBarWidth, fBarHalf * 2.0), wave);
      }
    }
  }

  // loop markers
  if (bHasLoop)
  {
    for (plUInt64 uiFrame : {uiLoopStart, uiLoopEnd})
    {
      const int x = static_cast<int>(FrameToX(uiFrame));
      p.setPen(QPen(m_Accent, 2));
      p.drawLine(x, plot.top(), x, plot.bottom());

      const QRect flag = uiFrame == uiLoopStart ? QRect(x, plot.top(), 8, 10) : QRect(x - 8, plot.top(), 8, 10);
      p.fillRect(flag, m_Accent);
    }
  }

  // play cursor
  if (m_iPlayCursor >= 0 && header.m_uiFrames > 0)
  {
    const int x = static_cast<int>(FrameToX(static_cast<plUInt64>(m_iPlayCursor) % header.m_uiFrames));
    p.setPen(QPen(QColor(0xff, 0xff, 0xff), 1));
    p.drawLine(x, plot.top(), x, plot.bottom());
  }
}

void plQtAudioWaveformWidget::mousePressEvent(QMouseEvent* pEvent)
{
  if (pEvent->button() != Qt::LeftButton || m_pPreview == nullptr || !m_pPreview->m_bSuccess || m_pPreview->m_Header.m_uiFrames == 0)
  {
    QWidget::mousePressEvent(pEvent);
    return;
  }

  const int x = pEvent->pos().x();

  plUInt64 uiStart = 0;
  plUInt64 uiEnd = 0;
  if (GetLoop(uiStart, uiEnd))
  {
    if (plMath::Abs(x - static_cast<int>(FrameToX(uiStart))) <= kMarkerGrab)
    {
      m_Drag = Drag::LoopStart;
      m_uiDragStart = uiStart;
      m_uiDragEnd = uiEnd;
      return;
    }

    if (plMath::Abs(x - static_cast<int>(FrameToX(uiEnd))) <= kMarkerGrab)
    {
      m_Drag = Drag::LoopEnd;
      m_uiDragStart = uiStart;
      m_uiDragEnd = uiEnd;
      return;
    }
  }

  m_Drag = Drag::Create;
  m_uiDragAnchor = XToFrame(x);
  m_uiDragStart = m_uiDragAnchor;
  m_uiDragEnd = m_uiDragAnchor;
  update();
}

void plQtAudioWaveformWidget::mouseMoveEvent(QMouseEvent* pEvent)
{
  if (m_Drag == Drag::None)
  {
    plUInt64 uiStart = 0;
    plUInt64 uiEnd = 0;
    const int x = pEvent->pos().x();
    const bool bOverMarker = GetLoop(uiStart, uiEnd) && (plMath::Abs(x - static_cast<int>(FrameToX(uiStart))) <= kMarkerGrab || plMath::Abs(x - static_cast<int>(FrameToX(uiEnd))) <= kMarkerGrab);
    setCursor(bOverMarker ? Qt::SizeHorCursor : Qt::IBeamCursor);
    return;
  }

  const plUInt64 uiFrame = XToFrame(pEvent->pos().x());

  switch (m_Drag)
  {
    case Drag::LoopStart:
      m_uiDragStart = plMath::Min(uiFrame, m_uiDragEnd > 0 ? m_uiDragEnd - 1 : 0);
      break;
    case Drag::LoopEnd:
      m_uiDragEnd = plMath::Max(uiFrame, m_uiDragStart + 1);
      break;
    case Drag::Create:
      m_uiDragStart = plMath::Min(uiFrame, m_uiDragAnchor);
      m_uiDragEnd = plMath::Max(uiFrame, m_uiDragAnchor);
      break;
    default:
      break;
  }

  update();
}

void plQtAudioWaveformWidget::mouseReleaseEvent(QMouseEvent* pEvent)
{
  if (pEvent->button() != Qt::LeftButton || m_Drag == Drag::None)
  {
    QWidget::mouseReleaseEvent(pEvent);
    return;
  }

  const Drag drag = m_Drag;
  m_Drag = Drag::None;

  // a click without dragging does not create a loop
  if (drag == Drag::Create && FrameToX(m_uiDragEnd) - FrameToX(m_uiDragStart) < 3.0)
  {
    update();
    return;
  }

  const plAudioClipHeader& header = m_pPreview->m_Header;
  if (header.m_uiSampleRate == 0 || m_uiDragEnd <= m_uiDragStart)
  {
    update();
    return;
  }

  // loop points are stored in source seconds
  const double fStart = m_pPreview->m_fTrimOffset + static_cast<double>(m_uiDragStart) / header.m_uiSampleRate;
  const double fEnd = m_pPreview->m_fTrimOffset + static_cast<double>(m_uiDragEnd) / header.m_uiSampleRate;

  const auto& children = m_pDocument->GetObjectManager()->GetRootObject()->GetChildren();
  if (!children.IsEmpty())
  {
    plObjectCommandAccessor accessor(m_pDocument->GetCommandHistory());
    accessor.StartTransaction("Set Loop Points");
    accessor.SetValueByName(children[0], "OverrideLoop", true).IgnoreResult();
    accessor.SetValueByName(children[0], "LoopStart", static_cast<float>(fStart)).IgnoreResult();
    accessor.SetValueByName(children[0], "LoopEnd", static_cast<float>(fEnd)).IgnoreResult();
    accessor.FinishTransaction();
  }

  update();
}

//////////////////////////////////////////////////////////////////////////
// plAudioClipAssetDocumentWindow
//////////////////////////////////////////////////////////////////////////

plAudioClipAssetDocumentWindow::plAudioClipAssetDocumentWindow(plDocument* pDocument)
  : plQtDocumentWindow(pDocument)
{
  m_pAssetDoc = static_cast<plAudioClipAssetDocument*>(pDocument);
  m_Accent = AccentColor(pDocument);

  // Menu Bar
  {
    plQtMenuBarActionMapView* pMenuBar = static_cast<plQtMenuBarActionMapView*>(menuBar());
    plActionContext context;
    context.m_sMapping = "AudioClipAssetMenuBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pMenuBar->SetActionContext(context);
  }

  // Tool Bar
  {
    plQtToolBarActionMapView* pToolBar = new plQtToolBarActionMapView("Toolbar", this);
    plActionContext context;
    context.m_sMapping = "AudioClipAssetToolBar";
    context.m_pDocument = pDocument;
    context.m_pWindow = this;
    pToolBar->SetActionContext(context);
    pToolBar->setObjectName("AudioClipAssetWindowToolBar");
    addToolBar(pToolBar);
  }

  // Central: buttons, platform tabs, waveform, estimates
  {
    QWidget* pCentralWidget = new QWidget(this);
    pCentralWidget->setStyleSheet(ClipToolStyle(m_Accent));

    QVBoxLayout* pCentralLayout = new QVBoxLayout(pCentralWidget);
    pCentralLayout->setContentsMargins(0, 0, 0, 0);
    pCentralLayout->setSpacing(0);

    // button row
    {
      QWidget* pRow = new QWidget(pCentralWidget);
      pRow->setObjectName("ClipButtonRow");

      QHBoxLayout* pLayout = new QHBoxLayout(pRow);
      pLayout->setContentsMargins(6, 5, 6, 5);
      pLayout->setSpacing(6);

      QToolButton* pPlay = new QToolButton(pRow);
      pPlay->setObjectName("PlayButton");
      pPlay->setText(QString::fromUtf8("\xE2\x96\xB6 Play"));
      pPlay->setToolTip("Plays the clip as built for the selected platform tab (codec, rate and normalisation applied)");
      connect(pPlay, &QToolButton::clicked, this, &plAudioClipAssetDocumentWindow::OnPlay);
      pLayout->addWidget(pPlay);

      QToolButton* pStop = new QToolButton(pRow);
      pStop->setText(QString::fromUtf8("\xE2\x96\xA0 Stop"));
      pStop->setToolTip("Stops playback");
      connect(pStop, &QToolButton::clicked, this, &plAudioClipAssetDocumentWindow::OnStop);
      pLayout->addWidget(pStop);

      m_pLoopButton = new QToolButton(pRow);
      m_pLoopButton->setText("Loop region");
      m_pLoopButton->setCheckable(true);
      m_pLoopButton->setChecked(true);
      m_pLoopButton->setToolTip("Repeats the loop region while playing. Drag across the waveform to set the loop, drag the markers to adjust it.");
      pLayout->addWidget(m_pLoopButton);

      pLayout->addSpacing(12);

      m_pInfoLabel = new QLabel(pRow);
      m_pInfoLabel->setObjectName("ClipInfo");
      pLayout->addWidget(m_pInfoLabel);

      pLayout->addStretch();

      m_pLoopLabel = new QLabel(pRow);
      pLayout->addWidget(m_pLoopLabel);

      pCentralLayout->addWidget(pRow);
    }

    // platform tabs
    {
      QWidget* pRow = new QWidget(pCentralWidget);
      pRow->setObjectName("ClipTabRow");

      QHBoxLayout* pLayout = new QHBoxLayout(pRow);
      pLayout->setContentsMargins(6, 4, 6, 0);
      pLayout->setSpacing(4);

      m_pTabBar = new QTabBar(pRow);
      m_pTabBar->setDrawBase(false);
      m_pTabBar->setExpanding(false);
      m_pTabBar->setContextMenuPolicy(Qt::CustomContextMenu);
      m_pTabBar->setToolTip("Default settings and per platform profile overrides. Right-click an override to remove it.");
      connect(m_pTabBar, &QTabBar::currentChanged, this, &plAudioClipAssetDocumentWindow::OnTabChanged);
      connect(m_pTabBar, &QWidget::customContextMenuRequested, this, &plAudioClipAssetDocumentWindow::OnTabContextMenu);
      pLayout->addWidget(m_pTabBar);

      m_pAddTabMenu = new QMenu(pRow);
      connect(m_pAddTabMenu, &QMenu::aboutToShow, this, &plAudioClipAssetDocumentWindow::OnFillAddOverrideMenu);

      m_pAddTabButton = new QToolButton(pRow);
      m_pAddTabButton->setText("+");
      m_pAddTabButton->setToolTip("Adds a codec and quality override for a platform profile");
      m_pAddTabButton->setPopupMode(QToolButton::InstantPopup);
      m_pAddTabButton->setMenu(m_pAddTabMenu);
      pLayout->addWidget(m_pAddTabButton);

      pLayout->addStretch();

      pCentralLayout->addWidget(pRow);
    }

    m_pWaveform = new plQtAudioWaveformWidget(pCentralWidget, pDocument, m_Accent);
    pCentralLayout->addWidget(m_pWaveform, 1);

    // estimates
    {
      QWidget* pRow = new QWidget(pCentralWidget);
      pRow->setObjectName("ClipStatLine");

      QHBoxLayout* pLayout = new QHBoxLayout(pRow);
      pLayout->setContentsMargins(10, 5, 10, 5);
      pLayout->setSpacing(18);

      m_pResidentLabel = new QLabel(pRow);
      m_pResidentLabel->setToolTip("Memory the clip holds while loaded");
      m_pDiskLabel = new QLabel(pRow);
      m_pDiskLabel->setToolTip("Size of the built clip on disk (default language)");
      m_pDecodeLabel = new QLabel(pRow);
      m_pDecodeLabel->setToolTip("CPU work per playing voice");
      m_pLoudnessLabel = new QLabel(pRow);

      pLayout->addWidget(m_pResidentLabel);
      pLayout->addWidget(m_pDiskLabel);
      pLayout->addWidget(m_pDecodeLabel);
      pLayout->addWidget(m_pLoudnessLabel);
      pLayout->addStretch();

      pCentralLayout->addWidget(pRow);
    }

    plQtDocumentPanel* pCentral = new plQtDocumentPanel(this, pDocument);
    pCentral->setObjectName("AudioClipWaveformDockWidget");
    pCentral->setWindowTitle("Audio Clip");
    pCentral->setWidget(pCentralWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->setCentralWidget(pCentral);
  }

  // Right dock: asset status, properties of the selected tab, users of the clip
  {
    plQtDocumentPanel* pPropertyPanel = new plQtDocumentPanel(this, pDocument);
    pPropertyPanel->setObjectName("AudioClipAssetDockWidget");
    pPropertyPanel->setWindowTitle("Clip Properties");
    pPropertyPanel->show();

    QWidget* pWidget = new QWidget();
    pWidget->setObjectName("Group");
    QVBoxLayout* pLayout = new QVBoxLayout(pWidget);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(2);

    pLayout->addWidget(new plQtAssetStatusIndicator((plAssetDocument*)GetDocument()));

    m_pPlatformLabel = new QLabel(pWidget);
    m_pPlatformLabel->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; padding: 6px 6px 2px 6px;").arg(m_Accent.name()));
    pLayout->addWidget(m_pPlatformLabel);

    m_pPropertyGrid = new plQtPropertyGridWidget(pWidget, pDocument, false);
    pLayout->addWidget(m_pPropertyGrid, 1);

    QLabel* pUsedByHeader = new QLabel("USED BY", pWidget);
    pUsedByHeader->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; padding: 6px 6px 2px 6px;").arg(m_Accent.name()));
    pLayout->addWidget(pUsedByHeader);

    m_pUsedByList = new QListWidget(pWidget);
    m_pUsedByList->setMaximumHeight(140);
    m_pUsedByList->setToolTip("Assets that reference this clip. Double-click to open one.");
    connect(m_pUsedByList, &QListWidget::itemDoubleClicked, this, [](QListWidgetItem* pItem)
      {
        const QString sPath = pItem->data(Qt::UserRole).toString();
        if (!sPath.isEmpty())
          plQtEditorApp::GetSingleton()->OpenDocument(sPath.toUtf8().data(), plDocumentFlags::RequestWindow | plDocumentFlags::AddToRecentFilesList);
      });
    pLayout->addWidget(m_pUsedByList);

    pPropertyPanel->setWidget(pWidget, ads::CDockWidget::ForceNoScrollArea);
    m_pDockManager->addDockWidgetTab(ads::RightDockWidgetArea, pPropertyPanel);
  }

  pDocument->GetSelectionManager()->SetSelection(GetPropertiesObject());

  GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::StructureEventHandler, this));
  plAssetCurator::GetSingleton()->m_Events.AddEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::AssetCuratorEventHandler, this));

  m_pTimer = new QTimer(this);
  m_pTimer->setInterval(40);
  connect(m_pTimer, &QTimer::timeout, this, &plAudioClipAssetDocumentWindow::OnTimer);
  m_pTimer->start();

  RebuildTabs();
  RequestPreview(true);

  FinishWindowCreation();
}

plAudioClipAssetDocumentWindow::~plAudioClipAssetDocumentWindow()
{
  plAudioPreviewService::GetSingleton().StopAll();

  if (m_pRunningJob != nullptr)
  {
    plTaskSystem::WaitForGroup(m_RunningTask);
    m_pRunningJob.Clear();
  }

  plAssetCurator::GetSingleton()->m_Events.RemoveEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::AssetCuratorEventHandler, this));
  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plAudioClipAssetDocumentWindow::StructureEventHandler, this));
}

const plDocumentObject* plAudioClipAssetDocumentWindow::GetPropertiesObject() const
{
  const auto& children = GetDocument()->GetObjectManager()->GetRootObject()->GetChildren();
  return children.IsEmpty() ? nullptr : children[0];
}

const plDocumentObject* plAudioClipAssetDocumentWindow::GetActiveOverrideObject() const
{
  if (!m_ActiveOverrideGuid.IsValid())
    return nullptr;

  const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(m_ActiveOverrideGuid);
  if (pObject == nullptr || !pObject->GetParentProperty().IsEqual("PlatformOverrides"))
    return nullptr;

  return pObject;
}

void plAudioClipAssetDocumentWindow::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  if (e.m_sProperty == "Profile")
    m_bTabsDirty = true;

  RequestPreview(false);
}

void plAudioClipAssetDocumentWindow::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  PL_IGNORE_UNUSED(e);
  m_bTabsDirty = true;
  RequestPreview(false);
}

void plAudioClipAssetDocumentWindow::AssetCuratorEventHandler(const plAssetCuratorEvent& e)
{
  switch (e.m_Type)
  {
    case plAssetCuratorEvent::Type::AssetAdded:
    case plAssetCuratorEvent::Type::AssetRemoved:
    case plAssetCuratorEvent::Type::AssetMoved:
    case plAssetCuratorEvent::Type::AssetUpdated:
    case plAssetCuratorEvent::Type::AssetListReset:
      m_bUsedByDirty = true;
      break;

    default:
      break;
  }
}

void plAudioClipAssetDocumentWindow::RebuildTabs()
{
  m_bTabsDirty = false;
  m_bUpdatingTabs = true;

  while (m_pTabBar->count() > 0)
    m_pTabBar->removeTab(0);

  m_TabOverrides.Clear();

  m_pTabBar->addTab("Default");
  m_TabOverrides.PushBack(plUuid());

  int iSelect = 0;

  if (const plDocumentObject* pProps = GetPropertiesObject())
  {
    for (const plDocumentObject* pChild : pProps->GetChildren())
    {
      if (!pChild->GetParentProperty().IsEqual("PlatformOverrides"))
        continue;

      const plString sProfile = pChild->GetTypeAccessor().GetValue("Profile").ConvertTo<plString>();
      const int iTab = m_pTabBar->addTab(sProfile.IsEmpty() ? QString("(no profile)") : QString::fromUtf8(sProfile.GetData()));
      m_TabOverrides.PushBack(pChild->GetGuid());

      if (pChild->GetGuid() == m_ActiveOverrideGuid)
        iSelect = iTab;
    }
  }

  if (iSelect == 0)
    m_ActiveOverrideGuid = plUuid();

  m_pTabBar->setCurrentIndex(iSelect);
  m_bUpdatingTabs = false;

  UpdatePropertyGridSelection();
}

void plAudioClipAssetDocumentWindow::OnTabChanged(int iIndex)
{
  if (m_bUpdatingTabs || iIndex < 0 || iIndex >= static_cast<int>(m_TabOverrides.GetCount()))
    return;

  m_ActiveOverrideGuid = m_TabOverrides[iIndex];
  UpdatePropertyGridSelection();
  RequestPreview(true);
}

void plAudioClipAssetDocumentWindow::UpdatePropertyGridSelection()
{
  const plDocumentObject* pOverride = GetActiveOverrideObject();
  const plDocumentObject* pObject = pOverride != nullptr ? pOverride : GetPropertiesObject();

  plDeque<const plDocumentObject*> selection;
  if (pObject != nullptr)
    selection.PushBack(pObject);
  m_pPropertyGrid->SetSelection(selection);

  if (pOverride == nullptr)
  {
    m_pPlatformLabel->setText(QString::fromUtf8("PLATFORM \xC2\xB7 Default"));
    return;
  }

  const plString sProfile = pOverride->GetTypeAccessor().GetValue("Profile").ConvertTo<plString>();

  bool bKnown = false;
  for (plUInt32 i = 0; i < plAssetCurator::GetSingleton()->GetNumAssetProfiles(); ++i)
  {
    if (plAssetCurator::GetSingleton()->GetAssetProfile(i)->GetConfigName().IsEqual_NoCase(sProfile))
      bKnown = true;
  }

  QString sText = QString::fromUtf8("PLATFORM \xC2\xB7 ") + QString::fromUtf8(sProfile.GetData());
  if (!bKnown)
    sText += " (no such asset profile)";
  m_pPlatformLabel->setText(sText);
}

void plAudioClipAssetDocumentWindow::OnFillAddOverrideMenu()
{
  m_pAddTabMenu->clear();

  const plAudioClipAssetProperties* pProps = m_pAssetDoc->GetProperties();

  for (plUInt32 i = 0; i < plAssetCurator::GetSingleton()->GetNumAssetProfiles(); ++i)
  {
    const plString sProfile = plAssetCurator::GetSingleton()->GetAssetProfile(i)->GetConfigName();

    bool bExists = false;
    for (const plAudioClipPlatformOverride& o : pProps->m_PlatformOverrides)
    {
      if (o.m_sProfile.IsEqual_NoCase(sProfile))
        bExists = true;
    }

    QAction* pAction = m_pAddTabMenu->addAction(QString::fromUtf8(sProfile.GetData()));
    pAction->setEnabled(!bExists);

    connect(pAction, &QAction::triggered, this, [this, sProfile]()
      {
        const plDocumentObject* pProps = GetPropertiesObject();
        if (pProps == nullptr)
          return;

        plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
        accessor.StartTransaction("Add Platform Override");

        plUuid guid;
        if (accessor.AddObjectByName(pProps, "PlatformOverrides", -1, plGetStaticRTTI<plAudioClipPlatformOverride>(), guid).Failed())
        {
          accessor.CancelTransaction();
          return;
        }

        const plDocumentObject* pOverride = GetDocument()->GetObjectManager()->GetObject(guid);
        accessor.SetValueByName(pOverride, "Profile", sProfile).IgnoreResult();

        // start from the default settings
        for (const char* szProperty : {"LoadMode", "Codec", "OpusKbps", "SampleRate"})
        {
          accessor.SetValueByName(pOverride, szProperty, pProps->GetTypeAccessor().GetValue(szProperty)).IgnoreResult();
        }

        accessor.FinishTransaction();

        m_ActiveOverrideGuid = guid;
        RebuildTabs();
        RequestPreview(true);
      });
  }

  if (m_pAddTabMenu->isEmpty())
  {
    m_pAddTabMenu->addAction("No asset profiles")->setEnabled(false);
  }
}

void plAudioClipAssetDocumentWindow::OnTabContextMenu(const QPoint& pos)
{
  const int iTab = m_pTabBar->tabAt(pos);
  if (iTab <= 0 || iTab >= static_cast<int>(m_TabOverrides.GetCount()))
    return;

  const plUuid guid = m_TabOverrides[iTab];

  QMenu menu(this);
  QAction* pRemove = menu.addAction("Remove Override");
  if (menu.exec(m_pTabBar->mapToGlobal(pos)) != pRemove)
    return;

  const plDocumentObject* pOverride = GetDocument()->GetObjectManager()->GetObject(guid);
  if (pOverride == nullptr)
    return;

  plObjectCommandAccessor accessor(GetDocument()->GetCommandHistory());
  accessor.StartTransaction("Remove Platform Override");
  accessor.RemoveObject(pOverride).IgnoreResult();
  accessor.FinishTransaction();

  RebuildTabs();
  RequestPreview(true);
}

void plAudioClipAssetDocumentWindow::RequestPreview(bool bImmediate)
{
  m_bPreviewRequested = true;
  m_PreviewDue = plTime::Now() + (bImmediate ? plTime::MakeZero() : plTime::MakeFromMilliseconds(300));
}

void plAudioClipAssetDocumentWindow::OnTimer()
{
  if (m_bTabsDirty)
    RebuildTabs();

  if (m_bUsedByDirty)
    RefreshUsedBy();

  if (m_pRunningJob != nullptr && m_pRunningJob->m_bDone)
  {
    plSharedPtr<plAudioClipPreview> pResult = m_pRunningJob->m_pResult;
    m_pRunningJob.Clear();

    if (m_PreviewCache.GetCount() >= kPreviewCacheSize)
      m_PreviewCache.RemoveAtAndCopy(0);
    m_PreviewCache.PushBack(pResult);

    if (pResult->m_uiHash == m_uiWantedHash)
      ShowPreview(pResult);
  }

  StartPreviewIfNeeded();

  m_pWaveform->SetPlayCursor(plAudioPreviewService::GetSingleton().GetPlayCursorFrames());
}

void plAudioClipAssetDocumentWindow::StartPreviewIfNeeded()
{
  if (!m_bPreviewRequested || plTime::Now() < m_PreviewDue || m_pRunningJob != nullptr)
    return;

  m_bPreviewRequested = false;

  const plAudioClipAssetProperties* pProps = m_pAssetDoc->GetProperties();

  auto ShowMessage = [this](const QString& sMessage)
  {
    m_uiWantedHash = 0;
    m_pShownPreview.Clear();
    m_pWaveform->SetPreview(nullptr);
    m_pWaveform->SetMessage(sMessage);
    ShowPreview(nullptr);
  };

  if (pProps->m_sSourceFile.IsEmpty())
  {
    ShowMessage("Select a source file (WAV, MP3 or FLAC) in the properties.");
    return;
  }

  plStringBuilder sAbsPath = pProps->m_sSourceFile;
  plFileStats stats;
  if (!plQtEditorApp::GetSingleton()->MakeDataDirectoryRelativePathAbsolute(sAbsPath) || plOSFile::GetFileStats(sAbsPath, stats).Failed())
  {
    ShowMessage(QString("Source file not found: ") + QString::fromUtf8(pProps->m_sSourceFile.GetData()));
    return;
  }

  plAudioClipAssetMetaInfo info;
  if (plAudioClipAssetUtils::ReadSourceInfo(sAbsPath, info).Failed())
  {
    ShowMessage("The source file could not be read.");
    return;
  }

  // the override and the limits of the selected tab's profile
  const plAudioClipPlatformOverride* pOverride = nullptr;
  const plPlatformProfile* pProfile = nullptr;

  if (const plDocumentObject* pOverrideObject = GetActiveOverrideObject())
  {
    const plInt32 iIndex = pOverrideObject->GetPropertyIndex().ConvertTo<plInt32>();
    if (iIndex >= 0 && iIndex < static_cast<plInt32>(pProps->m_PlatformOverrides.GetCount()))
    {
      pOverride = &pProps->m_PlatformOverrides[iIndex];

      for (plUInt32 i = 0; i < plAssetCurator::GetSingleton()->GetNumAssetProfiles(); ++i)
      {
        if (plAssetCurator::GetSingleton()->GetAssetProfile(i)->GetConfigName().IsEqual_NoCase(pOverride->m_sProfile))
          pProfile = plAssetCurator::GetSingleton()->GetAssetProfile(i);
      }
    }
  }

  const plAudioClipBuildSettings settings = plAudioClipAssetUtils::MakeBuildSettings(*pProps, pOverride, pProfile, info.m_uiSampleRate);
  const plUInt64 uiHash = HashSettings(settings, sAbsPath, stats.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond));

  m_uiWantedHash = uiHash;

  for (const auto& pCached : m_PreviewCache)
  {
    if (pCached->m_uiHash == uiHash)
    {
      ShowPreview(pCached);
      return;
    }
  }

  plSharedPtr<plAudioClipPreview> pResult = PL_DEFAULT_NEW(plAudioClipPreview);
  pResult->m_uiHash = uiHash;
  pResult->m_Settings = settings;

  if (m_pShownPreview == nullptr)
    m_pWaveform->SetMessage("Building preview...");

  m_pRunningJob = PL_DEFAULT_NEW(plAudioClipPreviewJob);
  m_pRunningJob->m_sAbsSourceFile = sAbsPath;
  m_pRunningJob->m_Settings = settings;
  m_pRunningJob->m_pResult = pResult;

  plSharedPtr<plAudioClipPreviewJob> pJob = m_pRunningJob;
  plSharedPtr<plDelegateTask<void>> pTask = PL_DEFAULT_NEW(plDelegateTask<void>, "AudioClipPreview", plTaskNesting::Never, [pJob]()
    { pJob->Run(); });
  m_RunningTask = plTaskSystem::StartSingleTask(pTask, plTaskPriority::LongRunning);
}

void plAudioClipAssetDocumentWindow::ShowPreview(const plSharedPtr<plAudioClipPreview>& pPreview)
{
  m_pShownPreview = pPreview;

  if (pPreview != nullptr)
    m_pWaveform->SetPreview(pPreview);

  UpdateLoopLabel();

  const QString sDot = QString::fromUtf8(" \xC2\xB7 ");

  if (pPreview == nullptr || !pPreview->m_bSuccess)
  {
    m_pInfoLabel->setText(pPreview != nullptr ? QString::fromUtf8(pPreview->m_sError.GetData()) : QString());
    m_pResidentLabel->setText("Resident <b>-</b>");
    m_pDiskLabel->setText("Disk <b>-</b>");
    m_pDecodeLabel->setText("Decode cost <b>-</b>");
    m_pLoudnessLabel->setText("Loudness <b>-</b>");
    return;
  }

  const plAudioClipHeader& header = pPreview->m_Header;
  const double fDuration = header.m_uiSampleRate > 0 ? static_cast<double>(header.m_uiFrames) / header.m_uiSampleRate : 0.0;
  const float fLufs = header.m_fMeasuredLufs + header.m_fAppliedGainDb;

  QString sChannels = header.m_uiChannels == 1 ? QString("mono") : header.m_uiChannels == 2 ? QString("stereo")
                                                                                             : QString::asprintf("%u ch", header.m_uiChannels);

  QString sInfo = QString::asprintf("%.1f kHz", header.m_uiSampleRate / 1000.0) + sDot + sChannels + sDot + QString::asprintf("%.2f s", fDuration);
  if (plMath::IsFinite(fLufs))
    sInfo += sDot + QString::asprintf("%.1f LUFS", fLufs);
  sInfo += sDot + CodecName(header.m_eCodec) + sDot + LoadModeName(header.m_eLoadMode);
  m_pInfoLabel->setText(sInfo);

  // estimates
  const plAudioClipLanguageEntry& lang = header.m_Languages[0];
  const plUInt64 uiPcmBytes = header.m_uiFrames * header.m_uiChannels * sizeof(plInt16);

  plUInt64 uiResident = 0;
  switch (header.m_eLoadMode)
  {
    case plAudioClipLoadMode::DecompressOnLoad:
      uiResident = uiPcmBytes;
      break;
    case plAudioClipLoadMode::CompressedInMemory:
      uiResident = lang.m_uiPayloadBytes;
      break;
    case plAudioClipLoadMode::Streamed:
      uiResident = kStreamRingBytes + lang.m_uiPrimingBytes + lang.m_uiLoopPrimingBytes;
      break;
  }

  QString sDecode;
  if (header.m_eLoadMode == plAudioClipLoadMode::DecompressOnLoad)
  {
    sDecode = header.m_eCodec == plAudioClipCodec::Pcm16 ? QString("none") : QString("once on load (%1)").arg(CodecName(header.m_eCodec));
  }
  else
  {
    switch (header.m_eCodec)
    {
      case plAudioClipCodec::Pcm16:
        sDecode = "none";
        break;
      case plAudioClipCodec::ImaAdpcm:
        sDecode = "low (ADPCM per voice)";
        break;
      case plAudioClipCodec::Opus:
        sDecode = "high (Opus per voice)";
        break;
    }

    if (header.m_eLoadMode == plAudioClipLoadMode::Streamed)
      sDecode += " + disk stream";
  }

  QString sStreamNote = header.m_eLoadMode == plAudioClipLoadMode::Streamed ? QString(" per voice") : QString();
  m_pResidentLabel->setText(QString("Resident <b>%1</b>%2").arg(FormatBytes(uiResident)).arg(sStreamNote));
  m_pDiskLabel->setText(QString("Disk <b>%1</b>").arg(FormatBytes(pPreview->m_uiFileBytes)));
  m_pDecodeLabel->setText(QString("Decode cost <b>%1</b>").arg(sDecode));

  QString sLoudness;
  if (!plMath::IsFinite(header.m_fMeasuredLufs))
    sLoudness = QString::asprintf("too short to measure (gain %+.1f dB)", header.m_fAppliedGainDb);
  else if (pPreview->m_Settings.m_fNormaliseLufs != 0.0f)
    sLoudness = QString::asprintf("normalised to %.1f LUFS (gain %+.1f dB)", fLufs, header.m_fAppliedGainDb);
  else
    sLoudness = QString::asprintf("%.1f LUFS (not normalised)", header.m_fMeasuredLufs);
  m_pLoudnessLabel->setText(QString("Loudness <b>%1</b>").arg(sLoudness));
}

void plAudioClipAssetDocumentWindow::UpdateLoopLabel()
{
  if (m_pShownPreview == nullptr || !m_pShownPreview->m_bSuccess || !m_pShownPreview->m_Header.m_bHasLoop || m_pShownPreview->m_Header.m_uiSampleRate == 0)
  {
    m_pLoopLabel->setText("no loop");
    return;
  }

  const plAudioClipHeader& header = m_pShownPreview->m_Header;
  m_pLoopLabel->setText(QString::asprintf("loop %.2f s - %.2f s", static_cast<double>(header.m_uiLoopStart) / header.m_uiSampleRate, static_cast<double>(header.m_uiLoopEnd) / header.m_uiSampleRate));
}

void plAudioClipAssetDocumentWindow::RefreshUsedBy()
{
  m_bUsedByDirty = false;
  m_pUsedByList->clear();

  plSet<plUuid> uses;
  plAssetCurator::GetSingleton()->FindAllUses(GetDocument()->GetGuid(), uses, false);

  for (const plUuid& guid : uses)
  {
    plAssetCurator::plLockedSubAsset pSubAsset = plAssetCurator::GetSingleton()->GetSubAsset(guid);
    if (pSubAsset == nullptr || pSubAsset->m_pAssetInfo == nullptr)
      continue;

    QListWidgetItem* pItem = new QListWidgetItem(QString::fromUtf8(pSubAsset->m_pAssetInfo->m_Path.GetDataDirParentRelativePath().GetStartPointer(), static_cast<int>(pSubAsset->m_pAssetInfo->m_Path.GetDataDirParentRelativePath().GetElementCount())));
    pItem->setData(Qt::UserRole, QString::fromUtf8(pSubAsset->m_pAssetInfo->m_Path.GetAbsolutePath().GetData()));
    m_pUsedByList->addItem(pItem);
  }

  if (m_pUsedByList->count() == 0)
  {
    QListWidgetItem* pItem = new QListWidgetItem("No asset references this clip");
    pItem->setFlags(Qt::NoItemFlags);
    m_pUsedByList->addItem(pItem);
  }
}

void plAudioClipAssetDocumentWindow::OnPlay()
{
  const plSharedPtr<plAudioClipPreview>& pPreview = m_pShownPreview;
  if (pPreview == nullptr || !pPreview->m_bSuccess || pPreview->m_Pcm.IsEmpty())
    return;

  const plAudioClipHeader& header = pPreview->m_Header;
  const bool bLoop = m_pLoopButton->isChecked() && header.m_bHasLoop;

  plAudioPreviewService::GetSingleton().PlayPcm(pPreview->m_Pcm.GetData(), header.m_uiFrames, header.m_uiChannels, header.m_uiSampleRate, header.m_uiLoopStart, header.m_uiLoopEnd, bLoop).IgnoreResult();
}

void plAudioClipAssetDocumentWindow::OnStop()
{
  plAudioPreviewService::GetSingleton().StopAll();
}
